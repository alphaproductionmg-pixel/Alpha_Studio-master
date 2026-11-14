#include "AlphaStudio/Core/DSP/REVerenceConvolution.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// Radix2FFT (Cooley-Tukey In-Place Complex FFT)
// ============================================================================
void Radix2FFT::transformInPlace(std::span<std::complex<float>> data, bool inverse) noexcept {
    const std::size_t N = data.size();
    if (N <= 1 || (N & (N - 1)) != 0) {
        return;
    }

    // Bit-reversal permutation
    std::size_t j = 0;
    for (std::size_t i = 1; i < N; ++i) {
        std::size_t bit = N >> 1U;
        while ((j & bit) != 0U) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    const float sign = inverse ? 1.0f : -1.0f;
    for (std::size_t len = 2; len <= N; len <<= 1U) {
        const float angle = sign * (2.0f * std::numbers::pi_v<float>) / static_cast<float>(len);
        const std::complex<float> wLen(std::cos(angle), std::sin(angle));
        const std::size_t half = len >> 1U;

        for (std::size_t i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
                w *= wLen;
            }
        }
    }

    if (inverse) {
        const float invN = 1.0f / static_cast<float>(N);
        for (auto& c : data) {
            c *= invN;
        }
    }
}

// ============================================================================
// REVerenceProcessor
// ============================================================================
REVerenceProcessor::REVerenceProcessor()
    : wetScratchBlock_(SpeakerConfig::Stereo, AudioBlock32::kDefaultMaxFrames) {
    fdlLeft_.resize(maxPartitions_);
    fdlRight_.resize(maxPartitions_);
    loadImpulseResponse(synthesizeFactoryHallIR(48000.0, 0.45, true));
}

TrueStereoImpulseResponse REVerenceProcessor::synthesizeFactoryHallIR(
    double sampleRate, double reverbTimeSeconds, bool trueStereo) {
    TrueStereoImpulseResponse ir;
    ir.sampleRate = std::max(8000.0, sampleRate);
    ir.isTrueStereo4Ch = trueStereo;
    const auto len = static_cast<std::size_t>(
        std::clamp(reverbTimeSeconds * ir.sampleRate, 512.0, static_cast<double>(kPartitionSize * 60)));

    ir.irLL.resize(len, 0.0f);
    ir.irLR.resize(len, 0.0f);
    ir.irRL.resize(len, 0.0f);
    ir.irRR.resize(len, 0.0f);

    std::uint32_t rng = 0x9E3779B9U;
    auto nextWhite = [&rng]() noexcept -> float {
        rng = rng * 1664525U + 1013904223U;
        return (static_cast<float>(rng >> 8U) / 8388608.0f) - 1.0f;
    };

    const double tau = std::max(0.05, reverbTimeSeconds) / 6.907755; // -60 dB RT60 decay constant
    for (std::size_t n = 0; n < len; ++n) {
        const double t = static_cast<double>(n) / ir.sampleRate;
        const float env = static_cast<float>(std::exp(-t / tau)) * 0.06f;
        ir.irLL[n] = nextWhite() * env;
        ir.irRR[n] = nextWhite() * env;
        ir.irLR[n] = trueStereo ? (nextWhite() * env * 0.65f) : 0.0f;
        ir.irRL[n] = trueStereo ? (nextWhite() * env * 0.65f) : 0.0f;
    }
    // Distinct early reflection spikes
    if (len > 400) {
        ir.irLL[64] += 0.35f;
        ir.irRR[79] += 0.35f;
        if (trueStereo) {
            ir.irLR[142] += 0.22f;
            ir.irRL[168] += 0.22f;
        }
    }
    return ir;
}

REVerenceProcessor::PartitionedBank REVerenceProcessor::buildPartitionedSpectra(
    const TrueStereoImpulseResponse& ir) {
    PartitionedBank bank;
    bank.trueStereo = ir.isTrueStereo4Ch;
    const std::size_t irLen = ir.irLL.size();
    bank.numPartitions = std::min<std::size_t>(64U, (irLen + kPartitionSize - 1U) / kPartitionSize);

    auto partitionChannel = [&](const std::vector<float>& src,
                                std::vector<std::array<std::complex<float>, kFftSize>>& dst) {
        dst.resize(bank.numPartitions);
        for (std::size_t p = 0; p < bank.numPartitions; ++p) {
            dst[p].fill(std::complex<float>(0.0f, 0.0f));
            const std::size_t start = p * kPartitionSize;
            for (std::size_t k = 0; k < kPartitionSize && (start + k) < src.size(); ++k) {
                dst[p][k] = std::complex<float>(src[start + k], 0.0f);
            }
            Radix2FFT::transformInPlace(dst[p], false);
        }
    };

    partitionChannel(ir.irLL, bank.H_LL);
    partitionChannel(ir.irRR, bank.H_RR);
    if (bank.trueStereo) {
        partitionChannel(ir.irLR, bank.H_LR);
        partitionChannel(ir.irRL, bank.H_RL);
    }
    return bank;
}

void REVerenceProcessor::loadImpulseResponse(const TrueStereoImpulseResponse& ir) {
    if (parameterFading_ && activeBank_.numPartitions > 0) {
        fadingOldBank_ = activeBank_;
        crossfadeProgress_ = 0.0f;
    } else {
        crossfadeProgress_ = 1.0f;
    }
    activeBank_ = buildPartitionedSpectra(ir);
    if (activeBank_.numPartitions > maxPartitions_) {
        maxPartitions_ = activeBank_.numPartitions;
        fdlLeft_.resize(maxPartitions_);
        fdlRight_.resize(maxPartitions_);
    }
}

void REVerenceProcessor::prepare(double sampleRate, std::uint32_t maxBlockFrames) {
    sampleRate_ = std::max(8000.0, sampleRate);
    preDelayLine_.prepare(2);
    setPreDelayMs(preDelayMs_);
    wetScratchBlock_.prepare(SpeakerConfig::Stereo, maxBlockFrames);
    postEQ_.prepare(sampleRate_);
    postEQ_.configureBand(3, true, ChannelEQBandType::HighShelf, 8000.0f, -2.0f, 0.707f);
    reset();
}

void REVerenceProcessor::reset() noexcept {
    for (auto& s : fdlLeft_) s.fill({0.0f, 0.0f});
    for (auto& s : fdlRight_) s.fill({0.0f, 0.0f});
    timeOverlapInL_.fill(0.0f);
    timeOverlapInR_.fill(0.0f);
    convOutBlockL_.fill(0.0f);
    convOutBlockR_.fill(0.0f);
    fdlPos_ = 0;
    frameInPartition_ = 0;
    preDelayLine_.reset();
    postEQ_.resetState();
}

void REVerenceProcessor::setDryWetMix(float wet01, bool forceIgnoreLock) noexcept {
    if (dryWetLocked_ && !forceIgnoreLock) {
        return; // DryWetLock protects 100% Wet mix on Group / FX channels!
    }
    wetMix_ = std::clamp(wet01, 0.0f, 1.0f);
}

void REVerenceProcessor::setPreDelayMs(float ms) noexcept {
    preDelayMs_ = std::clamp(ms, 0.0f, 250.0f);
    preDelaySamples_ = static_cast<std::uint32_t>((preDelayMs_ * 0.001) * sampleRate_);
    preDelayLine_.setDelaySamples(preDelaySamples_);
}

void REVerenceProcessor::convolvePartitionBlockRT() noexcept {
    for (std::size_t k = 0; k < kFftSize; ++k) {
        fftWorkL_[k] = {timeOverlapInL_[k], 0.0f};
        fftWorkR_[k] = {timeOverlapInR_[k], 0.0f};
    }
    Radix2FFT::transformInPlace(fftWorkL_, false);
    Radix2FFT::transformInPlace(fftWorkR_, false);

    fdlLeft_[fdlPos_] = fftWorkL_;
    fdlRight_[fdlPos_] = fftWorkR_;

    auto evaluateBank = [&](const PartitionedBank& bank,
                            std::array<std::complex<float>, kFftSize>& outSpecL,
                            std::array<std::complex<float>, kFftSize>& outSpecR) noexcept {
        outSpecL.fill({0.0f, 0.0f});
        outSpecR.fill({0.0f, 0.0f});
        for (std::size_t p = 0; p < bank.numPartitions; ++p) {
            const std::size_t idx = (fdlPos_ + maxPartitions_ - p) % maxPartitions_;
            const auto& XL = fdlLeft_[idx];
            const auto& XR = fdlRight_[idx];
            if (bank.trueStereo) {
                for (std::size_t bin = 0; bin < kFftSize; ++bin) {
                    outSpecL[bin] += XL[bin] * bank.H_LL[p][bin] + XR[bin] * bank.H_RL[p][bin];
                    outSpecR[bin] += XL[bin] * bank.H_LR[p][bin] + XR[bin] * bank.H_RR[p][bin];
                }
            } else {
                for (std::size_t bin = 0; bin < kFftSize; ++bin) {
                    outSpecL[bin] += XL[bin] * bank.H_LL[p][bin];
                    outSpecR[bin] += XR[bin] * bank.H_RR[p][bin];
                }
            }
        }
        Radix2FFT::transformInPlace(outSpecL, true);
        Radix2FFT::transformInPlace(outSpecR, true);
    };

    evaluateBank(activeBank_, accumOutL_, accumOutR_);

    // Overlap-Save keeps the second half [kPartitionSize .. kFftSize - 1] (discarding circular wrap)
    if (parameterFading_ && crossfadeProgress_ < 1.0f && fadingOldBank_.numPartitions > 0) {
        std::array<std::complex<float>, kFftSize> oldL{};
        std::array<std::complex<float>, kFftSize> oldR{};
        evaluateBank(fadingOldBank_, oldL, oldR);
        for (std::size_t n = 0; n < kPartitionSize; ++n) {
            const float a = std::min(1.0f, crossfadeProgress_);
            convOutBlockL_[n] = oldL[kPartitionSize + n].real() * (1.0f - a) +
                                accumOutL_[kPartitionSize + n].real() * a;
            convOutBlockR_[n] = oldR[kPartitionSize + n].real() * (1.0f - a) +
                                accumOutR_[kPartitionSize + n].real() * a;
            crossfadeProgress_ = std::min(1.0f, crossfadeProgress_ + (1.0f / 2048.0f));
        }
    } else {
        for (std::size_t n = 0; n < kPartitionSize; ++n) {
            convOutBlockL_[n] = accumOutL_[kPartitionSize + n].real();
            convOutBlockR_[n] = accumOutR_[kPartitionSize + n].real();
        }
    }

    // Shift overlap-save input history: first half <- second half
    std::copy(timeOverlapInL_.begin() + kPartitionSize, timeOverlapInL_.end(), timeOverlapInL_.begin());
    std::copy(timeOverlapInR_.begin() + kPartitionSize, timeOverlapInR_.end(), timeOverlapInR_.begin());
    fdlPos_ = (fdlPos_ + 1U) % maxPartitions_;
}

void REVerenceProcessor::processAudioRT(AudioBlock32& ioBuffer,
                                        const AudioBlock32* /*sidechainIn*/,
                                        std::uint32_t numFrames) noexcept {
    if (ioBuffer.numChannels() == 0 || numFrames == 0) {
        return;
    }
    float* chL = ioBuffer.channel(0);
    float* chR = (ioBuffer.numChannels() > 1) ? ioBuffer.channel(1) : chL;
    float* wetL = wetScratchBlock_.channel(0);
    float* wetR = wetScratchBlock_.channel(1);

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        timeOverlapInL_[kPartitionSize + frameInPartition_] = chL[f];
        timeOverlapInR_[kPartitionSize + frameInPartition_] = chR[f];

        wetL[f] = convOutBlockL_[frameInPartition_];
        wetR[f] = convOutBlockR_[frameInPartition_];

        if (++frameInPartition_ >= kPartitionSize) {
            convolvePartitionBlockRT();
            frameInPartition_ = 0;
        }
    }

    if (preDelaySamples_ > 0) {
        preDelayLine_.processInPlaceRT(wetScratchBlock_.channelPointers(), 2U, numFrames);
    }
    postEQ_.processRT(wetScratchBlock_, numFrames);

    const float dry = 1.0f - wetMix_;
    const float wet = wetMix_;
    for (std::uint32_t f = 0; f < numFrames; ++f) {
        const float lDry = chL[f];
        const float rDry = chR[f];
        chL[f] = lDry * dry + wetL[f] * wet;
        if (chR != chL) {
            chR[f] = rDry * dry + wetR[f] * wet;
        }
    }
}

void REVerenceProcessor::setParameterNormalized(std::uint32_t paramId, float normValue) noexcept {
    const float v = std::clamp(normValue, 0.0f, 1.0f);
    switch (paramId) {
        case 0: setDryWetMix(v, false); break;
        case 1: setPreDelayMs(v * 200.0f); break;
        case 2: erTailBalance_ = v; break;
        case 3: parameterFading_ = (v >= 0.5f); break;
        case 4: dryWetLocked_ = (v >= 0.5f); break;
        default: break;
    }
}

float REVerenceProcessor::getParameterNormalized(std::uint32_t paramId) const noexcept {
    switch (paramId) {
        case 0: return wetMix_;
        case 1: return preDelayMs_ / 200.0f;
        case 2: return erTailBalance_;
        case 3: return parameterFading_ ? 1.0f : 0.0f;
        case 4: return dryWetLocked_ ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}

const char* REVerenceProcessor::parameterName(std::uint32_t paramId) const noexcept {
    switch (paramId) {
        case 0: return "Mix";
        case 1: return "PreDelay";
        case 2: return "ER/Tail";
        case 3: return "ParamFading";
        case 4: return "DryWetLock";
        default: return "";
    }
}

} // namespace AlphaStudio::Core::DSP
