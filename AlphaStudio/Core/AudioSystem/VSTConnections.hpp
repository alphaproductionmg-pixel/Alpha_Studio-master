#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// True multichannel speaker arrangements up to Surround 5.1 (Zero Video)
enum class SpeakerConfig : std::uint8_t {
    Mono       = 1, // C
    Stereo     = 2, // L, R
    LCR        = 3, // L, C, R
    Quad       = 4, // L, R, Ls, Rs
    LCRS       = 5, // L, C, R, S
    Surround50 = 6, // L, R, C, Ls, Rs
    Surround51 = 7  // L, R, C, LFE, Ls, Rs
};

[[nodiscard]] std::uint32_t channelCountForSpeakerConfig(SpeakerConfig cfg) noexcept;
[[nodiscard]] const char* speakerConfigName(SpeakerConfig cfg) noexcept;

enum class ConnectionBusCategory : std::uint8_t {
    PhysicalInput      = 0,
    PhysicalOutput     = 1,
    GroupBus           = 2,
    FXReturnBus        = 3,
    ExternalFX         = 4,
    ExternalInstrument = 5,
    StudioMonitorBus   = 6
};

struct ChildBusDescriptor {
    std::uint32_t subBusId{0};
    std::string name;
    SpeakerConfig config{SpeakerConfig::Stereo};
    std::vector<std::uint32_t> parentChannelOffsets;
};

struct AudioConnectionBus {
    std::uint32_t busId{0};
    std::string name;
    ConnectionBusCategory category{ConnectionBusCategory::PhysicalOutput};
    SpeakerConfig speakerConfig{SpeakerConfig::Stereo};
    std::vector<int> devicePortAssignments; // Physical ASIO port index per channel (-1 = unconnected)
    std::vector<ChildBusDescriptor> childSubBuses;
    bool isDefaultMainMix{false};
};

// External Hardware Effect (Send + Return + Hardware Latency Ping + MIDI Link)
struct ExternalFXDefinition {
    std::uint32_t id{0};
    std::string name;
    SpeakerConfig sendConfig{SpeakerConfig::Stereo};
    SpeakerConfig returnConfig{SpeakerConfig::Stereo};
    std::vector<int> sendDevicePorts;
    std::vector<int> returnDevicePorts;
    double hardwareDelayMs{0.0};
    float sendGainDb{0.0f};
    float returnGainDb{0.0f};
    std::string midiDeviceName;
    bool isFavorite{false};
    bool usedInProject{false};

    [[nodiscard]] std::uint32_t latencySamples(double sampleRate) const noexcept;
};

// External Hardware Synthesizer / Instrument (MIDI Port + Audio Return Bus)
struct ExternalInstrumentDefinition {
    std::uint32_t id{0};
    std::string name;
    std::string midiDeviceName;
    std::uint8_t midiChannelMask{0xFFU};
    std::uint32_t numMonoReturns{0};
    std::uint32_t numStereoReturns{1};
    std::vector<int> returnDevicePorts;
    double hardwareDelayMs{0.0};
    float returnGainDb{0.0f};
    bool usedInProject{false};

    [[nodiscard]] std::uint32_t latencySamples(double sampleRate) const noexcept;
};

class VSTConnections {
public:
    static constexpr std::uint32_t kMaxPhysicalDevicePorts = 256;

    VSTConnections();

    // Exclusive port reservation for Control Room monitor channels
    void setExclusiveMonitorPortPolicy(bool exclusive);
    [[nodiscard]] bool exclusiveMonitorPortPolicy() const noexcept { return exclusiveMonitorPorts_; }
    void reserveMonitorDevicePort(int portIndex, bool isOutputPort);
    void releaseMonitorDevicePorts();
    [[nodiscard]] bool isPortReservedByControlRoom(int portIndex, bool isOutputPort) const;

    // Bus creation and management
    std::uint32_t addInputBus(const std::string& name, SpeakerConfig cfg,
                              const std::vector<int>& devicePorts);
    std::uint32_t addOutputBus(const std::string& name, SpeakerConfig cfg,
                               const std::vector<int>& devicePorts, bool setAsMainMix = false);
    std::uint32_t addGroupBus(const std::string& name, SpeakerConfig cfg);
    std::uint32_t addFXReturnBus(const std::string& name, SpeakerConfig cfg);
    bool addChildSubBus(std::uint32_t parentBusId, const std::string& subName,
                        SpeakerConfig subCfg, const std::vector<std::uint32_t>& offsets);
    bool removeBus(std::uint32_t busId);

    // External FX & External Instruments
    std::uint32_t addExternalFX(ExternalFXDefinition fx);
    double pingExternalFXLatencyMs(std::uint32_t fxId, double sampleRate, std::uint32_t asioRoundTripSamples);
    std::uint32_t addExternalInstrument(ExternalInstrumentDefinition inst);

    [[nodiscard]] const std::vector<AudioConnectionBus>& inputBuses() const noexcept { return inputBuses_; }
    [[nodiscard]] const std::vector<AudioConnectionBus>& outputBuses() const noexcept { return outputBuses_; }
    [[nodiscard]] const std::vector<AudioConnectionBus>& groupBuses() const noexcept { return groupBuses_; }
    [[nodiscard]] const std::vector<AudioConnectionBus>& fxReturnBuses() const noexcept { return fxReturnBuses_; }
    [[nodiscard]] const std::vector<ExternalFXDefinition>& externalFXList() const noexcept { return externalFX_; }
    [[nodiscard]] const std::vector<ExternalInstrumentDefinition>& externalInstruments() const noexcept { return externalInstruments_; }

    [[nodiscard]] const AudioConnectionBus* findBus(std::uint32_t busId) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> mainMixOutputBusId() const noexcept;

    void createDefaultStereoIO();

private:
    std::vector<int> sanitizePortList(const std::vector<int>& ports, std::uint32_t expectedChannels,
                                      bool isOutputPort) const;

    std::uint32_t nextBusId_{1};
    bool exclusiveMonitorPorts_{true};
    std::set<int> reservedMonitorInputPorts_;
    std::set<int> reservedMonitorOutputPorts_;

    std::vector<AudioConnectionBus> inputBuses_;
    std::vector<AudioConnectionBus> outputBuses_;
    std::vector<AudioConnectionBus> groupBuses_;
    std::vector<AudioConnectionBus> fxReturnBuses_;
    std::vector<ExternalFXDefinition> externalFX_;
    std::vector<ExternalInstrumentDefinition> externalInstruments_;
};

} // namespace AlphaStudio::Core
