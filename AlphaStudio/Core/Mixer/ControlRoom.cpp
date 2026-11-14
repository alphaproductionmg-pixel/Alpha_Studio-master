#include "AlphaStudio/Core/Mixer/ControlRoom.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

namespace {
[[nodiscard]] inline float dbToGain(float db) noexcept {
    return (db <= -120.0f) ? 0.0f : std::pow(10.0f, db * 0.05f);
}
} // namespace

// ============================================================================
// ControlRoomChannel
// ============================================================================
ControlRoomChannel::ControlRoomChannel(std::string name, ControlRoomChannelType type,
                                       SpeakerConfig speakerCfg, std::vector<int> hwOutputPorts)
    : name_(std::move(name)),
      type_(type),
      speakerConfig_(speakerCfg),
      hwOutputPorts_(std::move(hwOutputPorts)),
      outputBuffer_(speakerCfg, AudioBlock32::kDefaultMaxFrames) {}

void ControlRoomChannel::prepare(double sampleRate, std::uint32_t maxBlockFrames) {
    sampleRate_ = std::max(8000.0, sampleRate);
    maxFrames_ = std::max(16U, maxBlockFrames);
    outputBuffer_.prepare(speakerConfig_, maxFrames_);
    for (auto& ins : inserts_) {
        if (ins.processor) {
            ins.processor->prepare(sampleRate_, maxFrames_);
        }
    }
}

void ControlRoomChannel::setInsert(std::size_t slot0To7,
                                   std::shared_ptr<IAudioInsertProcessor> proc,
                                   bool enabled) {
    if (slot0To7 >= kTotalInserts) {
        return;
    }
    inserts_[slot0To7].processor = std::move(proc);
    inserts_[slot0To7].enabled = enabled && (inserts_[slot0To7].processor != nullptr);
    inserts_[slot0To7].bypassed = false;
    if (inserts_[slot0To7].processor) {
        inserts_[slot0To7].processor->prepare(sampleRate_, maxFrames_);
    }
}

void ControlRoomChannel::applyDownmixPresetRT(AudioBlock32& buf, DownmixPreset preset,
                                              std::uint32_t numFrames) noexcept {
    const std::uint32_t ch = buf.numChannels();
    if (preset == DownmixPreset::Direct1To1 || ch < 2U) {
        return;
    }
    constexpr float kInvSqrt2 = 0.70710678f;

    if (preset == DownmixPreset::Surround51ToStereo && ch >= 6U) {
        float* L = buf.channel(0);
        float* R = buf.channel(1);
        float* C = buf.channel(2);
        float* LFE = buf.channel(3);
        float* Ls = buf.channel(4);
        float* Rs = buf.channel(5);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float lDown = L[f] + kInvSqrt2 * C[f] + kInvSqrt2 * Ls[f];
            const float rDown = R[f] + kInvSqrt2 * C[f] + kInvSqrt2 * Rs[f];
            L[f] = lDown;
            R[f] = rDown;
            C[f] = 0.0f;
            LFE[f] = 0.0f;
            Ls[f] = 0.0f;
            Rs[f] = 0.0f;
        }
    } else if (preset == DownmixPreset::StereoToMono) {
        float* L = buf.channel(0);
        float* R = buf.channel(1);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float m = (L[f] + R[f]) * kInvSqrt2;
            L[f] = m;
            R[f] = m;
        }
    } else if (preset == DownmixPreset::LeftMinusRightDiff) {
        float* L = buf.channel(0);
        float* R = buf.channel(1);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float diff = (L[f] - R[f]) * kInvSqrt2;
            L[f] = diff;
            R[f] = diff;
        }
    }
}

void ControlRoomChannel::processChannelRT(const AudioBlock32& sourceSignal,
                                          const AudioBlock32* talkbackSignal,
                                          float talkbackDimGainLinear,
                                          float globalDimGainLinear,
                                          DownmixPreset downmix,
                                          std::uint32_t numFrames) noexcept {
    outputBuffer_.copyFromRT(sourceSignal, numFrames);

    if (type_ == ControlRoomChannelType::MonitorSpeakerSet) {
        applyDownmixPresetRT(outputBuffer_, downmix, numFrames);
    }

    // 6 Pre-Fader Inserts (indices 0..5)
    for (std::size_t i = 0; i < kPreFaderInserts; ++i) {
        if (inserts_[i].enabled && !inserts_[i].bypassed && inserts_[i].processor) {
            inserts_[i].processor->processAudioRT(outputBuffer_, nullptr, numFrames);
        }
    }

    // Inject Talkback into Studio/Cue mixes (dimming program material while talking)
    if (type_ == ControlRoomChannelType::StudioCueMix && receiveTalkback_ && talkbackSignal != nullptr) {
        outputBuffer_.applyGainRT(numFrames, talkbackDimGainLinear);
        outputBuffer_.accumulateFromRT(*talkbackSignal, numFrames, 1.0f);
    }

    // Channel Level + Dim + Pan
    float gain = dbToGain(levelDb_);
    if (type_ == ControlRoomChannelType::MonitorSpeakerSet) {
        gain *= globalDimGainLinear * talkbackDimGainLinear;
    }
    outputBuffer_.applyGainRT(numFrames, gain);
    panner_.processRT(outputBuffer_, numFrames);

    // 2 Post-Fader Inserts (indices 6..7, e.g. speaker/room calibration or headphone crossfeed)
    for (std::size_t i = kPreFaderInserts; i < kTotalInserts; ++i) {
        if (inserts_[i].enabled && !inserts_[i].bypassed && inserts_[i].processor) {
            inserts_[i].processor->processAudioRT(outputBuffer_, nullptr, numFrames);
        }
    }
}

// ============================================================================
// ControlRoom
// ============================================================================
ControlRoom::ControlRoom(VSTConnections& connections, double sampleRate, std::uint32_t maxBlockFrames)
    : connections_(connections),
      sampleRate_(sampleRate),
      maxBlockFrames_(maxBlockFrames),
      listenScratchBus_(SpeakerConfig::Surround51, maxBlockFrames),
      monitorSourceScratch_(SpeakerConfig::Surround51, maxBlockFrames) {
    for (auto& cue : aggregatedCueBuses_) {
        cue.prepare(SpeakerConfig::Stereo, maxBlockFrames);
    }
    connections_.setExclusiveMonitorPortPolicy(exclusiveMonitorPorts_);
}

void ControlRoom::setExclusiveDevicePortsForMonitorChannels(bool exclusive) {
    exclusiveMonitorPorts_ = exclusive;
    connections_.setExclusiveMonitorPortPolicy(exclusiveMonitorPorts_);
    syncPortReservations();
}

void ControlRoom::syncPortReservations() {
    connections_.releaseMonitorDevicePorts();
    if (!exclusiveMonitorPorts_) {
        return;
    }
    auto reserveVec = [this](const std::vector<std::unique_ptr<ControlRoomChannel>>& list) {
        for (const auto& ch : list) {
            for (int port : ch->hardwareOutputPorts()) {
                connections_.reserveMonitorDevicePort(port, true);
            }
        }
    };
    reserveVec(monitors_);
    reserveVec(phones_);
    reserveVec(studioCues_);
}

ControlRoomChannel* ControlRoom::addMonitorSpeakerSet(const std::string& name, SpeakerConfig cfg,
                                                      std::vector<int> hwOutPorts) {
    auto ch = std::make_unique<ControlRoomChannel>(
        name, ControlRoomChannelType::MonitorSpeakerSet, cfg, std::move(hwOutPorts));
    ch->prepare(sampleRate_, maxBlockFrames_);
    monitors_.push_back(std::move(ch));
    syncPortReservations();
    return monitors_.back().get();
}

ControlRoomChannel* ControlRoom::addPhonesChannel(const std::string& name, std::vector<int> hwOutPorts) {
    auto ch = std::make_unique<ControlRoomChannel>(
        name, ControlRoomChannelType::PhonesChannel, SpeakerConfig::Stereo, std::move(hwOutPorts));
    ch->prepare(sampleRate_, maxBlockFrames_);
    phones_.push_back(std::move(ch));
    syncPortReservations();
    return phones_.back().get();
}

ControlRoomChannel* ControlRoom::addStudioCueMix(const std::string& name, std::vector<int> hwOutPorts) {
    if (studioCues_.size() >= kMaxStudioCueMixes) {
        return nullptr;
    }
    const auto cueIdx = studioCues_.size();
    auto ch = std::make_unique<ControlRoomChannel>(
        name, ControlRoomChannelType::StudioCueMix, SpeakerConfig::Stereo, std::move(hwOutPorts));
    ch->setSource(static_cast<MonitorSourceSelection>(
        static_cast<std::uint8_t>(MonitorSourceSelection::StudioCueMix1) + cueIdx));
    ch->prepare(sampleRate_, maxBlockFrames_);
    studioCues_.push_back(std::move(ch));
    syncPortReservations();
    return studioCues_.back().get();
}

void ControlRoom::selectActiveMonitorIndex(std::size_t monitorIdx) noexcept {
    if (!monitors_.empty()) {
        activeMonitorIdx_ = std::min(monitorIdx, monitors_.size() - 1);
    }
}

void ControlRoom::setDimActive(bool dim, float dimAttenuationDb) noexcept {
    dimActive_ = dim;
    dimDb_ = dimAttenuationDb;
}

void ControlRoom::setTalkbackActive(bool active, float talkbackDimDb) noexcept {
    talkbackActive_ = active;
    talkbackDimDb_ = talkbackDimDb;
}

void ControlRoom::setListenMode(ListenMode mode, float listenDimDb) noexcept {
    listenMode_ = mode;
    listenDimDb_ = listenDimDb;
}

void ControlRoom::processControlRoomRT(
    const AudioBlock32& mainMixBus,
    std::span<const std::unique_ptr<ChannelStrip>> mixerStrips,
    const AudioBlock32* talkbackMicIn,
    const AudioBlock32* externalTwoTrackIn,
    std::uint32_t numFrames) noexcept {
    if (!enabled_ || numFrames == 0) {
        return;
    }

    // 1. Aggregate Studio Cue Sends (1..4) and PFL/AFL Listen Bus across Mixer Strips
    for (auto& cueBus : aggregatedCueBuses_) {
        cueBus.clearRT(numFrames);
    }
    listenScratchBus_.clearRT(numFrames);
    bool anyListenActive = false;

    for (const auto& strip : mixerStrips) {
        for (std::size_t c = 0; c < kMaxStudioCueMixes; ++c) {
            if (strip->studioSend(c).enabled) {
                aggregatedCueBuses_[c].accumulateFromRT(strip->studioCueTapBuffer(c), numFrames, 1.0f);
            }
        }
        if (strip->isListenEnabled()) {
            anyListenActive = true;
            const AudioBlock32& tap = (listenMode_ == ListenMode::PFL_PreFaderListen)
                ? strip->pflTapBuffer()
                : strip->aflTapBuffer();
            listenScratchBus_.accumulateFromRT(tap, numFrames, 1.0f);
        }
    }

    auto resolveSource = [&](MonitorSourceSelection sel) -> const AudioBlock32& {
        switch (sel) {
            case MonitorSourceSelection::StudioCueMix1: return aggregatedCueBuses_[0];
            case MonitorSourceSelection::StudioCueMix2: return aggregatedCueBuses_[1];
            case MonitorSourceSelection::StudioCueMix3: return aggregatedCueBuses_[2];
            case MonitorSourceSelection::StudioCueMix4: return aggregatedCueBuses_[3];
            case MonitorSourceSelection::ExternalInput1:
                return (externalTwoTrackIn != nullptr) ? *externalTwoTrackIn : mainMixBus;
            case MonitorSourceSelection::MainMixBus:
            default:
                return mainMixBus;
        }
    };

    const float tbDimLin = talkbackActive_ ? dbToGain(talkbackDimDb_) : 1.0f;
    const float globalDimLin = dimActive_ ? dbToGain(dimDb_) : 1.0f;
    const AudioBlock32* tbPtr = talkbackActive_ ? talkbackMicIn : nullptr;

    // 2. Process Studio Cue Mixes (1..4)
    for (std::size_t i = 0; i < studioCues_.size(); ++i) {
        const AudioBlock32& cueSrc = resolveSource(studioCues_[i]->source());
        studioCues_[i]->processChannelRT(cueSrc, tbPtr, tbDimLin, 1.0f,
                                         DownmixPreset::Direct1To1, numFrames);
    }

    // 3. Process Active Monitor Speaker Set (injecting PFL/AFL Listen Bus without touching MainMix!)
    for (std::size_t m = 0; m < monitors_.size(); ++m) {
        if (m != activeMonitorIdx_) {
            monitorSourceScratch_.clearRT(numFrames);
            monitors_[m]->processChannelRT(monitorSourceScratch_, nullptr, 1.0f, 0.0f,
                                           activeDownmix_, numFrames);
            continue;
        }
        monitorSourceScratch_.copyFromRT(resolveSource(monitors_[m]->source()), numFrames);
        if (anyListenActive) {
            monitorSourceScratch_.applyGainRT(numFrames, dbToGain(listenDimDb_));
            monitorSourceScratch_.accumulateFromRT(listenScratchBus_, numFrames, 1.0f);
        }
        monitors_[m]->processChannelRT(monitorSourceScratch_, nullptr, tbDimLin,
                                       globalDimLin, activeDownmix_, numFrames);
    }

    // 4. Process Phones Channels
    for (auto& ph : phones_) {
        monitorSourceScratch_.copyFromRT(resolveSource(ph->source()), numFrames);
        if (anyListenActive) {
            monitorSourceScratch_.applyGainRT(numFrames, dbToGain(listenDimDb_));
            monitorSourceScratch_.accumulateFromRT(listenScratchBus_, numFrames, 1.0f);
        }
        ph->processChannelRT(monitorSourceScratch_, nullptr, 1.0f, 1.0f,
                             DownmixPreset::Surround51ToStereo, numFrames);
    }
}

} // namespace AlphaStudio::Core
