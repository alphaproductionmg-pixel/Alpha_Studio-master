#include "AlphaStudio/Core/DSP/VariAudioAndPitch.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// VariAudioEngine (Clean-Room YIN + Segmentation + TD-PSOLA + Audio-to-MIDI)
// ============================================================================
VariAudioEngine::VariAudioEngine(double sampleRate)
    : sampleRate_(std::max(8000.0, sampleRate)) {}

float VariAudioEngine::detectPitchYIN(std::span<const float> windowSamples,
                                      double sampleRate,
                                      float yinThreshold,
                                      float* outConfidence) noexcept {
    const std::size_t N = windowSamples.size();
    const std::size_t halfN = N / 2;
    if (halfN < 32 || sampleRate <= 0.0) {
        if (outConfidence) *outConfidence = 0.0f;
        return 0.0f;
    }

    const std::size_t minTau = std::max<std::size_t>(2, static_cast<std::size_t>(sampleRate / 1200.0));
    const std::size_t maxTau = std::min<std::size_t>(halfN - 1, static_cast<std::size_t>(sampleRate / 55.0));
    if (minTau >= maxTau) {
        if (outConfidence) *outConfidence = 0.0f;
        return 0.0f;
    }

    // Stack-allocated CMNDF buffer (zero heap allocation for RT callers up to N=4096)
    std::array<float, 2048> cmndf{};
    const std::size_t tauLimit = std::min<std::size_t>(maxTau + 1, cmndf.size());
    cmndf[0] = 1.0f;
    double runningSum = 0.0;

    for (std::size_t tau = 1; tau < tauLimit; ++tau) {
        double diff = 0.0;
        for (std::size_t j = 0; j < halfN; ++j) {
            const double d = static_cast<double>(windowSamples[j]) -
                             static_cast<double>(windowSamples[j + tau]);
            diff += d * d;
        }
        runningSum += diff;
        cmndf[tau] = (runningSum > 1e-12)
            ? static_cast<float>((diff * static_cast<double>(tau)) / runningSum)
            : 1.0f;
    }

    std::size_t bestTau = 0;
    for (std::size_t tau = minTau; tau + 1 < tauLimit; ++tau) {
        if (cmndf[tau] < yinThreshold) {
            while (tau + 1 < tauLimit && cmndf[tau + 1] < cmndf[tau]) {
                ++tau;
            }
            bestTau = tau;
            break;
        }
    }

    if (bestTau == 0) {
        float minVal = 1.0f;
        for (std::size_t tau = minTau; tau < tauLimit; ++tau) {
            if (cmndf[tau] < minVal) {
                minVal = cmndf[tau];
                bestTau = tau;
            }
        }
        if (minVal > 0.45f) {
            if (outConfidence) *outConfidence = 0.0f;
            return 0.0f;
        }
    }

    // Parabolic sub-sample tau refinement
    double refinedTau = static_cast<double>(bestTau);
    if (bestTau > 1 && bestTau + 1 < tauLimit) {
        const double s0 = cmndf[bestTau - 1];
        const double s1 = cmndf[bestTau];
        const double s2 = cmndf[bestTau + 1];
        const double denom = 2.0 * (2.0 * s1 - s2 - s0);
        if (std::abs(denom) > 1e-9) {
            refinedTau += (s2 - s0) / denom;
        }
    }

    if (outConfidence) {
        *outConfidence = std::clamp(1.0f - cmndf[bestTau], 0.0f, 1.0f);
    }
    return (refinedTau > 1.0) ? static_cast<float>(sampleRate / refinedTau) : 0.0f;
}

void VariAudioEngine::analyzeAudioClip(std::span<const float> monoSamples) {
    segments_.clear();
    constexpr std::size_t kWinSize = 2048;
    constexpr std::size_t kHopSize = 512;
    if (monoSamples.size() < kWinSize) {
        return;
    }

    struct FrameEst {
        std::size_t frame{0};
        float hz{0.0f};
        float midi{0.0f};
        bool voiced{false};
    };

    std::vector<FrameEst> frames;
    for (std::size_t pos = 0; pos + kWinSize <= monoSamples.size(); pos += kHopSize) {
        float conf = 0.0f;
        const float hz = detectPitchYIN(monoSamples.subspan(pos, kWinSize), sampleRate_, 0.16f, &conf);
        FrameEst fe;
        fe.frame = pos;
        fe.hz = hz;
        fe.voiced = (hz > 50.0f && conf > 0.55f);
        fe.midi = fe.voiced ? (69.0f + 12.0f * std::log2(hz / 440.0f)) : 0.0f;
        frames.push_back(fe);
    }

    // Group contiguous voiced frames within 0.65 semitones into editable VariAudioSegments
    std::uint32_t nextId = 1;
    std::size_t i = 0;
    while (i < frames.size()) {
        if (!frames[i].voiced) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        double midiSum = frames[i].midi;
        while (j < frames.size() && frames[j].voiced) {
            const double meanMidi = midiSum / static_cast<double>(j - i);
            if (std::abs(static_cast<double>(frames[j].midi) - meanMidi) > 0.65) {
                break;
            }
            midiSum += frames[j].midi;
            ++j;
        }

        if (j - i >= 2) {
            const float avgMidi = static_cast<float>(midiSum / static_cast<double>(j - i));
            const auto roundedSemi = static_cast<std::int32_t>(std::lround(avgMidi));
            VariAudioSegment seg;
            seg.segmentId = nextId++;
            seg.startFrame = frames[i].frame;
            seg.endFrame = std::min(monoSamples.size(), frames[j - 1].frame + kHopSize);
            seg.detectedMidiPitch = avgMidi;
            seg.targetMidiNote = std::clamp(roundedSemi, 0, 127);
            seg.microPitchCents = (avgMidi - static_cast<float>(roundedSemi)) * 100.0f;
            for (std::size_t k = i; k < j; ++k) {
                seg.frameContourHz.push_back(frames[k].hz);
            }
            segments_.push_back(std::move(seg));
        }
        i = j;
    }
}

void VariAudioEngine::quantizePitchAll(float strength01) noexcept {
    const float s = std::clamp(strength01, 0.0f, 1.0f);
    for (auto& seg : segments_) {
        seg.microPitchCents *= (1.0f - s);
    }
}

void VariAudioEngine::straightenVibratoAll(float straightenAmount01) noexcept {
    const float s = std::clamp(straightenAmount01, 0.0f, 1.0f);
    for (auto& seg : segments_) {
        seg.vibratoScale = 1.0f - s;
    }
}

std::vector<float> VariAudioEngine::resynthesizePSOLA(std::span<const float> originalMono) const {
    std::vector<float> out(originalMono.begin(), originalMono.end());
    if (originalMono.empty() || segments_.empty()) {
        return out;
    }

    for (const auto& seg : segments_) {
        const std::size_t sFrame = std::min(seg.startFrame, originalMono.size());
        const std::size_t eFrame = std::min(seg.endFrame, originalMono.size());
        if (eFrame <= sFrame + 32) {
            continue;
        }

        std::fill(out.begin() + static_cast<std::ptrdiff_t>(sFrame),
                  out.begin() + static_cast<std::ptrdiff_t>(eFrame), 0.0f);

        const float targetPitch = static_cast<float>(seg.targetMidiNote) + (seg.microPitchCents * 0.01f);
        const float semiShift = targetPitch - seg.detectedMidiPitch;
        const double baseRatio = std::pow(2.0, static_cast<double>(semiShift) / 12.0);
        const double srcF0 = 440.0 * std::pow(2.0, (seg.detectedMidiPitch - 69.0) / 12.0);
        const std::size_t srcPeriod = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(sampleRate_ / std::max(55.0, srcF0))), 24U, 1024U);

        double synthPos = static_cast<double>(sFrame);
        while (synthPos + srcPeriod < static_cast<double>(eFrame)) {
            const double normT = (synthPos - static_cast<double>(sFrame)) /
                                 static_cast<double>(eFrame - sFrame);
            const double tiltSemis = seg.pitchTiltSemitones * (normT - 0.5);
            const double localRatio = std::clamp(
                baseRatio * std::pow(2.0, tiltSemis / 12.0), 0.5, 2.0);
            const double synthPeriod = std::max(16.0, static_cast<double>(srcPeriod) / localRatio);

            const auto srcCenter = static_cast<std::size_t>(std::clamp<double>(
                sFrame + normT * static_cast<double>(eFrame - sFrame - 1),
                static_cast<double>(sFrame), static_cast<double>(eFrame - 1)));
            const std::size_t grainHalf = srcPeriod;
            const auto dstCenter = static_cast<std::int64_t>(std::llround(synthPos));

            // Hann-windowed pitch-synchronous grain overlap-add
            for (std::int64_t k = -static_cast<std::int64_t>(grainHalf);
                 k <= static_cast<std::int64_t>(grainHalf); ++k) {
                const std::int64_t srcIdx = static_cast<std::int64_t>(srcCenter) + k;
                const std::int64_t dstIdx = dstCenter + k;
                if (srcIdx >= static_cast<std::int64_t>(sFrame) &&
                    srcIdx < static_cast<std::int64_t>(eFrame) &&
                    dstIdx >= static_cast<std::int64_t>(sFrame) &&
                    dstIdx < static_cast<std::int64_t>(eFrame)) {
                    const double phase = (static_cast<double>(k) + static_cast<double>(grainHalf)) /
                                         static_cast<double>(2 * grainHalf);
                    const float hann = static_cast<float>(
                        0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * phase)));
                    out[static_cast<std::size_t>(dstIdx)] +=
                        originalMono[static_cast<std::size_t>(srcIdx)] * hann;
                }
            }
            synthPos += synthPeriod;
        }
    }
    return out;
}

std::vector<MidiSequenceEvent> VariAudioEngine::extractAudioToMidi(
    const MusicalClock& clock, std::int64_t clipStartSample, bool includePitchBend) const {
    std::vector<MidiSequenceEvent> midiOut;
    midiOut.reserve(segments_.size() * 2U);

    for (const auto& seg : segments_) {
        const std::int64_t sTick = clock.samplesToTicks(
            clipStartSample + static_cast<std::int64_t>(seg.startFrame));
        const std::int64_t eTick = clock.samplesToTicks(
            clipStartSample + static_cast<std::int64_t>(seg.endFrame));

        midiOut.push_back(MidiSequenceEvent{
            .kind = SequencerMidiKind::Note,
            .startTick = sTick,
            .lengthTicks = std::max<std::int64_t>(30, eTick - sTick),
            .channel = 0,
            .pitchOrCC = static_cast<std::uint8_t>(std::clamp(seg.targetMidiNote, 0, 127)),
            .velocityOrVal = 100
        });

        if (includePitchBend && std::abs(seg.microPitchCents) > 1.0f) {
            // +/- 2 semitones (200 cents) pitchbend range -> [-8192 .. +8191]
            const auto bend = static_cast<std::int16_t>(
                std::clamp(std::lround((seg.microPitchCents / 200.0f) * 8191.0f), -8192L, 8191L));
            midiOut.push_back(MidiSequenceEvent{
                .kind = SequencerMidiKind::PitchBend,
                .startTick = sTick,
                .lengthTicks = 0,
                .channel = 0,
                .pitchBend14Bit = bend
            });
        }
    }
    return midiOut;
}

// ============================================================================
// PitchCorrectProcessor (Clean-Room Real-Time Scale-Quantized Pitch Corrector)
// ============================================================================
PitchCorrectProcessor::PitchCorrectProcessor() = default;

void PitchCorrectProcessor::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void PitchCorrectProcessor::reset() noexcept {
    inputRing_.fill(0.0f);
    writePos_ = 0;
    readPhaseA_ = 0.0;
    readPhaseB_ = 512.0;
    smoothedRatio_ = 1.0f;
    hopCounter_ = 0;
}

float PitchCorrectProcessor::snapMidiNoteToActiveScale(float rawMidi) const noexcept {
    const int baseNote = static_cast<int>(std::lround(rawMidi));
    int bestSemi = baseNote;
    float bestDist = 999.0f;

    for (int offset = -6; offset <= 6; ++offset) {
        const int candidate = baseNote + offset;
        const int pitchClass = ((candidate % 12) + 12) % 12;
        if (scaleMask_[static_cast<std::size_t>(pitchClass)]) {
            const float dist = std::abs(static_cast<float>(candidate) - rawMidi);
            if (dist < bestDist) {
                bestDist = dist;
                bestSemi = candidate;
            }
        }
    }
    if (bestDist * 100.0f <= toleranceCents_) {
        return rawMidi; // Within tolerance window
    }
    return static_cast<float>(bestSemi);
}

void PitchCorrectProcessor::processAudioRT(AudioBlock32& ioBuffer,
                                           const AudioBlock32* /*sidechainIn*/,
                                           std::uint32_t numFrames) noexcept {
    if (ioBuffer.numChannels() == 0 || numFrames == 0) {
        return;
    }
    float* ch0 = ioBuffer.channel(0);
    float* ch1 = (ioBuffer.numChannels() > 1) ? ioBuffer.channel(1) : nullptr;

    constexpr double kGrainLen = 1024.0;
    for (std::uint32_t f = 0; f < numFrames; ++f) {
        const float inSample = ch0[f];
        inputRing_[writePos_ & kRingMask] = inSample;

        // Re-estimate pitch every 256 samples with zero heap allocations
        if ((++hopCounter_ & 255U) == 0U) {
            for (std::size_t k = 0; k < yinScratch_.size(); ++k) {
                yinScratch_[k] = inputRing_[(writePos_ + kAnalysisRingSize - yinScratch_.size() + k) & kRingMask];
            }
            float conf = 0.0f;
            const float hz = VariAudioEngine::detectPitchYIN(yinScratch_, sampleRate_, 0.18f, &conf);
            if (hz > 60.0f && conf > 0.5f) {
                const float detectedMidi = 69.0f + 12.0f * std::log2(hz / 440.0f);
                const float targetMidi = snapMidiNoteToActiveScale(detectedMidi) + transposeSemitones_;
                const float targetRatio = std::clamp(
                    std::pow(2.0f, (targetMidi - detectedMidi) / 12.0f), 0.5f, 2.0f);
                const float alpha = 0.05f + 0.90f * std::clamp(speed_, 0.0f, 1.0f);
                smoothedRatio_ = smoothedRatio_ * (1.0f - alpha) + targetRatio * alpha;
            } else {
                smoothedRatio_ = smoothedRatio_ * 0.95f + 1.0f * 0.05f;
            }
        }

        // Dual-tap Hann crossfaded fractional delay line for phase-coherent pitch shifting
        auto sampleFractional = [&](double delaySamples) noexcept -> float {
            const double pos = static_cast<double>(writePos_ + kAnalysisRingSize) - delaySamples;
            const auto i0 = static_cast<std::size_t>(pos) & kRingMask;
            const std::size_t i1 = (i0 + 1U) & kRingMask;
            const float frac = static_cast<float>(pos - std::floor(pos));
            return inputRing_[i0] * (1.0f - frac) + inputRing_[i1] * frac;
        };

        const double rateDelta = 1.0 - static_cast<double>(smoothedRatio_);
        readPhaseA_ += rateDelta;
        readPhaseB_ += rateDelta;
        while (readPhaseA_ < 0.0) readPhaseA_ += kGrainLen;
        while (readPhaseA_ >= kGrainLen) readPhaseA_ -= kGrainLen;
        while (readPhaseB_ < 0.0) readPhaseB_ += kGrainLen;
        while (readPhaseB_ >= kGrainLen) readPhaseB_ -= kGrainLen;

        const float winA = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * (readPhaseA_ / kGrainLen))));
        const float winB = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * (readPhaseB_ / kGrainLen))));
        const float wet = sampleFractional(64.0 + readPhaseA_) * winA +
                          sampleFractional(64.0 + readPhaseB_) * winB;
        const float out = inSample * (1.0f - dryWet_) + wet * dryWet_;

        ch0[f] = out;
        if (ch1 != nullptr) {
            ch1[f] = out;
        }
        ++writePos_;
    }
}

void PitchCorrectProcessor::setParameterNormalized(std::uint32_t paramId, float normValue) noexcept {
    const float v = std::clamp(normValue, 0.0f, 1.0f);
    switch (paramId) {
        case 0: speed_ = v; break;
        case 1: toleranceCents_ = v * 100.0f; break;
        case 2: transposeSemitones_ = (v - 0.5f) * 24.0f; break;
        case 3: dryWet_ = v; break;
        default: break;
    }
}

float PitchCorrectProcessor::getParameterNormalized(std::uint32_t paramId) const noexcept {
    switch (paramId) {
        case 0: return speed_;
        case 1: return toleranceCents_ / 100.0f;
        case 2: return (transposeSemitones_ / 24.0f) + 0.5f;
        case 3: return dryWet_;
        default: return 0.0f;
    }
}

const char* PitchCorrectProcessor::parameterName(std::uint32_t paramId) const noexcept {
    switch (paramId) {
        case 0: return "Speed";
        case 1: return "Tolerance";
        case 2: return "Transpose";
        case 3: return "Mix";
        default: return "";
    }
}

} // namespace AlphaStudio::Core::DSP
