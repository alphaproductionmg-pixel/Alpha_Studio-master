#include "AlphaStudio/Core/DSP/AudioWarpAndSampleEditor.hpp"
#include "AlphaStudio/Core/Engine/AudioPool.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// AudioWarpAndMPEX (Transient-Preserving WSOLA + Windowed-Sinc Resampling)
// ============================================================================
std::vector<float> AudioWarpAndMPEX::processStretchAndPitch(
    std::span<const float> interleavedIn,
    std::uint32_t channels,
    double sampleRate,
    double timeStretchRatio,
    double pitchShiftSemitones,
    MPEXQualityAlgorithm algorithm) {
    if (interleavedIn.empty() || channels == 0 || sampleRate <= 0.0) {
        return {};
    }

    const double pitchRatio = std::pow(2.0, std::clamp(pitchShiftSemitones, -24.0, 24.0) / 12.0);
    // To achieve independent timeStretchRatio AND pitchShiftSemitones:
    // Step 1: Time-stretch by (timeStretchRatio * pitchRatio) preserving transients
    // Step 2: Resample by (1.0 / pitchRatio) via high-order windowed-sinc
    const double internalStretch = std::clamp(timeStretchRatio * pitchRatio, 0.25, 4.0);
    const std::size_t inFrames = interleavedIn.size() / channels;
    const auto stretchedFrames = static_cast<std::size_t>(
        std::max<long long>(1LL, std::llround(static_cast<double>(inFrames) * internalStretch)));

    const std::size_t winFrames = (algorithm == MPEXQualityAlgorithm::MPEX_PolyComplex) ? 2048U : 1024U;
    const std::size_t synthHop = winFrames / 2U;
    const std::size_t searchTolerance = (algorithm == MPEXQualityAlgorithm::MPEX_SoloFast) ? 64U : 192U;

    std::vector<float> stretched(stretchedFrames * channels, 0.0f);
    std::vector<float> normWeights(stretchedFrames, 0.0f);
    std::vector<float> hann(winFrames);
    for (std::size_t k = 0; k < winFrames; ++k) {
        hann[k] = static_cast<float>(
            0.5 * (1.0 - std::cos((2.0 * std::numbers::pi * static_cast<double>(k)) /
                                  static_cast<double>(winFrames - 1))));
    }

    std::size_t prevSrcPos = 0;
    float prevRms = 0.0f;

    for (std::size_t outPos = 0; outPos < stretchedFrames; outPos += synthHop) {
        const auto idealSrc = static_cast<std::size_t>(std::min<double>(
            static_cast<double>(inFrames > winFrames ? inFrames - winFrames : 0),
            static_cast<double>(outPos) / internalStretch));

        // Detect sharp percussive transient onset: lock phase directly to idealSrc without smear!
        float localRms = 0.0f;
        const std::size_t checkLen = std::min<std::size_t>(256U, inFrames - idealSrc);
        for (std::size_t f = 0; f < checkLen; ++f) {
            const float s = interleavedIn[(idealSrc + f) * channels];
            localRms += s * s;
        }
        localRms = (checkLen > 0) ? std::sqrt(localRms / static_cast<float>(checkLen)) : 0.0f;
        const bool isSharpTransient = (localRms > prevRms * 2.4f && localRms > 0.04f);
        prevRms = localRms * 0.7f + prevRms * 0.3f;

        std::size_t bestSrc = idealSrc;
        if (!isSharpTransient && outPos > 0 && prevSrcPos + synthHop + winFrames < inFrames) {
            // WSOLA waveform similarity cross-correlation search for glitch-free tonal continuity
            const std::size_t naturalPred = prevSrcPos + synthHop;
            const std::size_t lo = (idealSrc > searchTolerance) ? (idealSrc - searchTolerance) : 0;
            const std::size_t hi = std::min(
                inFrames > winFrames ? (inFrames - winFrames) : 0, idealSrc + searchTolerance);

            double bestCorr = -1e30;
            for (std::size_t cand = lo; cand <= hi; cand += 4) {
                double corr = 0.0;
                for (std::size_t k = 0; k < std::min<std::size_t>(256U, winFrames); k += 2) {
                    corr += static_cast<double>(interleavedIn[(naturalPred + k) * channels]) *
                            static_cast<double>(interleavedIn[(cand + k) * channels]);
                }
                if (corr > bestCorr) {
                    bestCorr = corr;
                    bestSrc = cand;
                }
            }
        }
        prevSrcPos = bestSrc;

        for (std::size_t k = 0; k < winFrames && (outPos + k) < stretchedFrames && (bestSrc + k) < inFrames; ++k) {
            const float w = hann[k];
            for (std::uint32_t c = 0; c < channels; ++c) {
                stretched[(outPos + k) * channels + c] += interleavedIn[(bestSrc + k) * channels + c] * w;
            }
            normWeights[outPos + k] += w;
        }
    }

    for (std::size_t f = 0; f < stretchedFrames; ++f) {
        const float invW = (normWeights[f] > 1e-5f) ? (1.0f / normWeights[f]) : 1.0f;
        for (std::uint32_t c = 0; c < channels; ++c) {
            stretched[f * channels + c] *= invW;
        }
    }

    if (std::abs(pitchRatio - 1.0) < 1e-4) {
        return stretched;
    }
    return AudioPool::convertSampleRateWindowedSinc(
        stretched, channels, sampleRate * pitchRatio, sampleRate);
}

std::vector<float> AudioWarpAndMPEX::applyWarpMarkers(
    std::span<const float> interleavedIn,
    std::uint32_t channels,
    double sampleRate,
    double targetTempoBpm,
    std::span<const WarpMarker> markers) {
    if (markers.size() < 2 || interleavedIn.empty() || channels == 0 || targetTempoBpm <= 0.0) {
        return std::vector<float>(interleavedIn.begin(), interleavedIn.end());
    }

    std::vector<float> result;
    const std::size_t totalFrames = interleavedIn.size() / channels;

    for (std::size_t i = 0; i + 1 < markers.size(); ++i) {
        const std::size_t f0 = std::min(markers[i].sampleFramePosition, totalFrames);
        const std::size_t f1 = std::min(markers[i + 1].sampleFramePosition, totalFrames);
        if (f1 <= f0) continue;

        const double beats = std::max(0.01, markers[i + 1].musicalBeatPosition - markers[i].musicalBeatPosition);
        const double targetDurationSec = (beats * 60.0) / targetTempoBpm;
        const double srcDurationSec = static_cast<double>(f1 - f0) / sampleRate;
        const double segRatio = targetDurationSec / std::max(1e-6, srcDurationSec);

        const auto segSpan = interleavedIn.subspan(f0 * channels, (f1 - f0) * channels);
        auto warpedSeg = processStretchAndPitch(
            segSpan, channels, sampleRate, segRatio, 0.0, MPEXQualityAlgorithm::RealtimeMusicalStandard);
        result.insert(result.end(), warpedSeg.begin(), warpedSeg.end());
    }
    return result;
}

// ============================================================================
// SampleEditorEngine
// ============================================================================
void SampleEditorEngine::loadSampleBuffer(std::span<const float> interleaved,
                                          std::uint32_t channels, double sampleRate) {
    interleaved_.assign(interleaved.begin(), interleaved.end());
    channels_ = std::max(1U, channels);
    sampleRate_ = std::max(8000.0, sampleRate);
    hitpoints_.clear();
    selectedHitpointIdx_ = 0;
}

std::size_t SampleEditorEngine::snapToNearestZeroCrossing(std::size_t framePos) const noexcept {
    const std::size_t totalFrames = interleaved_.size() / std::max(1U, channels_);
    if (totalFrames < 4 || framePos >= totalFrames) {
        return std::min(framePos, totalFrames);
    }
    constexpr std::size_t kSearchRadius = 128;
    const std::size_t lo = (framePos > kSearchRadius) ? (framePos - kSearchRadius) : 1;
    const std::size_t hi = std::min(totalFrames - 1, framePos + kSearchRadius);

    std::size_t best = framePos;
    std::size_t bestDist = kSearchRadius + 1;
    for (std::size_t f = lo; f < hi; ++f) {
        const float s0 = interleaved_[(f - 1) * channels_];
        const float s1 = interleaved_[f * channels_];
        if ((s0 <= 0.0f && s1 >= 0.0f) || (s0 >= 0.0f && s1 <= 0.0f)) {
            const std::size_t d = (f > framePos) ? (f - framePos) : (framePos - f);
            if (d < bestDist) {
                bestDist = d;
                best = f;
            }
        }
    }
    return best;
}

std::size_t SampleEditorEngine::detectHitpoints(float threshold01, double minIntervalMs) {
    hitpoints_.clear();
    const std::size_t totalFrames = interleaved_.size() / channels_;
    if (totalFrames < 256) {
        return 0;
    }

    constexpr std::size_t kHop = 128;
    std::vector<float> envFlux;
    float prevEnergy = 0.0f;
    float maxFlux = 1e-9f;

    for (std::size_t f = 0; f + kHop <= totalFrames; f += kHop) {
        float energy = 0.0f;
        for (std::size_t k = 0; k < kHop; ++k) {
            for (std::uint32_t c = 0; c < channels_; ++c) {
                const float s = interleaved_[(f + k) * channels_ + c];
                energy += std::abs(s);
            }
        }
        energy /= static_cast<float>(kHop * channels_);
        const float positiveFlux = std::max(0.0f, energy - prevEnergy);
        prevEnergy = energy;
        maxFlux = std::max(maxFlux, positiveFlux);
        envFlux.push_back(positiveFlux);
    }

    const auto minGapFrames = static_cast<std::size_t>((std::max(5.0, minIntervalMs) * 0.001) * sampleRate_);
    const float normThresh = std::clamp(threshold01, 0.02f, 0.98f);
    std::size_t lastHitFrame = 0;
    bool hasFirst = false;

    for (std::size_t i = 1; i + 1 < envFlux.size(); ++i) {
        const float strength = envFlux[i] / maxFlux;
        if (strength >= normThresh && envFlux[i] >= envFlux[i - 1] && envFlux[i] >= envFlux[i + 1]) {
            const std::size_t candidateFrame = snapToNearestZeroCrossing(i * kHop);
            if (!hasFirst || (candidateFrame >= lastHitFrame + minGapFrames)) {
                hitpoints_.push_back(HitpointMarker{candidateFrame, strength, false});
                lastHitFrame = candidateFrame;
                hasFirst = true;
            }
        }
    }
    selectedHitpointIdx_ = 0;
    return hitpoints_.size();
}

std::vector<AudioSliceRegion> SampleEditorEngine::createSlicesFromHitpoints(
    std::size_t declickFadeFrames) const {
    std::vector<AudioSliceRegion> slices;
    const std::size_t totalFrames = interleaved_.size() / channels_;
    if (totalFrames == 0) {
        return slices;
    }

    std::vector<std::size_t> boundaries;
    if (hitpoints_.empty() || hitpoints_.front().framePosition > 0) {
        boundaries.push_back(0);
    }
    for (const auto& hp : hitpoints_) {
        boundaries.push_back(std::min(hp.framePosition, totalFrames));
    }
    boundaries.push_back(totalFrames);

    const double totalBeats =
        static_cast<double>(definitionRuler_.numBars * definitionRuler_.numerator);

    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const std::size_t s = boundaries[i];
        const std::size_t e = boundaries[i + 1];
        if (e <= s) continue;

        const std::size_t len = e - s;
        AudioSliceRegion reg;
        reg.sliceIndex = static_cast<std::uint32_t>(slices.size());
        reg.startFrame = s;
        reg.endFrame = e;
        reg.musicalStartBeat = (static_cast<double>(s) / static_cast<double>(totalFrames)) * totalBeats;
        reg.interleavedSliceSamples.assign(
            interleaved_.begin() + static_cast<std::ptrdiff_t>(s * channels_),
            interleaved_.begin() + static_cast<std::ptrdiff_t>(e * channels_));

        const std::size_t fade = std::min(declickFadeFrames, len / 4U);
        for (std::size_t k = 0; k < fade; ++k) {
            const float g = static_cast<float>(k) / static_cast<float>(std::max<std::size_t>(1, fade));
            for (std::uint32_t c = 0; c < channels_; ++c) {
                reg.interleavedSliceSamples[k * channels_ + c] *= g;
                reg.interleavedSliceSamples[(len - 1 - k) * channels_ + c] *= g;
            }
        }
        slices.push_back(std::move(reg));
    }
    return slices;
}

std::size_t SampleEditorEngine::selectNextHitpoint() noexcept {
    if (!hitpoints_.empty() && selectedHitpointIdx_ + 1 < hitpoints_.size()) {
        ++selectedHitpointIdx_;
    }
    return selectedHitpointIdx_;
}

std::size_t SampleEditorEngine::selectPreviousHitpoint() noexcept {
    if (!hitpoints_.empty() && selectedHitpointIdx_ > 0) {
        --selectedHitpointIdx_;
    }
    return selectedHitpointIdx_;
}

void SampleEditorEngine::configureDefinitionRuler(std::uint32_t bars,
                                                  std::uint32_t num,
                                                  std::uint32_t den,
                                                  std::size_t startOffsetFrames) noexcept {
    definitionRuler_.definitionModeActive = true;
    definitionRuler_.numBars = std::max(1U, bars);
    definitionRuler_.numerator = std::max(1U, num);
    definitionRuler_.denominator = std::max(1U, den);
    definitionRuler_.gridStartOffsetFrames = startOffsetFrames;

    const std::size_t totalFrames = interleaved_.size() / std::max(1U, channels_);
    const std::size_t activeFrames = (totalFrames > startOffsetFrames)
        ? (totalFrames - startOffsetFrames)
        : totalFrames;
    const double seconds = static_cast<double>(activeFrames) / std::max(1.0, sampleRate_);
    const double quarterNotes =
        static_cast<double>(definitionRuler_.numBars * definitionRuler_.numerator) *
        (4.0 / static_cast<double>(definitionRuler_.denominator));
    definitionRuler_.inferredTempoBpm = (seconds > 1e-6) ? ((quarterNotes * 60.0) / seconds) : 120.0;
}

} // namespace AlphaStudio::Core::DSP
