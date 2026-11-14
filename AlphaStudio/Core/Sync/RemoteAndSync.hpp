#pragma once

#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::Sync {

// ============================================================================
// TransportStateController: Shared Transport Target for Remote, MMC, System Link & UI
// ============================================================================
struct StudioTransportState {
    bool isPlaying{false};
    bool isRecording{false};
    bool cycleActive{false};
    bool preCountEnabled{false};
    std::uint32_t preCountBars{2};
    bool fastForwarding{false};
    bool rewinding{false};
    float jogShuttleSpeed{0.0f}; // -8.0x .. +8.0x high-precision scrub/shuttle
    std::int64_t currentSamplePosition{0};
    std::int64_t leftLocatorTick{0};
    std::int64_t rightLocatorTick{7680}; // 4 bars @ 480 PPQ
    std::size_t selectedTrackIndex{0};
};

// ============================================================================
// 1. Generic Remote (XML Two-Grid Engine)
// - Upper Grid: MIDI Input Binding (Controller, Channel, Address CC, MaxValue, Flags)
// - Lower Grid: Internal Command / Mixer / Track Target
// ============================================================================
enum GenericRemoteMidiFlags : std::uint8_t {
    kGRFlagReceive  = 1U << 0,
    kGRFlagTransmit = 1U << 1,
    kGRFlagRelative = 1U << 2
};

enum class GenericRemoteTargetAction : std::uint8_t {
    TransportStart      = 0,
    TransportStop       = 1,
    TransportRecord     = 2,
    SelectTrack         = 3,
    ToggleTrackMonitor  = 4,
    ToggleTrackRecArm   = 5,
    ConsoleChannelFader = 6,
    ConsoleChannelPan   = 7
};

struct GenericRemoteUpperRow {
    std::string controlName{"Fader 1"};
    std::uint8_t midiStatus{0xB0}; // 0xB0 Control Change
    std::uint8_t midiChannel{0};   // 0..15
    std::uint8_t ccAddress{7};     // 0..127
    std::uint16_t maxValue{127};
    std::uint8_t flags{kGRFlagReceive | kGRFlagTransmit};
};

struct GenericRemoteLowerRow {
    std::string controlName{"Fader 1"};
    std::string deviceCategory{"VST Mixer"};
    std::uint32_t channelOrTrackIndex{0};
    GenericRemoteTargetAction action{GenericRemoteTargetAction::ConsoleChannelFader};
    bool toggleFlag{false};
};

class GenericRemoteEngine {
public:
    void addMappingPair(GenericRemoteUpperRow upper, GenericRemoteLowerRow lower);
    void clearMappings();

    [[nodiscard]] std::string serializeToXml() const;
    bool loadFromXml(const std::string& xmlContent);

    // Evaluates incoming MIDI controller message against Upper Grid -> executes Lower Grid
    bool processIncomingMidiMessage(std::uint8_t status, std::uint8_t data1, std::uint8_t data2,
                                    StudioTransportState& transport,
                                    std::vector<std::unique_ptr<TrackBase>>& tracks,
                                    VSTMixer& mixer);

    [[nodiscard]] const std::vector<GenericRemoteUpperRow>& upperGrid() const noexcept { return upperGrid_; }
    [[nodiscard]] const std::vector<GenericRemoteLowerRow>& lowerGrid() const noexcept { return lowerGrid_; }

private:
    std::vector<GenericRemoteUpperRow> upperGrid_;
    std::vector<GenericRemoteLowerRow> lowerGrid_;
};

// ============================================================================
// 2. MMC (MIDI Machine Control) & MIDI Clock (ProjectSynchronizationSetup)
// ============================================================================
enum class SyncTimecodeSource : std::uint8_t {
    InternalClock   = 0,
    MIDITimecodeMTC = 1,
    ASIOAudioClock  = 2,
    VSTSystemLink   = 3
};

struct ProjectSynchronizationSetup {
    SyncTimecodeSource timecodeSource{SyncTimecodeSource::InternalClock};
    bool mmcSlaveActive{false};  // MMC Slave Active toggle!
    bool mmcMasterActive{false};
    std::uint8_t mmcDeviceId{0x7F}; // 0x7F = All-Call broadcast
    bool sendMidiClock{false};
    bool sendMidiTimecode{false};
    double frameRateFps{25.0};
};

class MMCAndMidiClockEngine {
public:
    void setSetup(const ProjectSynchronizationSetup& setup) noexcept { setup_ = setup; }
    [[nodiscard]] ProjectSynchronizationSetup& setup() noexcept { return setup_; }
    [[nodiscard]] const ProjectSynchronizationSetup& setup() const noexcept { return setup_; }

    // MMC Universal Real-Time SysEx Builders: F0 7F <devId> 06 <cmd> F7
    [[nodiscard]] std::vector<std::uint8_t> buildMMCStop() const;
    [[nodiscard]] std::vector<std::uint8_t> buildMMCDeferredPlay() const;
    [[nodiscard]] std::vector<std::uint8_t> buildMMCRecordStrobe() const;
    [[nodiscard]] std::vector<std::uint8_t> buildMMCLocateSamples(std::int64_t samplePos, double sampleRate) const;

    // Parses incoming MMC SysEx when mmcSlaveActive == true
    bool handleIncomingMMCSysEx(std::span<const std::uint8_t> sysexBytes,
                                double sampleRate,
                                StudioTransportState& transport) const noexcept;

    // Generates 24-PPQ MIDI Timing Clock (0xF8) / Start (0xFA) / Stop (0xFC) bytes
    [[nodiscard]] std::vector<std::uint8_t> generateMidiClockBytesForTickSpan(
        std::int64_t startTick480, std::int64_t endTick480, bool transportJustStarted) const;

private:
    ProjectSynchronizationSetup setup_{};
};

// ============================================================================
// 3. VST System Link: Sample-Accurate Clock & Transport Synchronization
//    Encoded in Bit 0 (LSB) of a 24/32-bit Digital Audio Stream
//    Supports Play, Stop, Record, Locate, Fast Forward, Rewind, and Jog/Shuttle!
// ============================================================================
enum class SystemLinkTransportCmd : std::uint8_t {
    NoChange    = 0,
    Play        = 1,
    Stop        = 2,
    Record      = 3,
    Locate      = 4,
    FastForward = 5,
    Rewind      = 6,
    JogShuttle  = 7
};

struct SystemLinkPacket {
    SystemLinkTransportCmd command{SystemLinkTransportCmd::NoChange};
    std::uint64_t samplePosition{0};
    float jogShuttleVelocity{0.0f}; // Signed shuttle speed (-8.0 .. +8.0)
    std::uint8_t midiTunnelByte{0};
};

class VSTSystemLinkProcessor {
public:
    static constexpr std::size_t kFrameBits = 64;
    static constexpr std::uint16_t kSyncHeader12 = 0xA5BU;

    void setOnline(bool online) noexcept { online_ = online; }
    [[nodiscard]] bool isOnline() const noexcept { return online_; }

    // Encodes a 64-bit System Link frame into bit 0 (LSB) of 64 consecutive 24-bit audio samples
    // mapped inside 32-bit float digital audio (S/PDIF / ADAT / AES3 lossless 24-bit integer mantissa)
    void encodePacketIntoAudioLSB(std::span<float> digitalAudioChannel,
                                  const SystemLinkPacket& packet) noexcept;

    // Decodes bit 0 (LSB) stream back into SystemLinkPacket and updates StudioTransportState
    bool decodePacketFromAudioLSB(std::span<const float> digitalAudioChannel,
                                  SystemLinkPacket& outPacket,
                                  StudioTransportState* applyToTransport = nullptr) noexcept;

private:
    [[nodiscard]] static std::uint8_t computeCrc4(std::uint64_t payload60Bits) noexcept;
    bool online_{false};
};

// ============================================================================
// 4. ReWire: Host/Slave Audio & MIDI Inter-Application Link Abstraction
// ============================================================================
struct ReWireDeviceChannelBus {
    std::uint32_t busIndex{0};
    std::string label;
    bool active{false};
    AudioBlock32 audioBuffer;
};

class ReWireSubsystem {
public:
    static constexpr std::size_t kMaxReWireAudioBuses = 64;

    explicit ReWireSubsystem(double sampleRate = 48000.0, std::uint32_t maxBlockFrames = 4096);

    bool openDeviceSession(const std::string& slaveAppName, bool actAsHost = true);
    void closeDeviceSession() noexcept;
    [[nodiscard]] bool isSessionActive() const noexcept { return sessionActive_; }
    [[nodiscard]] bool isHostMode() const noexcept { return isHostMode_; }

    bool setBusActive(std::size_t busIdx, bool active);
    void renderLinkedDeviceRT(const StudioTransportState& transport,
                              double tempoBpm,
                              AudioBlock32& mixAccumulator,
                              std::uint32_t numFrames) noexcept;

private:
    double sampleRate_{48000.0};
    std::uint32_t maxFrames_{4096};
    bool sessionActive_{false};
    bool isHostMode_{true};
    std::string connectedPeerName_;
    std::vector<ReWireDeviceChannelBus> buses_;
    double testTonePhase_{0.0};
};

} // namespace AlphaStudio::Core::Sync
