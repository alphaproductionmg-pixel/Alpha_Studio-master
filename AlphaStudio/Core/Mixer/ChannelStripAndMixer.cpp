#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core {

namespace {
[[nodiscard]] inline float dbToLinear(float db) noexcept {
    if (db <= -120.0f) {
        return 0.0f;
    }
    return std::pow(10.0f, db * 0.05f);
}
} // namespace

// ============================================================================
// ChannelEQ
// ============================================================================
ChannelEQ::ChannelEQ() {
    configureBand(0, false, ChannelEQBandType::LowShelf,       100.0f,   0.0f, 0.707f, false);
    configureBand(1, false, ChannelEQBandType::ParametricPeak, 800.0f,   0.0f, 1.000f, false);
    configureBand(2, false, ChannelEQBandType::ParametricPeak, 3200.0f,  0.0f, 1.000f, false);
    configureBand(3, false, ChannelEQBandType::HighShelf,      10000.0f, 0.0f, 0.707f, false);
}

void ChannelEQ::prepare(double sampleRate) noexcept {
    sampleRate_ = std::max(8000.0, sampleRate);
    resetState();
    for (std::size_t i = 0; i < kNumBands; ++i) {
        recalculateCoefficients(i);
    }
}

void ChannelEQ::resetState() noexcept {
    for (auto& b : bands_) {
        b.z1.fill(0.0f);
        b.z2.fill(0.0f);
    }
}

void ChannelEQ::configureBand(std::size_t bandIdx, bool enabled, ChannelEQBandType type,
                              float freqHz, float gainDb, float q, bool invertGain) noexcept {
    if (bandIdx >= kNumBands) {
        return;
    }
    auto& b = bands_[bandIdx];
    b.enabled = enabled;
    b.type = type;
    b.frequencyHz = std::clamp(freqHz, 20.0f, 20000.0f);
    b.gainDb = std::clamp(gainDb, -24.0f, 24.0f);
    b.qFactor = std::clamp(q, 0.1f, 18.0f);
    b.invertGain = invertGain;
    recalculateCoefficients(bandIdx);
}

void ChannelEQ::setBandInvertGain(std::size_t bandIdx, bool invert) noexcept {
    if (bandIdx < kNumBands) {
        bands_[bandIdx].invertGain = invert;
        recalculateCoefficients(bandIdx);
    }
}

void ChannelEQ::recalculateCoefficients(std::size_t bandIdx) noexcept {
    auto& b = bands_[bandIdx];
    const double sr = std::max(8000.0, sampleRate_);
    const double fc = std::clamp(static_cast<double>(b.frequencyHz), 15.0, sr * 0.48);
    const double effGainDb = b.invertGain ? -static_cast<double>(b.gainDb) : static_cast<double>(b.gainDb);
    const double A = std::pow(10.0, effGainDb / 40.0);
    const double w0 = (2.0 * std::numbers::pi * fc) / sr;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double q = std::max(0.1, static_cast<double>(b.qFactor));
    const double alpha = sinW0 / (2.0 * q);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (b.type) {
        case ChannelEQBandType::ParametricPeak:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosW0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha / A;
            break;
        case ChannelEQBandType::LowShelf: {
            const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW0);
            a2 = (A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha;
            break;
        }
        case ChannelEQBandType::HighShelf: {
            const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW0);
            a2 = (A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha;
            break;
        }
        case ChannelEQBandType::LowCutHPF:
            b0 = (1.0 + cosW0) * 0.5;
            b1 = -(1.0 + cosW0);
            b2 = (1.0 + cosW0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;
        case ChannelEQBandType::HighCutLPF:
            b0 = (1.0 - cosW0) * 0.5;
            b1 = 1.0 - cosW0;
            b2 = (1.0 - cosW0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;
    }

    const double invA0 = 1.0 / a0;
    b.b0 = static_cast<float>(b0 * invA0);
    b.b1 = static_cast<float>(b1 * invA0);
    b.b2 = static_cast<float>(b2 * invA0);
    b.a1 = static_cast<float>(a1 * invA0);
    b.a2 = static_cast<float>(a2 * invA0);
}

float ChannelEQ::magnitudeDbAtFrequency(float freqHz) const noexcept {
    if (bypassed_) {
        return 0.0f;
    }
    const double w = (2.0 * std::numbers::pi * std::clamp<double>(freqHz, 20.0, sampleRate_ * 0.49)) / sampleRate_;
    const double cosW = std::cos(w);
    const double cos2W = std::cos(2.0 * w);
    double totalMagSq = 1.0;

    for (const auto& b : bands_) {
        if (!b.enabled) continue;
        const double num = b.b0 * b.b0 + b.b1 * b.b1 + b.b2 * b.b2 +
                           2.0 * (b.b0 * b.b1 + b.b1 * b.b2) * cosW + 2.0 * b.b0 * b.b2 * cos2W;
        const double den = 1.0 + b.a1 * b.a1 + b.a2 * b.a2 +
                           2.0 * (b.a1 + b.a1 * b.a2) * cosW + 2.0 * b.a2 * cos2W;
        if (den > 1e-18) {
            totalMagSq *= std::max(1e-18, num / den);
        }
    }
    return static_cast<float>(10.0 * std::log10(totalMagSq));
}

void ChannelEQ::processRT(AudioBlock32& buffer, std::uint32_t numFrames) noexcept {
    if (bypassed_ || numFrames == 0) {
        return;
    }
    const std::uint32_t chCount = buffer.numChannels();
    for (auto& b : bands_) {
        if (!b.enabled) {
            continue;
        }
        for (std::uint32_t c = 0; c < chCount; ++c) {
            float* samples = buffer.channel(c);
            if (samples == nullptr) continue;
            float z1 = b.z1[c];
            float z2 = b.z2[c];
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float x = samples[f];
                const float y = b.b0 * x + z1;
                z1 = b.b1 * x - b.a1 * y + z2;
                z2 = b.b2 * x - b.a2 * y;
                samples[f] = y;
            }
            b.z1[c] = z1;
            b.z2[c] = z2;
        }
    }
}

// ============================================================================
// ChannelPanner
// ============================================================================
void ChannelPanner::setPan(float panNormalizedMinus1ToPlus1) noexcept {
    pan_ = std::clamp(panNormalizedMinus1ToPlus1, -1.0f, 1.0f);
}

void ChannelPanner::processRT(AudioBlock32& buffer, std::uint32_t numFrames) const noexcept {
    if (bypassed_ || numFrames == 0) {
        return;
    }
    const std::uint32_t ch = buffer.numChannels();
    if (ch < 2U) {
        return;
    }

    const float norm = (pan_ + 1.0f) * 0.5f; // [0..1]
    float gainL = 1.0f;
    float gainR = 1.0f;
    switch (lawMode_) {
        case PanLawMode::EqualPowerSinCos: {
            const float angle = norm * (std::numbers::pi_v<float> * 0.5f);
            gainL = std::cos(angle);
            gainR = std::sin(angle);
            break;
        }
        case PanLawMode::LinearMinus6dB:
            gainL = 1.0f - norm;
            gainR = norm;
            break;
        case PanLawMode::LinearZeroDb:
            gainL = (pan_ <= 0.0f) ? 1.0f : (1.0f - pan_);
            gainR = (pan_ >= 0.0f) ? 1.0f : (1.0f + pan_);
            break;
    }

    float* left = buffer.channel(0);
    float* right = buffer.channel(1);
    if (ch == 2U) {
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            left[f] *= gainL;
            right[f] *= gainR;
        }
    } else if (ch == 6U) {
        // True Surround 5.1 (L=0, R=1, C=2, LFE=3, Ls=4, Rs=5)
        const float frontWeight = std::clamp((frontRear_ + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float rearWeight = 1.0f - frontWeight;
        float* center = buffer.channel(2);
        float* lfe = buffer.channel(3);
        float* ls = buffer.channel(4);
        float* rs = buffer.channel(5);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float lIn = left[f];
            const float rIn = right[f];
            const float monoSum = (lIn + rIn) * 0.5f;
            left[f] = lIn * gainL * frontWeight;
            right[f] = rIn * gainR * frontWeight;
            center[f] = center[f] + monoSum * (1.0f - std::abs(pan_)) * 0.35f * frontWeight;
            lfe[f] = lfe[f] + monoSum * lfeSend_;
            ls[f] = ls[f] * rearWeight + lIn * gainL * rearWeight;
            rs[f] = rs[f] * rearWeight + rIn * gainR * rearWeight;
        }
    }
}

// ============================================================================
// ChannelStrip (Immutable Signal Chain)
// ============================================================================
ChannelStrip::ChannelStrip(std::uint32_t channelId, std::string name,
                           MixerChannelKind kind, SpeakerConfig config)
    : channelId_(channelId),
      name_(std::move(name)),
      kind_(kind),
      config_(config),
      pflTapBuffer_(config, AudioBlock32::kDefaultMaxFrames),
      aflTapBuffer_(config, AudioBlock32::kDefaultMaxFrames) {
    for (auto& b : auxTapBuffers_) {
        b.prepare(config, AudioBlock32::kDefaultMaxFrames);
    }
    for (auto& b : studioCueTapBuffers_) {
        b.prepare(SpeakerConfig::Stereo, AudioBlock32::kDefaultMaxFrames);
    }
}

void ChannelStrip::prepare(double sampleRate, std::uint32_t maxBlockFrames) {
    sampleRate_ = std::max(8000.0, sampleRate);
    maxBlockFrames_ = std::max(16U, maxBlockFrames);
    eq_.prepare(sampleRate_);
    pflTapBuffer_.prepare(config_, maxBlockFrames_);
    aflTapBuffer_.prepare(config_, maxBlockFrames_);
    for (auto& b : auxTapBuffers_) {
        b.prepare(config_, maxBlockFrames_);
    }
    for (auto& b : studioCueTapBuffers_) {
        b.prepare(SpeakerConfig::Stereo, maxBlockFrames_);
    }
    for (auto& ins : inserts_) {
        if (ins.processor) {
            ins.processor->prepare(sampleRate_, maxBlockFrames_);
        }
    }
}

void ChannelStrip::setInsert(std::size_t slotIdx, std::shared_ptr<IAudioInsertProcessor> proc, bool enabled) {
    if (slotIdx >= kNumAudioInserts) {
        return;
    }
    inserts_[slotIdx].processor = std::move(proc);
    inserts_[slotIdx].enabled = enabled && (inserts_[slotIdx].processor != nullptr);
    inserts_[slotIdx].bypassed = false;
    if (inserts_[slotIdx].processor) {
        inserts_[slotIdx].processor->prepare(sampleRate_, maxBlockFrames_);
    }
}

bool ChannelStrip::enableSidechainOnInsert(std::size_t slotIdx, std::uint32_t sourceChannelId,
                                           const AudioBlock32* sourceTapBuffer) noexcept {
    if (slotIdx >= kNumAudioInserts || !inserts_[slotIdx].processor) {
        return false;
    }
    if (!inserts_[slotIdx].processor->supportsSidechain()) {
        return false;
    }
    inserts_[slotIdx].sidechainActive = (sourceTapBuffer != nullptr);
    inserts_[slotIdx].sidechainSourceChannelId = sourceChannelId;
    inserts_[slotIdx].externalSidechainTap = sourceTapBuffer;
    return inserts_[slotIdx].sidechainActive;
}

void ChannelStrip::configureAuxSend(std::size_t idx, bool enabled, bool preFader,
                                    std::uint32_t targetBusId, float levelDb) noexcept {
    if (idx < kNumAuxSends) {
        auxSends_[idx] = AuxSendSlot{enabled, preFader, targetBusId, levelDb};
    }
}

void ChannelStrip::configureStudioSend(std::size_t cueIdx, bool enabled, bool preFader,
                                       float levelDb, float pan) noexcept {
    if (cueIdx < kNumStudioCueSends) {
        studioSends_[cueIdx] = StudioCueSendSlot{
            enabled, preFader, static_cast<std::uint32_t>(cueIdx), levelDb, std::clamp(pan, -1.0f, 1.0f)
        };
    }
}

ChannelSettingsSnapshot ChannelStrip::captureSettings() const noexcept {
    return ChannelSettingsSnapshot{
        .phaseReversed = phaseReversed_,
        .inputGainDb = inputGainDb_,
        .faderDb = faderDb_,
        .muted = muted_,
        .pan = panner_.pan(),
        .panBypassed = panner_.isBypassed(),
        .eqState = eq_,
        .auxSends = auxSends_,
        .studioSends = studioSends_
    };
}

void ChannelStrip::applySettings(const ChannelSettingsSnapshot& snap) noexcept {
    phaseReversed_ = snap.phaseReversed;
    inputGainDb_ = snap.inputGainDb;
    faderDb_ = snap.faderDb;
    muted_ = snap.muted;
    panner_.setPan(snap.pan);
    panner_.setBypassed(snap.panBypassed);
    eq_ = snap.eqState;
    eq_.prepare(sampleRate_);
    auxSends_ = snap.auxSends;
    studioSends_ = snap.studioSends;
}

void ChannelStrip::processStripRT(AudioBlock32& mainIo,
                                  std::uint32_t numFrames,
                                  bool anyOtherChannelSoloed,
                                  bool bypassInsertsAndEQ) noexcept {
    // 1. IN -> PhaseReverse & InputGain (Trim)
    const float polarity = phaseReversed_ ? -1.0f : 1.0f;
    const float trimGain = polarity * dbToLinear(inputGainDb_);
    mainIo.applyGainRT(numFrames, trimGain);

    // 2. Meter (Pre) & PFL immediately after IN (post-Trim/PhaseReverse, pre-Inserts)
    prePeakL_.store(mainIo.computePeakRT(0, numFrames), std::memory_order_relaxed);
    prePeakR_.store(mainIo.computePeakRT(mainIo.numChannels() > 1 ? 1 : 0, numFrames),
                    std::memory_order_relaxed);
    pflTapBuffer_.copyFromRT(mainIo, numFrames);

    // 3. Inserts 1 to 6 (indices 0..5, strictly fixed in Pre-Fader)
    if (!bypassInsertsAndEQ) {
        for (std::size_t slot = 0; slot < kNumPreFaderInserts; ++slot) {
            auto& ins = inserts_[slot];
            if (ins.enabled && !ins.bypassed && ins.processor != nullptr) {
                const AudioBlock32* sc = ins.sidechainActive ? ins.externalSidechainTap : nullptr;
                ins.processor->processAudioRT(mainIo, sc, numFrames);
            }
        }
    }

    // 4. ChannelEQ (with per-band Gain Inverse)
    if (!bypassInsertsAndEQ) {
        eq_.processRT(mainIo, numFrames);
    }

    // 5. Aux Sends (Pre) & Studio Sends (Pre) — always after Inserts 1..6 and ChannelEQ
    for (std::size_t s = 0; s < kNumAuxSends; ++s) {
        auxTapBuffers_[s].clearRT(numFrames);
        if (auxSends_[s].enabled && auxSends_[s].preFader) {
            auxTapBuffers_[s].accumulateFromRT(mainIo, numFrames, dbToLinear(auxSends_[s].levelDb));
        }
    }
    for (std::size_t c = 0; c < kNumStudioCueSends; ++c) {
        studioCueTapBuffers_[c].clearRT(numFrames);
        if (studioSends_[c].enabled && studioSends_[c].preFader) {
            studioCueTapBuffers_[c].accumulateFromRT(mainIo, numFrames, dbToLinear(studioSends_[c].levelDb));
        }
    }

    // 6. Level (Main Fader), Mute, Solo
    const bool effectivelyMuted = muted_ || (anyOtherChannelSoloed && !soloed_ && !soloDefeat_);
    const float faderLinear = effectivelyMuted ? 0.0f : dbToLinear(faderDb_);
    mainIo.applyGainRT(numFrames, faderLinear);

    // 7. Inserts 7 and 8 (indices 6..7, strictly fixed in Post-Fader)
    if (!bypassInsertsAndEQ) {
        for (std::size_t slot = kNumPreFaderInserts; slot < kNumAudioInserts; ++slot) {
            auto& ins = inserts_[slot];
            if (ins.enabled && !ins.bypassed && ins.processor != nullptr) {
                const AudioBlock32* sc = ins.sidechainActive ? ins.externalSidechainTap : nullptr;
                ins.processor->processAudioRT(mainIo, sc, numFrames);
            }
        }
    }

    // 8. Aux Sends (Post) & Studio Sends (Post)
    for (std::size_t s = 0; s < kNumAuxSends; ++s) {
        if (auxSends_[s].enabled && !auxSends_[s].preFader) {
            auxTapBuffers_[s].accumulateFromRT(mainIo, numFrames, dbToLinear(auxSends_[s].levelDb));
        }
    }
    for (std::size_t c = 0; c < kNumStudioCueSends; ++c) {
        if (studioSends_[c].enabled && !studioSends_[c].preFader) {
            studioCueTapBuffers_[c].accumulateFromRT(mainIo, numFrames, dbToLinear(studioSends_[c].levelDb));
        }
    }

    // 9. Meter (Post 1) strictly BEFORE Panner
    post1PeakL_.store(mainIo.computePeakRT(0, numFrames), std::memory_order_relaxed);
    post1PeakR_.store(mainIo.computePeakRT(mainIo.numChannels() > 1 ? 1 : 0, numFrames),
                      std::memory_order_relaxed);

    // 10. Pan (Equal Power & Bypass) -> OUT
    panner_.processRT(mainIo, numFrames);

    // 11. Meter (Post 2) & AFL strictly AFTER Panner
    post2PeakL_.store(mainIo.computePeakRT(0, numFrames), std::memory_order_relaxed);
    post2PeakR_.store(mainIo.computePeakRT(mainIo.numChannels() > 1 ? 1 : 0, numFrames),
                      std::memory_order_relaxed);
    aflTapBuffer_.copyFromRT(mainIo, numFrames);
}

ChannelMeterSnapshot ChannelStrip::meterSnapshot() const noexcept {
    return ChannelMeterSnapshot{
        .prePeakL = prePeakL_.load(std::memory_order_relaxed),
        .prePeakR = prePeakR_.load(std::memory_order_relaxed),
        .post1PrePanPeakL = post1PeakL_.load(std::memory_order_relaxed),
        .post1PrePanPeakR = post1PeakR_.load(std::memory_order_relaxed),
        .post2PostPanPeakL = post2PeakL_.load(std::memory_order_relaxed),
        .post2PostPanPeakR = post2PeakR_.load(std::memory_order_relaxed)
    };
}

// ============================================================================
// VSTMixer
// ============================================================================
VSTMixer::VSTMixer(double sampleRate, std::uint32_t maxBlockFrames)
    : sampleRate_(sampleRate), maxBlockFrames_(maxBlockFrames) {
    savedViews_[0] = MixerViewConfiguration{.viewName = "View 1: Full Console"};
    savedViews_[1] = MixerViewConfiguration{
        .viewName = "View 2: Audio & Instruments",
        .showInputChannels = false,
        .showMIDIChannels = false,
        .showGroupChannels = false,
        .showFXReturns = false
    };
    savedViews_[2] = MixerViewConfiguration{
        .viewName = "View 3: Subgroups, FX & Masters",
        .showInputChannels = false,
        .showAudioChannels = false,
        .showInstrumentChannels = false,
        .showMIDIChannels = false,
        .extendedUpperView = true
    };
}

ChannelStrip* VSTMixer::addInputChannel(const std::string& name, SpeakerConfig cfg) {
    if (numInputsCount_ >= kMaxPhysicalIO) {
        return nullptr;
    }
    ++numInputsCount_;
    auto strip = std::make_unique<ChannelStrip>(nextChannelId_++, name, MixerChannelKind::InputChannel, cfg);
    strip->prepare(sampleRate_, maxBlockFrames_);
    strip->setVisibleInProjectWindow(true);
    channels_.push_back(std::move(strip));
    return channels_.back().get();
}

ChannelStrip* VSTMixer::addAudioOrInstrumentChannel(const std::string& name, bool isInstrument, SpeakerConfig cfg) {
    auto strip = std::make_unique<ChannelStrip>(
        nextChannelId_++, name,
        isInstrument ? MixerChannelKind::InstrumentChannel : MixerChannelKind::AudioChannel, cfg);
    strip->prepare(sampleRate_, maxBlockFrames_);
    channels_.push_back(std::move(strip));
    return channels_.back().get();
}

ChannelStrip* VSTMixer::addGroupChannel(const std::string& name, SpeakerConfig cfg) {
    if (numGroupsCount_ >= kMaxGroupChannels) {
        return nullptr;
    }
    ++numGroupsCount_;
    auto strip = std::make_unique<ChannelStrip>(nextChannelId_++, name, MixerChannelKind::GroupChannel, cfg);
    strip->prepare(sampleRate_, maxBlockFrames_);
    channels_.push_back(std::move(strip));
    return channels_.back().get();
}

ChannelStrip* VSTMixer::addFXReturnChannel(const std::string& name, SpeakerConfig cfg) {
    if (numFXReturnsCount_ >= kMaxFXReturns) {
        return nullptr;
    }
    ++numFXReturnsCount_;
    auto strip = std::make_unique<ChannelStrip>(nextChannelId_++, name, MixerChannelKind::FXReturnChannel, cfg);
    strip->prepare(sampleRate_, maxBlockFrames_);
    channels_.push_back(std::move(strip));
    return channels_.back().get();
}

ChannelStrip* VSTMixer::addOutputChannel(const std::string& name, SpeakerConfig cfg) {
    if (numOutputsCount_ >= kMaxPhysicalIO) {
        return nullptr;
    }
    ++numOutputsCount_;
    auto strip = std::make_unique<ChannelStrip>(nextChannelId_++, name, MixerChannelKind::OutputChannel, cfg);
    strip->prepare(sampleRate_, maxBlockFrames_);
    strip->setSoloDefeat(true);
    channels_.push_back(std::move(strip));
    return channels_.back().get();
}

ChannelStrip* VSTMixer::findChannel(std::uint32_t channelId) noexcept {
    for (auto& ch : channels_) {
        if (ch->channelId() == channelId) return ch.get();
    }
    return nullptr;
}

const ChannelStrip* VSTMixer::findChannel(std::uint32_t channelId) const noexcept {
    for (const auto& ch : channels_) {
        if (ch->channelId() == channelId) return ch.get();
    }
    return nullptr;
}

bool VSTMixer::routeSidechain(std::uint32_t sourceChannelId,
                              std::uint32_t targetChannelId,
                              std::size_t targetInsertSlotIdx) noexcept {
    if (sourceChannelId == targetChannelId) {
        return false;
    }
    const ChannelStrip* src = findChannel(sourceChannelId);
    ChannelStrip* dst = findChannel(targetChannelId);
    if (!src || !dst) {
        return false;
    }
    return dst->enableSidechainOnInsert(targetInsertSlotIdx, sourceChannelId, &src->aflTapBuffer());
}

void VSTMixer::saveMixerView(std::size_t slot0To2, const MixerViewConfiguration& cfg) {
    if (slot0To2 < kNumSavedViews) {
        savedViews_[slot0To2] = cfg;
    }
}

const MixerViewConfiguration& VSTMixer::recallMixerView(std::size_t slot0To2) noexcept {
    activeViewIdx_ = std::min<std::size_t>(slot0To2, kNumSavedViews - 1);
    return savedViews_[activeViewIdx_];
}

void VSTMixer::copyChannelSettings(std::uint32_t sourceChannelId) {
    if (const auto* ch = findChannel(sourceChannelId)) {
        clipboard_ = ch->captureSettings();
        hasClipboard_ = true;
    }
}

std::size_t VSTMixer::pasteSettingsToChannelGroup(std::span<const std::uint32_t> targetChannelIds) {
    if (!hasClipboard_) {
        return 0;
    }
    std::size_t applied = 0;
    for (std::uint32_t id : targetChannelIds) {
        if (auto* ch = findChannel(id)) {
            ch->applySettings(clipboard_);
            ++applied;
        }
    }
    return applied;
}

void VSTMixer::processConsoleRT(AudioBlock32& mainMixOut, std::uint32_t numFrames) noexcept {
    mainMixOut.clearRT(numFrames);
    bool anySolo = false;
    for (const auto& ch : channels_) {
        if (ch->isSoloed()) {
            anySolo = true;
            break;
        }
    }

    for (auto& ch : channels_) {
        if (ch->kind() == MixerChannelKind::OutputChannel) {
            continue;
        }
        AudioBlock32 work = ch->aflTapBuffer();
        ch->processStripRT(work, numFrames, anySolo, false);
        if (ch->kind() != MixerChannelKind::InputChannel) {
            mainMixOut.accumulateFromRT(work, numFrames, 1.0f);
        }
    }

    for (auto& ch : channels_) {
        if (ch->kind() == MixerChannelKind::OutputChannel) {
            ch->processStripRT(mainMixOut, numFrames, false, false);
            break;
        }
    }
}

} // namespace AlphaStudio::Core
