#pragma once

#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// Control Room (Monitoring & Cue Mix Engine)
// - Strictly decoupled from the Main Mix export/output bus
// - Channels: Monitor (A..D), Phones (1..2), and Studio/Cue Mixes (1..4)
// - Each Control Room channel features 6 Pre-Fader Inserts + 2 Post-Fader Inserts
// - Downmix Presets, Talkback (with auto Talkback Dim), Dim, PFL/AFL Listen Bus,
//   and ExclusiveDevicePortsForMonitorChannels policy
// ============================================================================
enum class ControlRoomChannelType : std::uint8_t {
    MonitorSpeakerSet = 0,
    PhonesChannel     = 1,
    StudioCueMix      = 2
};

enum class MonitorSourceSelection : std::uint8_t {
    MainMixBus     = 0,
    StudioCueMix1  = 1,
    StudioCueMix2  = 2,
    StudioCueMix3  = 3,
    StudioCueMix4  = 4,
    ExternalInput1 = 5
};

enum class DownmixPreset : std::uint8_t {
    Direct1To1         = 0,
    Surround51ToStereo = 1, // ITU-R BS.775: L' = L + 0.707*C + 0.707*Ls
    StereoToMono       = 2, // (L + R) * 0.707
    LeftMinusRightDiff = 3  // (L - R) * 0.707 phase/side check
};

enum class ListenMode : std::uint8_t {
    PFL_PreFaderListen  = 0,
    AFL_AfterFaderListen = 1
};

class ControlRoomChannel {
public:
    static constexpr std::size_t kPreFaderInserts  = 6;
    static constexpr std::size_t kPostFaderInserts = 2;
    static constexpr std::size_t kTotalInserts     = kPreFaderInserts + kPostFaderInserts; // 8

    ControlRoomChannel(std::string name, ControlRoomChannelType type,
                       SpeakerConfig speakerCfg, std::vector<int> hwOutputPorts);

    void prepare(double sampleRate, std::uint32_t maxBlockFrames);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] ControlRoomChannelType type() const noexcept { return type_; }
    [[nodiscard]] SpeakerConfig speakerConfig() const noexcept { return speakerConfig_; }
    [[nodiscard]] const std::vector<int>& hardwareOutputPorts() const noexcept { return hwOutputPorts_; }

    void setSource(MonitorSourceSelection src) noexcept { source_ = src; }
    [[nodiscard]] MonitorSourceSelection source() const noexcept { return source_; }
    void setLevelDb(float db) noexcept { levelDb_ = db; }
    [[nodiscard]] float levelDb() const noexcept { return levelDb_; }
    void setPan(float pan) noexcept { panner_.setPan(pan); }
    [[nodiscard]] float pan() const noexcept { return panner_.pan(); }
    void setTalkbackEnabledOnCue(bool enabled) noexcept { receiveTalkback_ = enabled; }
    [[nodiscard]] bool receiveTalkback() const noexcept { return receiveTalkback_; }

    void setInsert(std::size_t slot0To7, std::shared_ptr<IAudioInsertProcessor> proc, bool enabled = true);
    [[nodiscard]] static constexpr bool isInsertPreFader(std::size_t slot0To7) noexcept {
        return slot0To7 < kPreFaderInserts;
    }

    void processChannelRT(const AudioBlock32& sourceSignal,
                          const AudioBlock32* talkbackSignal,
                          float talkbackDimGainLinear,
                          float globalDimGainLinear,
                          DownmixPreset downmix,
                          std::uint32_t numFrames) noexcept;

    [[nodiscard]] const AudioBlock32& outputBuffer() const noexcept { return outputBuffer_; }

private:
    static void applyDownmixPresetRT(AudioBlock32& buf, DownmixPreset preset, std::uint32_t numFrames) noexcept;

    std::string name_;
    ControlRoomChannelType type_{ControlRoomChannelType::MonitorSpeakerSet};
    SpeakerConfig speakerConfig_{SpeakerConfig::Stereo};
    std::vector<int> hwOutputPorts_;
    double sampleRate_{48000.0};
    std::uint32_t maxFrames_{4096};

    MonitorSourceSelection source_{MonitorSourceSelection::MainMixBus};
    float levelDb_{0.0f};
    bool receiveTalkback_{true};
    ChannelPanner panner_;
    std::array<AudioInsertSlot, kTotalInserts> inserts_{};
    AudioBlock32 outputBuffer_;
};

class ControlRoom {
public:
    static constexpr std::size_t kMaxStudioCueMixes = 4;

    ControlRoom(VSTConnections& connections, double sampleRate = 48000.0,
                std::uint32_t maxBlockFrames = 4096);

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    // ExclusiveDevicePortsForMonitorChannels: prevents Main Mix / Output buses from
    // sharing physical ports assigned to Control Room Monitor / Phones / Cue channels
    void setExclusiveDevicePortsForMonitorChannels(bool exclusive);
    [[nodiscard]] bool exclusiveDevicePortsForMonitorChannels() const noexcept {
        return exclusiveMonitorPorts_;
    }

    ControlRoomChannel* addMonitorSpeakerSet(const std::string& name, SpeakerConfig cfg,
                                             std::vector<int> hwOutPorts);
    ControlRoomChannel* addPhonesChannel(const std::string& name, std::vector<int> hwOutPorts);
    ControlRoomChannel* addStudioCueMix(const std::string& name, std::vector<int> hwOutPorts);

    void selectActiveMonitorIndex(std::size_t monitorIdx) noexcept;
    [[nodiscard]] std::size_t activeMonitorIndex() const noexcept { return activeMonitorIdx_; }

    void setDownmixPreset(DownmixPreset preset) noexcept { activeDownmix_ = preset; }
    [[nodiscard]] DownmixPreset downmixPreset() const noexcept { return activeDownmix_; }

    void setDimActive(bool dim, float dimAttenuationDb = -20.0f) noexcept;
    [[nodiscard]] bool isDimActive() const noexcept { return dimActive_; }
    [[nodiscard]] float dimAttenuationDb() const noexcept { return dimDb_; }

    void setTalkbackActive(bool active, float talkbackDimDb = -20.0f) noexcept;
    [[nodiscard]] bool isTalkbackActive() const noexcept { return talkbackActive_; }

    void setListenMode(ListenMode mode, float listenDimDb = -18.0f) noexcept;
    [[nodiscard]] ListenMode listenMode() const noexcept { return listenMode_; }

    [[nodiscard]] const std::vector<std::unique_ptr<ControlRoomChannel>>& monitorSets() const noexcept {
        return monitors_;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<ControlRoomChannel>>& phonesChannels() const noexcept {
        return phones_;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<ControlRoomChannel>>& studioCueChannels() const noexcept {
        return studioCues_;
    }

    // Processes monitoring & cue buses strictly without altering mainMixBus!
    void processControlRoomRT(const AudioBlock32& mainMixBus,
                              std::span<const std::unique_ptr<ChannelStrip>> mixerStrips,
                              const AudioBlock32* talkbackMicIn,
                              const AudioBlock32* externalTwoTrackIn,
                              std::uint32_t numFrames) noexcept;

private:
    void syncPortReservations();

    VSTConnections& connections_;
    double sampleRate_{48000.0};
    std::uint32_t maxBlockFrames_{4096};
    bool enabled_{true};
    bool exclusiveMonitorPorts_{true};

    std::size_t activeMonitorIdx_{0};
    DownmixPreset activeDownmix_{DownmixPreset::Direct1To1};
    bool dimActive_{false};
    float dimDb_{-20.0f};
    bool talkbackActive_{false};
    float talkbackDimDb_{-20.0f};
    ListenMode listenMode_{ListenMode::AFL_AfterFaderListen};
    float listenDimDb_{-18.0f};

    std::vector<std::unique_ptr<ControlRoomChannel>> monitors_;
    std::vector<std::unique_ptr<ControlRoomChannel>> phones_;
    std::vector<std::unique_ptr<ControlRoomChannel>> studioCues_;

    std::array<AudioBlock32, kMaxStudioCueMixes> aggregatedCueBuses_{};
    AudioBlock32 listenScratchBus_;
    AudioBlock32 monitorSourceScratch_;
};

} // namespace AlphaStudio::Core
