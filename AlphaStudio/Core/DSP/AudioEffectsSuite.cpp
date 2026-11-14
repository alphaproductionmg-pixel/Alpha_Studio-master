#include "AlphaStudio/Core/DSP/AudioEffectsSuite.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core::DSP {

namespace {
[[nodiscard]] inline float db2lin(float db) noexcept {
    return (db <= -120.0f) ? 0.0f : std::pow(10.0f, db * 0.05f);
}
[[nodiscard]] inline float lin2db(float lin) noexcept {
    return 20.0f * std::log10(std::max(1e-7f, std::abs(lin)));
}
} // namespace

// ============================================================================
// UV22HRDitherEngine
// ============================================================================
void UV22HRDitherEngine::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    carrierPhase_ = 0.0;
    silentCounter_ = 0;
}

float UV22HRDitherEngine::processSampleRT(float input, std::uint32_t targetBitDepth,
                                          bool lowLevelMode, bool autoBlackMute) noexcept {
    const std::uint32_t bits = std::clamp(targetBitDepth, 8U, 24U);
    if (autoBlackMute) {
        if (std::abs(input) < 1e-8f) {
            if (++silentCounter_ > 512U) {
                return 0.0f; // Autoblack silences dither during digital silence
            }
        } else {
            silentCounter_ = 0;
        }
    }

    const float scale = static_cast<float>(1ULL << (bits - 1U));
    const float invScale = 1.0f / scale;

    // High-resolution ~22.05 kHz ultrasonic energy bias + TPDF white noise
    const double carrierFreq = std::min(22050.0, sampleRate_ * 0.46);
    carrierPhase_ += (2.0 * std::numbers::pi * carrierFreq) / sampleRate_;
    if (carrierPhase_ >= 2.0 * std::numbers::pi) {
        carrierPhase_ -= 2.0 * std::numbers::pi;
    }

    auto nextRand01 = [this]() noexcept -> float {
        rngState_ = rngState_ * 1664525U + 1013904223U;
        return static_cast<float>(rngState_ >> 8U) / 16777216.0f;
    };

    const float tpdf = (nextRand01() - nextRand01()) * 0.35f;
    const float hiBiasAmp = lowLevelMode ? 0.35f : 0.70f;
    const float ditherLsb = static_cast<float>(std::sin(carrierPhase_)) * hiBiasAmp + tpdf;

    const float quantized = std::round(std::clamp(input, -1.0f, 0.999999f) * scale + ditherLsb);
    return std::clamp(quantized * invScale, -1.0f, 1.0f);
}

// ============================================================================
// ParametricSuiteAudioEffect::SVFilterState (Cytomic/Trapezoidal Zero-Delay SVF)
// ============================================================================
void ParametricSuiteAudioEffect::SVFilterState::processSample(
    float x, float cutoffHz, float q, double sr,
    float& lp, float& bp, float& hp) noexcept {
    const float fc = std::clamp(cutoffHz, 20.0f, static_cast<float>(sr * 0.47));
    const float g = std::tan(std::numbers::pi_v<float> * fc / static_cast<float>(sr));
    const float k = 1.0f / std::max(0.15f, q);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = x - ic2eq;
    const float v1 = a1 * ic1eq + a2 * v3;
    const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
    ic1eq = 2.0f * v1 - ic1eq;
    ic2eq = 2.0f * v2 - ic2eq;
    lp = v2;
    bp = v1;
    hp = x - k * v1 - v2;
}

// ============================================================================
// ParametricSuiteAudioEffect
// ============================================================================
ParametricSuiteAudioEffect::ParametricSuiteAudioEffect(
    std::string exactName,
    AudioEffectCategory category,
    AudioEffectAlgorithm algo,
    std::vector<EffectParameterSpec> paramSpecs)
    : name_(std::move(exactName)),
      category_(category),
      algo_(algo),
      specs_(std::move(paramSpecs)),
      delayLineL_(kDelayBufLen, 0.0f),
      delayLineR_(kDelayBufLen, 0.0f) {
    normValues_.resize(specs_.size(), 0.5f);
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        const float span = specs_[i].maxVal - specs_[i].minVal;
        normValues_[i] = (std::abs(span) > 1e-9f)
            ? std::clamp((specs_[i].defaultVal - specs_[i].minVal) / span, 0.0f, 1.0f)
            : 0.5f;
    }
}

void ParametricSuiteAudioEffect::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    internalEQ_.prepare(sampleRate_);
    uv22Engine_.prepare(sampleRate_);
    reset();
}

void ParametricSuiteAudioEffect::reset() noexcept {
    std::fill(delayLineL_.begin(), delayLineL_.end(), 0.0f);
    std::fill(delayLineR_.begin(), delayLineR_.end(), 0.0f);
    delayWrite_ = 0;
    lfoPhase_ = 0.0;
    oscPhase_ = 0.0;
    envFollower_ = 0.0f;
    fastEnv_ = 0.0f;
    slowEnv_ = 0.0f;
    gateOpenState_ = false;
    allpassStates_[0].fill(0.0f);
    allpassStates_[1].fill(0.0f);
    svfLeft_.fill({});
    svfRight_.fill({});
    geqZ1L_.fill(0.0f); geqZ2L_.fill(0.0f);
    geqZ1R_.fill(0.0f); geqZ2R_.fill(0.0f);
    internalEQ_.resetState();
}

bool ParametricSuiteAudioEffect::supportsSidechain() const noexcept {
    return algo_ == AudioEffectAlgorithm::Compressor ||
           algo_ == AudioEffectAlgorithm::VintageCompressor ||
           algo_ == AudioEffectAlgorithm::Gate ||
           algo_ == AudioEffectAlgorithm::Expander ||
           algo_ == AudioEffectAlgorithm::DeEsser ||
           algo_ == AudioEffectAlgorithm::VSTDynamics ||
           algo_ == AudioEffectAlgorithm::MultibandCompressor;
}

void ParametricSuiteAudioEffect::setParameterNormalized(std::uint32_t paramId, float normValue) noexcept {
    if (paramId < normValues_.size()) {
        normValues_[paramId] = std::clamp(normValue, 0.0f, 1.0f);
    }
}

float ParametricSuiteAudioEffect::getParameterNormalized(std::uint32_t paramId) const noexcept {
    return (paramId < normValues_.size()) ? normValues_[paramId] : 0.0f;
}

float ParametricSuiteAudioEffect::getParameterPlain(std::uint32_t paramId) const noexcept {
    if (paramId >= specs_.size()) {
        return 0.0f;
    }
    return specs_[paramId].minVal + normValues_[paramId] * (specs_[paramId].maxVal - specs_[paramId].minVal);
}

const char* ParametricSuiteAudioEffect::parameterName(std::uint32_t paramId) const noexcept {
    return (paramId < specs_.size()) ? specs_[paramId].name.c_str() : "";
}

void ParametricSuiteAudioEffect::processAudioRT(AudioBlock32& ioBuffer,
                                                const AudioBlock32* sidechainIn,
                                                std::uint32_t numFrames) noexcept {
    if (ioBuffer.numChannels() == 0 || numFrames == 0) {
        return;
    }
    float* chL = ioBuffer.channel(0);
    float* chR = (ioBuffer.numChannels() > 1) ? ioBuffer.channel(1) : chL;
    const float* scL = (sidechainIn != nullptr && sidechainIn->numChannels() > 0)
        ? sidechainIn->channel(0)
        : nullptr;
    const float* scR = (sidechainIn != nullptr && sidechainIn->numChannels() > 1)
        ? sidechainIn->channel(1)
        : scL;

    const float p0 = getParameterPlain(0);
    const float p1 = getParameterPlain(1);
    const float p2 = getParameterPlain(2);
    const float p3 = getParameterPlain(3);

    auto readDelay = [&](const std::vector<float>& line, double delaySamples) noexcept -> float {
        const double d = std::clamp(delaySamples, 1.0, static_cast<double>(kDelayBufLen - 4));
        const double pos = static_cast<double>(delayWrite_ + kDelayBufLen) - d;
        const std::size_t i0 = static_cast<std::size_t>(pos) & kDelayMask;
        const std::size_t i1 = (i0 + 1U) & kDelayMask;
        const float frac = static_cast<float>(pos - std::floor(pos));
        return line[i0] * (1.0f - frac) + line[i1] * frac;
    };

    // Graphic / Parametric Channel EQ shortcuts
    if (algo_ == AudioEffectAlgorithm::StandardChannelEQ || algo_ == AudioEffectAlgorithm::StudioEQ) {
        internalEQ_.configureBand(0, true, ChannelEQBandType::LowShelf, 100.0f, p0, 0.707f);
        internalEQ_.configureBand(1, true, ChannelEQBandType::ParametricPeak, p1, p2, 1.1f);
        internalEQ_.configureBand(3, true, ChannelEQBandType::HighShelf, 8500.0f, p3, 0.707f);
        internalEQ_.processRT(ioBuffer, numFrames);
        return;
    }

    if (algo_ == AudioEffectAlgorithm::GEQ10 || algo_ == AudioEffectAlgorithm::GEQ30) {
        const std::size_t numBands = (algo_ == AudioEffectAlgorithm::GEQ10) ? 10U : 30U;
        for (std::size_t b = 0; b < numBands; ++b) {
            const float gainDb = (b < specs_.size()) ? getParameterPlain(static_cast<std::uint32_t>(b)) : 0.0f;
            if (std::abs(gainDb) < 0.05f) continue;
            const double fc = 31.25 * std::pow(2.0, static_cast<double>(b) * (algo_ == AudioEffectAlgorithm::GEQ10 ? 1.0 : (1.0 / 3.0)));
            if (fc >= sampleRate_ * 0.47) continue;
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * std::numbers::pi * fc / sampleRate_;
            const double alpha = std::sin(w0) / (2.0 * (algo_ == AudioEffectAlgorithm::GEQ10 ? 1.414 : 4.318));
            const double invA0 = 1.0 / (1.0 + alpha / A);
            const float b0 = static_cast<float>((1.0 + alpha * A) * invA0);
            const float b1 = static_cast<float>((-2.0 * std::cos(w0)) * invA0);
            const float b2 = static_cast<float>((1.0 - alpha * A) * invA0);
            const float a1 = b1;
            const float a2 = static_cast<float>((1.0 - alpha / A) * invA0);
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float xL = chL[f];
                const float yL = b0 * xL + geqZ1L_[b];
                geqZ1L_[b] = b1 * xL - a1 * yL + geqZ2L_[b];
                geqZ2L_[b] = b2 * xL - a2 * yL;
                chL[f] = yL;
                if (chR != chL) {
                    const float xR = chR[f];
                    const float yR = b0 * xR + geqZ1R_[b];
                    geqZ1R_[b] = b1 * xR - a1 * yR + geqZ2R_[b];
                    geqZ2R_[b] = b2 * xR - a2 * yR;
                    chR[f] = yR;
                }
            }
        }
        return;
    }

    // Multi-channel matrix processors: Mix6to2, MixConvert, Surround Panner
    if (algo_ == AudioEffectAlgorithm::Mix6to2 || algo_ == AudioEffectAlgorithm::MixConvert ||
        algo_ == AudioEffectAlgorithm::SurroundPanner) {
        if (ioBuffer.numChannels() >= 6U) {
            float* C = ioBuffer.channel(2);
            float* LFE = ioBuffer.channel(3);
            float* Ls = ioBuffer.channel(4);
            float* Rs = ioBuffer.channel(5);
            const float cGain = db2lin(p0);
            const float surGain = db2lin(p1);
            const float lfeGain = db2lin(p2);
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                if (algo_ == AudioEffectAlgorithm::SurroundPanner) {
                    const float angle = (p0 + 180.0f) * (std::numbers::pi_v<float> / 360.0f);
                    const float mono = 0.5f * (chL[f] + chR[f]);
                    chL[f] = mono * std::cos(angle);
                    chR[f] = mono * std::sin(angle);
                    C[f] = mono * p1 * 0.5f;
                    LFE[f] = mono * lfeGain;
                    Ls[f] = chL[f] * surGain * 0.5f;
                    Rs[f] = chR[f] * surGain * 0.5f;
                } else {
                    chL[f] = chL[f] + 0.7071f * cGain * C[f] + surGain * Ls[f] + lfeGain * LFE[f];
                    chR[f] = chR[f] + 0.7071f * cGain * C[f] + surGain * Rs[f] + lfeGain * LFE[f];
                    if (algo_ == AudioEffectAlgorithm::Mix6to2) {
                        C[f] = LFE[f] = Ls[f] = Rs[f] = 0.0f;
                    }
                }
            }
        } else {
            ioBuffer.applyGainRT(numFrames, db2lin(p0 * 0.25f));
        }
        return;
    }

    double sumLR = 0.0, sumLL = 1e-9, sumRR = 1e-9;

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        float xL = chL[f];
        float xR = chR[f];
        const float detL = (scL != nullptr) ? scL[f] : xL;
        const float detR = (scR != nullptr) ? scR[f] : xR;
        const float detAbs = std::max(std::abs(detL), std::abs(detR));

        float yL = xL;
        float yR = xR;

        switch (algo_) {
            // ---------------- Family 1: Dynamics ----------------
            case AudioEffectAlgorithm::Compressor:
            case AudioEffectAlgorithm::VintageCompressor:
            case AudioEffectAlgorithm::VSTDynamics: {
                const float threshDb = p0;
                const float ratio = std::max(1.0f, p1);
                const float atkCoeff = std::exp(-1.0f / static_cast<float>(std::max(1.0, p2 * 0.001 * sampleRate_)));
                const float relCoeff = std::exp(-1.0f / static_cast<float>(std::max(1.0, p3 * 0.001 * sampleRate_)));
                envFollower_ = (detAbs > envFollower_)
                    ? (atkCoeff * envFollower_ + (1.0f - atkCoeff) * detAbs)
                    : (relCoeff * envFollower_ + (1.0f - relCoeff) * detAbs);
                const float envDb = lin2db(envFollower_);
                float grDb = 0.0f;
                if (envDb > threshDb) {
                    grDb = -(envDb - threshDb) * (1.0f - 1.0f / ratio);
                }
                if (algo_ == AudioEffectAlgorithm::VSTDynamics && envDb < -55.0f) {
                    grDb -= 18.0f; // Integrated Gate stage
                }
                float g = db2lin(grDb + (algo_ == AudioEffectAlgorithm::VintageCompressor ? 1.5f : 0.0f));
                yL = xL * g;
                yR = xR * g;
                if (algo_ == AudioEffectAlgorithm::VSTDynamics) {
                    yL = std::clamp(yL, -0.98f, 0.98f);
                    yR = std::clamp(yR, -0.98f, 0.98f);
                }
                break;
            }
            case AudioEffectAlgorithm::Limiter:
            case AudioEffectAlgorithm::Maximizer: {
                const float drive = (algo_ == AudioEffectAlgorithm::Maximizer) ? db2lin(p0) : 1.0f;
                const float ceiling = db2lin(p1);
                const float boostedL = xL * drive;
                const float boostedR = xR * drive;
                const float pk = std::max(std::abs(boostedL), std::abs(boostedR));
                envFollower_ = std::max(pk, envFollower_ * 0.9985f);
                const float g = (envFollower_ > ceiling) ? (ceiling / envFollower_) : 1.0f;
                yL = boostedL * g;
                yR = boostedR * g;
                break;
            }
            case AudioEffectAlgorithm::Gate:
            case AudioEffectAlgorithm::Expander: {
                const float threshLin = db2lin(p0);
                envFollower_ = 0.99f * envFollower_ + 0.01f * detAbs;
                if (algo_ == AudioEffectAlgorithm::Gate) {
                    if (envFollower_ > threshLin) gateOpenState_ = true;
                    else if (envFollower_ < threshLin * 0.75f) gateOpenState_ = false;
                    const float target = gateOpenState_ ? 1.0f : db2lin(p2);
                    slowEnv_ = 0.992f * slowEnv_ + 0.008f * target;
                    yL = xL * slowEnv_;
                    yR = xR * slowEnv_;
                } else {
                    const float g = (envFollower_ < threshLin)
                        ? std::pow(std::max(1e-5f, envFollower_ / std::max(1e-5f, threshLin)), p1 - 1.0f)
                        : 1.0f;
                    yL = xL * g;
                    yR = xR * g;
                }
                break;
            }
            case AudioEffectAlgorithm::MIDIGate: {
                const float target = midiGateOpen_ ? midiGateVel_ : 0.0f;
                envFollower_ = 0.994f * envFollower_ + 0.006f * target;
                yL = xL * envFollower_;
                yR = xR * envFollower_;
                break;
            }
            case AudioEffectAlgorithm::DeEsser: {
                float lp = 0.0f, bp = 0.0f, hp = 0.0f;
                svfLeft_[0].processSample(detL, p0, 2.5f, sampleRate_, lp, bp, hp);
                envFollower_ = 0.985f * envFollower_ + 0.015f * std::abs(bp);
                const float thresh = db2lin(p1);
                const float atten = (envFollower_ > thresh) ? (thresh / envFollower_) : 1.0f;
                yL = xL - bp * (1.0f - atten);
                yR = xR - bp * (1.0f - atten);
                break;
            }
            case AudioEffectAlgorithm::EnvelopeShaper: {
                fastEnv_ = 0.94f * fastEnv_ + 0.06f * detAbs;
                slowEnv_ = 0.9985f * slowEnv_ + 0.0015f * detAbs;
                const float diff = fastEnv_ - slowEnv_;
                const float gainDb = (diff >= 0.0f) ? (p0 * std::min(1.0f, diff * 8.0f))
                                                    : (p1 * std::min(1.0f, -diff * 8.0f));
                const float g = db2lin(gainDb);
                yL = xL * g;
                yR = xR * g;
                break;
            }
            case AudioEffectAlgorithm::MultibandCompressor: {
                float lpL = 0.0f, bpL = 0.0f, hpL = 0.0f;
                float lpR = 0.0f, bpR = 0.0f, hpR = 0.0f;
                svfLeft_[0].processSample(xL, p0, 0.707f, sampleRate_, lpL, bpL, hpL);
                svfRight_[0].processSample(xR, p0, 0.707f, sampleRate_, lpR, bpR, hpR);
                const float lowPk = std::max(std::abs(lpL), std::abs(lpR));
                const float hiPk = std::max(std::abs(hpL), std::abs(hpR));
                fastEnv_ = 0.995f * fastEnv_ + 0.005f * lowPk;
                slowEnv_ = 0.995f * slowEnv_ + 0.005f * hiPk;
                const float thresh = db2lin(p1);
                const float gLow = (fastEnv_ > thresh) ? std::pow(thresh / fastEnv_, 1.0f - 1.0f / std::max(1.1f, p2)) : 1.0f;
                const float gHi  = (slowEnv_ > thresh) ? std::pow(thresh / slowEnv_, 1.0f - 1.0f / std::max(1.1f, p2)) : 1.0f;
                const float makeup = db2lin(p3);
                yL = (lpL * gLow + hpL * gHi) * makeup;
                yR = (lpR * gLow + hpR * gHi) * makeup;
                break;
            }

            // ---------------- Family 2: EQ & Filters ----------------
            case AudioEffectAlgorithm::DualFilter:
            case AudioEffectAlgorithm::Tonic:
            case AudioEffectAlgorithm::ToneBooster:
            case AudioEffectAlgorithm::WahWah:
            case AudioEffectAlgorithm::StepFilter: {
                float cutoff = p0;
                if (algo_ == AudioEffectAlgorithm::WahWah) {
                    envFollower_ = 0.995f * envFollower_ + 0.005f * detAbs;
                    cutoff = std::clamp(p0 + envFollower_ * p2 * 2400.0f, 200.0f, 4500.0f);
                } else if (algo_ == AudioEffectAlgorithm::StepFilter) {
                    const std::size_t stepIdx = (static_cast<std::size_t>(
                        (static_cast<double>(sampleCounter_) / sampleRate_) * std::max(0.5f, p2) * 4.0)) & 15U;
                    const float pattern = ((stepIdx * 5U + 3U) & 7U) / 7.0f;
                    cutoff = std::clamp(p0 * (0.4f + 1.4f * pattern), 80.0f, 12000.0f);
                }
                float lpL = 0.0f, bpL = 0.0f, hpL = 0.0f;
                float lpR = 0.0f, bpR = 0.0f, hpR = 0.0f;
                svfLeft_[0].processSample(xL, cutoff, std::max(0.5f, p1), sampleRate_, lpL, bpL, hpL);
                svfRight_[0].processSample(xR, cutoff, std::max(0.5f, p1), sampleRate_, lpR, bpR, hpR);
                if (algo_ == AudioEffectAlgorithm::WahWah || algo_ == AudioEffectAlgorithm::ToneBooster) {
                    yL = xL * 0.4f + bpL * 1.8f;
                    yR = xR * 0.4f + bpR * 1.8f;
                } else if (algo_ == AudioEffectAlgorithm::DualFilter) {
                    yL = lpL * (1.0f - p2) + hpL * p2;
                    yR = lpR * (1.0f - p2) + hpR * p2;
                } else {
                    yL = lpL;
                    yR = lpR;
                }
                break;
            }

            // ---------------- Family 3: Delays & Modulation ----------------
            case AudioEffectAlgorithm::MonoDelay:
            case AudioEffectAlgorithm::StereoDelay:
            case AudioEffectAlgorithm::PingPongDelay:
            case AudioEffectAlgorithm::ModMachine: {
                const double delSampL = std::max(4.0, p0 * 0.001 * sampleRate_);
                const double delSampR = (algo_ == AudioEffectAlgorithm::StereoDelay)
                    ? std::max(4.0, p1 * 0.001 * sampleRate_)
                    : delSampL;
                const float modWobble = (algo_ == AudioEffectAlgorithm::ModMachine)
                    ? static_cast<float>(std::sin(lfoPhase_) * 18.0)
                    : 0.0f;
                lfoPhase_ += (2.0 * std::numbers::pi * 1.2) / sampleRate_;
                const float dL = readDelay(delayLineL_, delSampL + modWobble);
                const float dR = readDelay(delayLineR_, delSampR - modWobble);
                const float fb = std::clamp(p2, 0.0f, 0.92f);
                if (algo_ == AudioEffectAlgorithm::PingPongDelay) {
                    delayLineL_[delayWrite_] = xL + xR + dR * fb;
                    delayLineR_[delayWrite_] = dL * fb;
                } else if (algo_ == AudioEffectAlgorithm::MonoDelay) {
                    const float m = 0.5f * (xL + xR);
                    delayLineL_[delayWrite_] = m + dL * fb;
                    delayLineR_[delayWrite_] = m + dL * fb;
                } else {
                    delayLineL_[delayWrite_] = xL + dL * fb;
                    delayLineR_[delayWrite_] = xR + dR * fb;
                }
                delayWrite_ = (delayWrite_ + 1U) & kDelayMask;
                yL = xL * (1.0f - p3) + dL * p3;
                yR = xR * (1.0f - p3) + dR * p3;
                break;
            }
            case AudioEffectAlgorithm::Chorus:
            case AudioEffectAlgorithm::StudioChorus:
            case AudioEffectAlgorithm::Flanger:
            case AudioEffectAlgorithm::Vibrato:
            case AudioEffectAlgorithm::Cloner: {
                lfoPhase_ += (2.0 * std::numbers::pi * std::max(0.05f, p0)) / sampleRate_;
                if (lfoPhase_ >= 2.0 * std::numbers::pi) lfoPhase_ -= 2.0 * std::numbers::pi;
                const double baseMs = (algo_ == AudioEffectAlgorithm::Flanger) ? 2.5 : 14.0;
                const double depthMs = (algo_ == AudioEffectAlgorithm::Flanger) ? (p1 * 3.5) : (p1 * 8.0);
                const double dL = (baseMs + depthMs * (0.5 + 0.5 * std::sin(lfoPhase_))) * 0.001 * sampleRate_;
                const double dR = (baseMs + depthMs * (0.5 + 0.5 * std::cos(lfoPhase_))) * 0.001 * sampleRate_;
                const float wetL = readDelay(delayLineL_, dL);
                const float wetR = readDelay(delayLineR_, dR);
                const float fb = (algo_ == AudioEffectAlgorithm::Flanger) ? std::clamp(p2, -0.85f, 0.85f) : 0.0f;
                delayLineL_[delayWrite_] = xL + wetL * fb;
                delayLineR_[delayWrite_] = xR + wetR * fb;
                delayWrite_ = (delayWrite_ + 1U) & kDelayMask;
                if (algo_ == AudioEffectAlgorithm::Vibrato) {
                    yL = wetL;
                    yR = wetR;
                } else {
                    yL = xL * (1.0f - p3) + wetL * p3;
                    yR = xR * (1.0f - p3) + wetR * p3;
                }
                break;
            }
            case AudioEffectAlgorithm::Phaser:
            case AudioEffectAlgorithm::Metalizer: {
                lfoPhase_ += (2.0 * std::numbers::pi * std::max(0.05f, p0)) / sampleRate_;
                const float a = std::clamp(0.15f + 0.70f * (0.5f + 0.5f * std::sin(static_cast<float>(lfoPhase_))) * p1, 0.05f, 0.92f);
                float sL = xL + allpassStates_[0][5] * std::clamp(p2, -0.85f, 0.85f);
                float sR = xR + allpassStates_[1][5] * std::clamp(p2, -0.85f, 0.85f);
                for (std::size_t st = 0; st < 6; ++st) {
                    const float nl = -a * sL + allpassStates_[0][st];
                    allpassStates_[0][st] = sL + a * nl;
                    sL = nl;
                    const float nr = -a * sR + allpassStates_[1][st];
                    allpassStates_[1][st] = sR + a * nr;
                    sR = nr;
                }
                yL = xL * (1.0f - p3) + sL * p3;
                yR = xR * (1.0f - p3) + sR * p3;
                break;
            }
            case AudioEffectAlgorithm::AutoPan:
            case AudioEffectAlgorithm::Tremolo:
            case AudioEffectAlgorithm::Chopper:
            case AudioEffectAlgorithm::Rotary: {
                const double rate = (algo_ == AudioEffectAlgorithm::Rotary) ? (p0 > 0.5f ? 6.4 : 0.85) : std::max(0.1f, p0);
                lfoPhase_ += (2.0 * std::numbers::pi * rate) / sampleRate_;
                if (lfoPhase_ >= 2.0 * std::numbers::pi) lfoPhase_ -= 2.0 * std::numbers::pi;
                const float s = std::sin(static_cast<float>(lfoPhase_));
                if (algo_ == AudioEffectAlgorithm::AutoPan || algo_ == AudioEffectAlgorithm::Rotary) {
                    const float panL = 0.5f * (1.0f - s * p1);
                    const float panR = 0.5f * (1.0f + s * p1);
                    yL = xL * panL * 1.414f;
                    yR = xR * panR * 1.414f;
                } else if (algo_ == AudioEffectAlgorithm::Chopper) {
                    const float gate = (s >= 0.0f) ? 1.0f : (1.0f - p1);
                    yL = xL * gate;
                    yR = xR * gate;
                } else {
                    const float mod = 1.0f - p1 * (0.5f + 0.5f * s);
                    yL = xL * mod;
                    yR = xR * mod;
                }
                break;
            }

            // ---------------- Family 4: Saturation, Distortion & Pitch FX ----------------
            case AudioEffectAlgorithm::AmpSimulator:
            case AudioEffectAlgorithm::DaTube:
            case AudioEffectAlgorithm::Distortion:
            case AudioEffectAlgorithm::SoftClipper: {
                const float drive = 1.0f + p0 * ((algo_ == AudioEffectAlgorithm::DaTube) ? 6.0f : 18.0f);
                auto shape = [&](float s) noexcept -> float {
                    const float d = s * drive;
                    if (algo_ == AudioEffectAlgorithm::DaTube) {
                        // Asymmetric warm even-harmonic tube saturation
                        return std::tanh(d + 0.15f * d * d) - std::tanh(0.15f);
                    }
                    if (algo_ == AudioEffectAlgorithm::SoftClipper) {
                        const float c = std::clamp(d, -1.5f, 1.5f);
                        return c - (c * c * c) / 6.75f;
                    }
                    return std::tanh(d);
                };
                yL = shape(xL) * db2lin(p2);
                yR = shape(xR) * db2lin(p2);
                if (algo_ == AudioEffectAlgorithm::AmpSimulator) {
                    float lpL = 0.0f, bpL = 0.0f, hpL = 0.0f;
                    float lpR = 0.0f, bpR = 0.0f, hpR = 0.0f;
                    svfLeft_[0].processSample(yL, 3800.0f, 0.85f, sampleRate_, lpL, bpL, hpL);
                    svfRight_[0].processSample(yR, 3800.0f, 0.85f, sampleRate_, lpR, bpR, hpR);
                    yL = lpL;
                    yR = lpR;
                }
                break;
            }
            case AudioEffectAlgorithm::Bitcrusher:
            case AudioEffectAlgorithm::Grungelizer: {
                const float bits = std::clamp(p0, 2.0f, 24.0f);
                const float levels = std::pow(2.0f, bits - 1.0f);
                const auto hold = std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(p1));
                if ((sampleCounter_ % hold) == 0U) {
                    fastEnv_ = std::round(xL * levels) / levels;
                    slowEnv_ = std::round(xR * levels) / levels;
                }
                float crackle = 0.0f;
                if (algo_ == AudioEffectAlgorithm::Grungelizer) {
                    rng_ = rng_ * 1664525U + 1013904223U;
                    const float r = (static_cast<float>(rng_ >> 8U) / 8388608.0f) - 1.0f;
                    crackle = (std::abs(r) > 0.996f) ? (r * p2 * 0.25f) : (r * p2 * 0.008f);
                }
                yL = fastEnv_ + crackle;
                yR = slowEnv_ + crackle;
                break;
            }
            case AudioEffectAlgorithm::Octaver: {
                if (prevSignSample_ <= 0.0f && xL > 0.0f) {
                    octaverFlip1_ = -octaverFlip1_;
                    if (octaverFlip1_ > 0.0f) octaverFlip2_ = -octaverFlip2_;
                }
                prevSignSample_ = xL;
                envFollower_ = 0.992f * envFollower_ + 0.008f * std::abs(xL);
                const float sub = (octaverFlip1_ * p0 + octaverFlip2_ * p1) * envFollower_;
                yL = xL * p2 + sub;
                yR = xR * p2 + sub;
                break;
            }
            case AudioEffectAlgorithm::RingModulator:
            case AudioEffectAlgorithm::Tranceformer: {
                oscPhase_ += (2.0 * std::numbers::pi * std::max(20.0f, p0)) / sampleRate_;
                if (oscPhase_ >= 2.0 * std::numbers::pi) oscPhase_ -= 2.0 * std::numbers::pi;
                const float carrier = std::sin(static_cast<float>(oscPhase_));
                yL = xL * (1.0f - p1) + (xL * carrier) * p1;
                yR = xR * (1.0f - p1) + (xR * carrier) * p1;
                break;
            }

            // ---------------- Family 5: Algorithmic Reverb (Roomworks & Roomworks SE) ----------------
            case AudioEffectAlgorithm::Roomworks:
            case AudioEffectAlgorithm::RoomworksSE: {
                const float fb = std::clamp(0.35f + p0 * 0.55f, 0.2f, 0.93f);
                const float c1L = readDelay(delayLineL_, 1557.0);
                const float c2L = readDelay(delayLineL_, 1617.0);
                const float c1R = readDelay(delayLineR_, 1491.0);
                const float c2R = readDelay(delayLineR_, 1422.0);
                const float damp = std::clamp(p1, 0.05f, 0.9f);
                fastEnv_ = c1L * (1.0f - damp) + fastEnv_ * damp;
                slowEnv_ = c1R * (1.0f - damp) + slowEnv_ * damp;
                delayLineL_[delayWrite_] = xL + 0.5f * (fastEnv_ + c2L) * fb;
                delayLineR_[delayWrite_] = xR + 0.5f * (slowEnv_ + c2R) * fb;
                delayWrite_ = (delayWrite_ + 1U) & kDelayMask;
                yL = xL * (1.0f - p2) + (c1L + c2L) * 0.5f * p2;
                yR = xR * (1.0f - p2) + (c1R + c2R) * 0.5f * p2;
                break;
            }

            // ---------------- Family 6: Spatial, Metering, Dither & Studio Tools ----------------
            case AudioEffectAlgorithm::StereoEnhancer:
            case AudioEffectAlgorithm::MonoToStereo: {
                if (algo_ == AudioEffectAlgorithm::MonoToStereo) {
                    const float d = readDelay(delayLineL_, 312.0);
                    delayLineL_[delayWrite_] = xL;
                    delayWrite_ = (delayWrite_ + 1U) & kDelayMask;
                    yL = xL + d * p0 * 0.45f;
                    yR = xL - d * p0 * 0.45f; // Complementary comb preserves exact mono sum!
                } else {
                    const float mid = 0.5f * (xL + xR);
                    const float side = 0.5f * (xL - xR) * p0;
                    yL = mid + side;
                    yR = mid - side;
                }
                break;
            }
            case AudioEffectAlgorithm::TestGenerator: {
                oscPhase_ += (2.0 * std::numbers::pi * std::max(20.0f, p0)) / sampleRate_;
                if (oscPhase_ >= 2.0 * std::numbers::pi) oscPhase_ -= 2.0 * std::numbers::pi;
                const int waveMode = static_cast<int>(std::lround(p2));
                float sig = 0.0f;
                if (waveMode == 0) {
                    sig = std::sin(static_cast<float>(oscPhase_));
                } else if (waveMode == 1) {
                    rng_ = rng_ * 1664525U + 1013904223U;
                    sig = (static_cast<float>(rng_ >> 8U) / 8388608.0f) - 1.0f; // White noise
                } else {
                    rng_ = rng_ * 1664525U + 1013904223U;
                    const float w = (static_cast<float>(rng_ >> 8U) / 8388608.0f) - 1.0f;
                    pinkB0_ = 0.99765f * pinkB0_ + w * 0.0990460f;
                    pinkB1_ = 0.96300f * pinkB1_ + w * 0.2965164f;
                    pinkB2_ = 0.57000f * pinkB2_ + w * 1.0526913f;
                    sig = (pinkB0_ + pinkB1_ + pinkB2_ + w * 0.1848f) * 0.22f; // Pink noise
                }
                yL = yR = sig * db2lin(p1);
                break;
            }
            case AudioEffectAlgorithm::SMPTEGenerator: {
                // Pure Audio Linear Timecode (LTC) Bi-Phase Mark Manchester Tone (2400 bps @ 30fps)
                const double bitRate = 80.0 * std::clamp(static_cast<double>(p0), 24.0, 30.0);
                oscPhase_ += bitRate / sampleRate_;
                if (oscPhase_ >= 1.0) {
                    oscPhase_ -= 1.0;
                    octaverFlip1_ = -octaverFlip1_;
                }
                const float ltcSquare = ((oscPhase_ >= 0.5) ? -octaverFlip1_ : octaverFlip1_) * db2lin(p1);
                yL = yR = ltcSquare;
                break;
            }
            case AudioEffectAlgorithm::UV22HR: {
                const auto bits = static_cast<std::uint32_t>(std::lround(p0));
                const bool lowMode = (p1 >= 0.5f);
                const bool autoBlack = (p2 >= 0.5f);
                yL = uv22Engine_.processSampleRT(xL, bits, lowMode, autoBlack);
                yR = uv22Engine_.processSampleRT(xR, bits, lowMode, autoBlack);
                break;
            }
            case AudioEffectAlgorithm::Tuner: {
                delayLineL_[delayWrite_ & 1023U] = xL;
                ++delayWrite_;
                if ((sampleCounter_ & 511ULL) == 0ULL) {
                    for (std::size_t k = 0; k < tunerScratch_.size(); ++k) {
                        tunerScratch_[k] = delayLineL_[(delayWrite_ + k) & 1023U];
                    }
                    const float hz = VariAudioEngine::detectPitchYIN(tunerScratch_, sampleRate_, 0.15f);
                    if (hz > 40.0f) {
                        tunerDetectedHz_ = hz;
                    }
                }
                yL = (p1 >= 0.5f) ? 0.0f : xL; // Optional Mute while tuning
                yR = (p1 >= 0.5f) ? 0.0f : xR;
                break;
            }
            case AudioEffectAlgorithm::MultiScope: {
                sumLR += static_cast<double>(xL) * static_cast<double>(xR);
                sumLL += static_cast<double>(xL) * static_cast<double>(xL);
                sumRR += static_cast<double>(xR) * static_cast<double>(xR);
                break;
            }
            default:
                break;
        }

        chL[f] = yL;
        if (chR != chL) {
            chR[f] = yR;
        }
        ++sampleCounter_;
    }

    if (algo_ == AudioEffectAlgorithm::MultiScope) {
        scopeStereoCorrelation_ = static_cast<float>(sumLR / std::sqrt(sumLL * sumRR));
    }
}

// ============================================================================
// EffectRegistry (All 58 Named Audio Effects)
// ============================================================================
EffectRegistry::EffectRegistry() {
    registerAll58Effects();
}

std::shared_ptr<IAudioInsertProcessor> EffectRegistry::createByName(std::string_view exactName) const {
    for (const auto& entry : entries_) {
        if (entry.name == exactName) {
            return entry.factory();
        }
    }
    return nullptr;
}

void EffectRegistry::registerAll58Effects() {
    using Cat = AudioEffectCategory;
    using Alg = AudioEffectAlgorithm;

    auto addParametric = [&](const char* name, Cat cat, Alg alg, std::vector<EffectParameterSpec> specs) {
        entries_.push_back(EffectRegistryEntry{
            .name = name,
            .category = cat,
            .factory = [n = std::string(name), cat, alg, sp = std::move(specs)]() {
                return std::make_shared<ParametricSuiteAudioEffect>(n, cat, alg, sp);
            }
        });
    };

    std::vector<EffectParameterSpec> geq10Specs;
    for (int i = 0; i < 10; ++i) geq10Specs.push_back({"Band " + std::to_string(i + 1), "dB", -12.0f, 12.0f, 0.0f});
    std::vector<EffectParameterSpec> geq30Specs;
    for (int i = 0; i < 30; ++i) geq30Specs.push_back({"Band " + std::to_string(i + 1), "dB", -12.0f, 12.0f, 0.0f});

    // Alphabetical registration of every single one of the 58 effects from specification:
    addParametric("AmpSimulator", Cat::SaturationAndDistortion, Alg::AmpSimulator, {{"Drive", "", 0, 1, 0.5f}, {"Tone", "", 0, 1, 0.5f}, {"Output", "dB", -24, 12, 0.0f}, {"Cab", "", 0, 1, 1.0f}});
    addParametric("AutoPan", Cat::DelayAndModulation, Alg::AutoPan, {{"Rate", "Hz", 0.1f, 12.0f, 1.5f}, {"Width", "", 0, 1, 0.8f}, {"Shape", "", 0, 1, 0.0f}, {"Sync", "", 0, 1, 0.0f}});
    addParametric("Bitcrusher", Cat::SaturationAndDistortion, Alg::Bitcrusher, {{"BitDepth", "bit", 2.0f, 24.0f, 8.0f}, {"Downsample", "x", 1.0f, 32.0f, 4.0f}, {"Mix", "", 0, 1, 1.0f}, {"Mode", "", 0, 1, 0.0f}});
    addParametric("Chopper", Cat::DelayAndModulation, Alg::Chopper, {{"Rate", "Hz", 0.5f, 16.0f, 4.0f}, {"Depth", "", 0, 1, 0.9f}, {"Shape", "", 0, 1, 0.5f}, {"Stereo", "", 0, 1, 0.5f}});
    addParametric("Chorus", Cat::DelayAndModulation, Alg::Chorus, {{"Rate", "Hz", 0.1f, 6.0f, 0.8f}, {"Depth", "", 0, 1, 0.5f}, {"Width", "", 0, 1, 1.0f}, {"Mix", "", 0, 1, 0.4f}});
    addParametric("Cloner", Cat::DelayAndModulation, Alg::Cloner, {{"Detune", "Hz", 0.1f, 3.0f, 0.6f}, {"Delay", "", 0, 1, 0.4f}, {"Humanize", "", 0, 1, 0.5f}, {"Mix", "", 0, 1, 0.5f}});
    addParametric("Compressor", Cat::Dynamics, Alg::Compressor, {{"Threshold", "dB", -60.0f, 0.0f, -18.0f}, {"Ratio", ":1", 1.0f, 20.0f, 4.0f}, {"Attack", "ms", 0.1f, 100.0f, 10.0f}, {"Release", "ms", 10.0f, 1000.0f, 120.0f}});
    addParametric("DaTube", Cat::SaturationAndDistortion, Alg::DaTube, {{"Drive", "", 0, 1, 0.35f}, {"Balance", "", 0, 1, 0.5f}, {"Output", "dB", -18, 6, 0.0f}, {"Warmth", "", 0, 1, 0.6f}});
    addParametric("DeEsser", Cat::Dynamics, Alg::DeEsser, {{"Freq", "Hz", 3000.0f, 12000.0f, 6500.0f}, {"Threshold", "dB", -48.0f, 0.0f, -20.0f}, {"Reduction", "", 0, 1, 0.6f}, {"SoloSC", "", 0, 1, 0.0f}});
    addParametric("Distortion", Cat::SaturationAndDistortion, Alg::Distortion, {{"Boost", "", 0, 1, 0.6f}, {"Feedback", "", 0, 1, 0.2f}, {"Output", "dB", -24, 6, -3.0f}, {"Tone", "", 0, 1, 0.5f}});
    addParametric("DualFilter", Cat::EQAndFilters, Alg::DualFilter, {{"Cutoff", "Hz", 40.0f, 16000.0f, 1200.0f}, {"Resonance", "", 0.5f, 10.0f, 1.5f}, {"Position", "", 0, 1, 0.5f}, {"Gain", "dB", -12, 12, 0.0f}});
    addParametric("EnvelopeShaper", Cat::Dynamics, Alg::EnvelopeShaper, {{"Attack", "dB", -18.0f, 18.0f, 4.0f}, {"Release", "dB", -18.0f, 18.0f, 0.0f}, {"Length", "ms", 5.0f, 100.0f, 25.0f}, {"Output", "dB", -12, 12, 0.0f}});
    addParametric("Expander", Cat::Dynamics, Alg::Expander, {{"Threshold", "dB", -60.0f, 0.0f, -32.0f}, {"Ratio", ":1", 1.0f, 8.0f, 2.0f}, {"Attack", "ms", 0.1f, 50.0f, 2.0f}, {"Release", "ms", 20.0f, 1000.0f, 150.0f}});
    addParametric("Flanger", Cat::DelayAndModulation, Alg::Flanger, {{"Rate", "Hz", 0.05f, 5.0f, 0.4f}, {"Depth", "", 0, 1, 0.65f}, {"Feedback", "", -0.85f, 0.85f, 0.5f}, {"Mix", "", 0, 1, 0.5f}});
    addParametric("Gate", Cat::Dynamics, Alg::Gate, {{"Threshold", "dB", -72.0f, 0.0f, -40.0f}, {"Hold", "ms", 1.0f, 500.0f, 35.0f}, {"Range", "dB", -80.0f, 0.0f, -60.0f}, {"Release", "ms", 10.0f, 1000.0f, 120.0f}});
    addParametric("GEQ-10", Cat::EQAndFilters, Alg::GEQ10, geq10Specs);
    addParametric("GEQ-30", Cat::EQAndFilters, Alg::GEQ30, geq30Specs);
    addParametric("Grungelizer", Cat::SaturationAndDistortion, Alg::Grungelizer, {{"Quantize", "bit", 4.0f, 16.0f, 12.0f}, {"Decimate", "x", 1.0f, 8.0f, 2.0f}, {"Crackle", "", 0, 1, 0.35f}, {"Hum", "", 0, 1, 0.1f}});
    addParametric("Limiter", Cat::Dynamics, Alg::Limiter, {{"Input", "dB", -12.0f, 18.0f, 0.0f}, {"Output", "dB", -18.0f, 0.0f, -0.3f}, {"Release", "ms", 5.0f, 500.0f, 50.0f}, {"AutoRel", "", 0, 1, 1.0f}});
    addParametric("Maximizer", Cat::Dynamics, Alg::Maximizer, {{"Optimize", "dB", 0.0f, 18.0f, 4.5f}, {"Output", "dB", -12.0f, 0.0f, -0.1f}, {"SoftClip", "", 0, 1, 0.2f}, {"Mix", "", 0, 1, 1.0f}});
    addParametric("Metalizer", Cat::DelayAndModulation, Alg::Metalizer, {{"Freq", "Hz", 0.1f, 8.0f, 1.2f}, {"Feed", "", 0, 1, 0.7f}, {"Resonance", "", -0.8f, 0.8f, 0.65f}, {"Mix", "", 0, 1, 0.5f}});
    addParametric("MIDI Gate", Cat::Dynamics, Alg::MIDIGate, {{"Attack", "ms", 0.5f, 200.0f, 5.0f}, {"Decay", "ms", 5.0f, 500.0f, 80.0f}, {"Sustain", "", 0, 1, 1.0f}, {"Release", "ms", 5.0f, 1000.0f, 100.0f}});
    addParametric("Mix6to2", Cat::SpatialToolsAndAnalysis, Alg::Mix6to2, {{"Center", "dB", -24.0f, 6.0f, 0.0f}, {"Surround", "dB", -24.0f, 6.0f, -3.0f}, {"LFE", "dB", -48.0f, 6.0f, -12.0f}, {"Master", "dB", -24, 6, 0.0f}});
    addParametric("MixConvert", Cat::SpatialToolsAndAnalysis, Alg::MixConvert, {{"CenterLvl", "dB", -24.0f, 6.0f, 0.0f}, {"SurroundLvl", "dB", -24.0f, 6.0f, -3.0f}, {"LfeGain", "dB", -48.0f, 6.0f, -10.0f}, {"Output", "dB", -24, 6, 0.0f}});
    addParametric("ModMachine", Cat::DelayAndModulation, Alg::ModMachine, {{"Delay", "ms", 10.0f, 1000.0f, 240.0f}, {"ModDepth", "", 0, 1, 0.5f}, {"Feedback", "", 0, 0.9f, 0.45f}, {"Mix", "", 0, 1, 0.35f}});
    addParametric("MonoDelay", Cat::DelayAndModulation, Alg::MonoDelay, {{"Delay", "ms", 1.0f, 2000.0f, 250.0f}, {"Sync", "", 0, 1, 1.0f}, {"Feedback", "", 0, 0.9f, 0.35f}, {"Mix", "", 0, 1, 0.3f}});
    addParametric("MonoToStereo", Cat::SpatialToolsAndAnalysis, Alg::MonoToStereo, {{"Width", "", 0.0f, 1.0f, 0.6f}, {"Delay", "ms", 2.0f, 30.0f, 12.0f}, {"Color", "", 0, 1, 0.5f}, {"MonoCheck", "", 0, 1, 0.0f}});
    addParametric("MultibandCompressor", Cat::Dynamics, Alg::MultibandCompressor, {{"Crossover", "Hz", 80.0f, 4000.0f, 450.0f}, {"Threshold", "dB", -48.0f, 0.0f, -18.0f}, {"Ratio", ":1", 1.0f, 12.0f, 3.0f}, {"Makeup", "dB", -12.0f, 12.0f, 1.5f}});
    addParametric("MultiScope", Cat::SpatialToolsAndAnalysis, Alg::MultiScope, {{"Mode", "", 0, 2, 0.0f}, {"Zoom", "", 0.1f, 10.0f, 1.0f}, {"PeakHold", "", 0, 1, 1.0f}, {"Freeze", "", 0, 1, 0.0f}});
    addParametric("Octaver", Cat::SaturationAndDistortion, Alg::Octaver, {{"Octave1", "", 0, 1, 0.7f}, {"Octave2", "", 0, 1, 0.3f}, {"Direct", "", 0, 1, 0.8f}, {"TrackResp", "", 0, 1, 0.5f}});
    addParametric("Phaser", Cat::DelayAndModulation, Alg::Phaser, {{"Rate", "Hz", 0.05f, 6.0f, 0.5f}, {"Width", "", 0, 1, 0.75f}, {"Feedback", "", -0.8f, 0.8f, 0.45f}, {"Mix", "", 0, 1, 0.5f}});
    addParametric("PingPongDelay", Cat::DelayAndModulation, Alg::PingPongDelay, {{"Delay", "ms", 10.0f, 2000.0f, 375.0f}, {"PanSpread", "", 0, 1, 1.0f}, {"Feedback", "", 0, 0.9f, 0.4f}, {"Mix", "", 0, 1, 0.3f}});

    entries_.push_back({"PitchCorrect", Cat::EQAndFilters, []() { return std::make_shared<PitchCorrectProcessor>(); }});
    entries_.push_back({"REVerence", Cat::Reverb, []() { return std::make_shared<REVerenceProcessor>(); }});

    addParametric("RingModulator", Cat::DelayAndModulation, Alg::RingModulator, {{"OscFreq", "Hz", 20.0f, 2500.0f, 220.0f}, {"Mix", "", 0, 1, 0.6f}, {"EnvMod", "", 0, 1, 0.2f}, {"Wave", "", 0, 3, 0.0f}});
    addParametric("Roomworks", Cat::Reverb, Alg::Roomworks, {{"ReverbTime", "", 0.05f, 1.0f, 0.55f}, {"Damping", "", 0.05f, 0.9f, 0.35f}, {"Mix", "", 0, 1, 0.3f}, {"Diffusion", "", 0, 1, 0.8f}});
    addParametric("Roomworks SE", Cat::Reverb, Alg::RoomworksSE, {{"ReverbTime", "", 0.05f, 1.0f, 0.45f}, {"Damping", "", 0.05f, 0.9f, 0.4f}, {"Mix", "", 0, 1, 0.28f}, {"Diffusion", "", 0, 1, 0.7f}});
    addParametric("Rotary", Cat::DelayAndModulation, Alg::Rotary, {{"SpeedFast", "", 0, 1, 0.0f}, {"Depth", "", 0, 1, 0.8f}, {"Drive", "", 0, 1, 0.2f}, {"Balance", "", 0, 1, 0.5f}});
    addParametric("SMPTE Generator", Cat::SpatialToolsAndAnalysis, Alg::SMPTEGenerator, {{"FrameRate", "fps", 24.0f, 30.0f, 25.0f}, {"Level", "dB", -36.0f, 0.0f, -12.0f}, {"Run", "", 0, 1, 1.0f}, {"SyncToProj", "", 0, 1, 1.0f}});
    addParametric("SoftClipper", Cat::SaturationAndDistortion, Alg::SoftClipper, {{"Input", "", 0, 1, 0.35f}, {"SecondHarm", "", 0, 1, 0.3f}, {"Output", "dB", -18, 6, 0.0f}, {"ThirdHarm", "", 0, 1, 0.3f}});
    addParametric("Standard Channel EQ", Cat::EQAndFilters, Alg::StandardChannelEQ, {{"LowGain", "dB", -24, 24, 0.0f}, {"MidFreq", "Hz", 200, 6000, 1200.0f}, {"MidGain", "dB", -24, 24, 0.0f}, {"HighGain", "dB", -24, 24, 0.0f}});
    addParametric("StepFilter", Cat::EQAndFilters, Alg::StepFilter, {{"BaseCutoff", "Hz", 80.0f, 8000.0f, 600.0f}, {"Resonance", "", 0.5f, 12.0f, 3.5f}, {"StepRate", "Hz", 0.5f, 8.0f, 2.0f}, {"Glide", "", 0, 1, 0.25f}});
    addParametric("StereoDelay", Cat::DelayAndModulation, Alg::StereoDelay, {{"DelayL", "ms", 5.0f, 2000.0f, 250.0f}, {"DelayR", "ms", 5.0f, 2000.0f, 375.0f}, {"Feedback", "", 0, 0.9f, 0.35f}, {"Mix", "", 0, 1, 0.3f}});
    addParametric("StereoEnhancer", Cat::SpatialToolsAndAnalysis, Alg::StereoEnhancer, {{"Width", "", 0.0f, 2.5f, 1.35f}, {"Delay", "", 0, 1, 0.15f}, {"Color", "", 0, 1, 0.5f}, {"MonoOut", "", 0, 1, 0.0f}});
    addParametric("StudioChorus", Cat::DelayAndModulation, Alg::StudioChorus, {{"Rate", "Hz", 0.05f, 5.0f, 0.65f}, {"Depth", "", 0, 1, 0.6f}, {"Crossover", "Hz", 40, 800, 180.0f}, {"Mix", "", 0, 1, 0.45f}});
    addParametric("StudioEQ", Cat::EQAndFilters, Alg::StudioEQ, {{"LowShelf", "dB", -24, 24, 0.0f}, {"MidFreq", "Hz", 100, 10000, 1500.0f}, {"MidGain", "dB", -24, 24, 0.0f}, {"HighShelf", "dB", -24, 24, 0.0f}});
    addParametric("Surround Panner", Cat::SpatialToolsAndAnalysis, Alg::SurroundPanner, {{"Angle", "deg", -180.0f, 180.0f, 0.0f}, {"CenterDist", "", 0, 1, 0.5f}, {"LFE", "dB", -60.0f, 6.0f, -18.0f}, {"Divergence", "", 0, 1, 0.3f}});
    addParametric("Test Generator", Cat::SpatialToolsAndAnalysis, Alg::TestGenerator, {{"Frequency", "Hz", 20.0f, 20000.0f, 1000.0f}, {"Level", "dB", -60.0f, 0.0f, -12.0f}, {"Waveform", "", 0.0f, 2.0f, 0.0f}, {"Active", "", 0, 1, 1.0f}});
    addParametric("ToneBooster", Cat::EQAndFilters, Alg::ToneBooster, {{"Frequency", "Hz", 100.0f, 8000.0f, 1400.0f}, {"Resonance", "", 0.7f, 10.0f, 2.8f}, {"Mix", "", 0, 1, 0.5f}, {"Mode", "", 0, 1, 0.0f}});
    addParametric("Tonic", Cat::EQAndFilters, Alg::Tonic, {{"Cutoff", "Hz", 40.0f, 16000.0f, 900.0f}, {"Resonance", "", 0.5f, 12.0f, 2.5f}, {"EnvFollow", "", 0, 1, 0.4f}, {"LfoDepth", "", 0, 1, 0.3f}});
    addParametric("Tranceformer", Cat::DelayAndModulation, Alg::Tranceformer, {{"PitchHz", "Hz", 30.0f, 1800.0f, 330.0f}, {"Mix", "", 0, 1, 0.5f}, {"LfoRate", "Hz", 0.1f, 10.0f, 1.0f}, {"LfoDepth", "", 0, 1, 0.4f}});
    addParametric("Tremolo", Cat::DelayAndModulation, Alg::Tremolo, {{"Rate", "Hz", 0.2f, 16.0f, 4.5f}, {"Depth", "", 0, 1, 0.65f}, {"Spread", "", 0, 1, 0.2f}, {"Wave", "", 0, 1, 0.0f}});
    addParametric("Tuner", Cat::SpatialToolsAndAnalysis, Alg::Tuner, {{"RefA4", "Hz", 415.0f, 460.0f, 440.0f}, {"MuteAudio", "", 0, 1, 0.0f}, {"StrobeMode", "", 0, 1, 0.0f}, {"Sensitivity", "", 0, 1, 0.8f}});
    addParametric("UV22 HR", Cat::SpatialToolsAndAnalysis, Alg::UV22HR, {{"Resolution", "bit", 16.0f, 24.0f, 16.0f}, {"LowLevel", "", 0, 1, 0.0f}, {"Autoblack", "", 0, 1, 1.0f}, {"Active", "", 0, 1, 1.0f}});
    addParametric("Vibrato", Cat::DelayAndModulation, Alg::Vibrato, {{"Rate", "Hz", 0.2f, 12.0f, 5.0f}, {"Depth", "", 0, 1, 0.35f}, {"Spatial", "", 0, 1, 0.2f}, {"Sync", "", 0, 1, 0.0f}});
    addParametric("VintageCompressor", Cat::Dynamics, Alg::VintageCompressor, {{"Threshold", "dB", -48.0f, 0.0f, -16.0f}, {"Ratio", ":1", 1.5f, 20.0f, 4.0f}, {"Attack", "ms", 0.1f, 50.0f, 5.0f}, {"Release", "ms", 20.0f, 1000.0f, 90.0f}});
    addParametric("VSTDynamics", Cat::Dynamics, Alg::VSTDynamics, {{"CompThresh", "dB", -48.0f, 0.0f, -18.0f}, {"CompRatio", ":1", 1.0f, 16.0f, 3.5f}, {"Attack", "ms", 0.5f, 80.0f, 8.0f}, {"Release", "ms", 20.0f, 800.0f, 100.0f}});
    addParametric("WahWah", Cat::EQAndFilters, Alg::WahWah, {{"PedalFreq", "Hz", 250.0f, 2800.0f, 650.0f}, {"Resonance", "", 1.0f, 12.0f, 4.5f}, {"AutoSens", "", 0, 1, 0.5f}, {"Mix", "", 0, 1, 1.0f}});
}

} // namespace AlphaStudio::Core::DSP
