#pragma once

#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"
#include "AlphaStudio/Core/Threading/LockFreeRingBuffer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// All Track Types (Strictly Zero Video Track!)
// ============================================================================
enum class TrackType : std::uint8_t {
    AudioTrack        = 0,
    MIDITrack         = 1,
    InstrumentTrack   = 2,
    GroupChannelTrack = 3,
    FXChannelTrack    = 4,
    FolderTrack       = 5,
    MarkerTrack       = 6,
    RulerTrack        = 7,
    TempoTrack        = 8,
    SignatureTrack    = 9,
    ArrangerTrack     = 10,
    TransposeTrack    = 11
};

[[nodiscard]] const char* trackTypeName(TrackType type) noexcept;

// 5 Recording Modes (Audio & MIDI, including Stacked Takes/Lanes)
enum class RecordMode : std::uint8_t {
    Mix       = 0,
    Overwrite = 1,
    Normal    = 2,
    Merge     = 3,
    Stacked   = 4 // Creates sequential Take Lanes on cycle loops
};

// Audio Clip Event with Per-Event Volume Envelope & Lane Index
struct EnvelopePoint {
    double normalizedPosition01{0.0}; // 0.0 (clip start) .. 1.0 (clip end)
    float gainLinear{1.0f};           // 0.0 .. 2.0 (+6 dB)
};

struct AudioClipEvent {
    std::uint32_t eventId{0};
    std::string name;
    std::uint32_t poolAssetId{0};
    std::int64_t startSample{0};
    std::int64_t startTick{0};
    std::size_t assetOffsetFrames{0};
    std::size_t lengthFrames{0};
    std::uint32_t laneIndex{0}; // Used by Stacked recording & comping
    float clipGainDb{0.0f};
    std::size_t fadeInFrames{0};
    std::size_t fadeOutFrames{0};
    bool musicalMode{false};
    bool independentFromTransposeTrack{false};
    std::int32_t semitoneTranspose{0};
    bool muted{false};
    std::vector<EnvelopePoint> volumeEnvelope; // Per-event VolumeEnvelope!

    [[nodiscard]] float evaluateVolumeEnvelope(double normPos01) const noexcept;
};

struct MidiPartContainer {
    std::uint32_t partId{0};
    std::string name{"MIDI Part"};
    std::int64_t startTick{0};
    std::int64_t lengthTicks{1920}; // 1 bar @ 4/4 480 PPQ
    std::uint32_t laneIndex{0};
    bool independentFromTransposeTrack{false};
    bool muted{false};
    std::vector<MidiSequenceEvent> events;
    std::vector<MidiXFChordEvent> xfMetadata; // MIDI XF chords/lyrics
};

// Standard Position Markers & Independent Cycle Markers
struct MarkerEvent {
    std::uint32_t markerId{1};
    std::string description;
    bool isCycleMarker{false}; // False = Position Marker, True = Cycle Marker [startTick..endTick]
    std::int64_t startTick{0};
    std::int64_t endTick{0};
};

enum class RulerDisplayMode : std::uint8_t {
    BarsAndBeats = 0,
    Seconds      = 1,
    AudioFrames  = 2,
    Samples      = 3
};

// ArrangerTrack Regions & Dynamic Playback Chain
struct ArrangerRegion {
    std::uint32_t regionId{0};
    std::string label{"Section A"};
    std::int64_t startTick{0};
    std::int64_t endTick{1920};
};

struct ArrangerChainStep {
    std::uint32_t regionId{0};
    std::uint32_t repeatCount{1};
};

struct GlobalTransposeEvent {
    std::int64_t startTick{0};
    std::int32_t semitones{0}; // -24 .. +24
};

// ============================================================================
// Track Hierarchy
// ============================================================================
class TrackBase {
public:
    TrackBase(std::uint32_t id, std::string name, TrackType type);
    virtual ~TrackBase() = default;

    [[nodiscard]] std::uint32_t trackId() const noexcept { return trackId_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string n) { name_ = std::move(n); }
    [[nodiscard]] TrackType type() const noexcept { return type_; }

    void setMuted(bool m) noexcept { muted_ = m; }
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setSoloed(bool s) noexcept { soloed_ = s; }
    [[nodiscard]] bool isSoloed() const noexcept { return soloed_; }
    void setRecordArmed(bool arm) noexcept { recordArmed_ = arm; }
    [[nodiscard]] bool isRecordArmed() const noexcept { return recordArmed_; }
    void setMonitorEnabled(bool mon) noexcept { monitorEnabled_ = mon; }
    [[nodiscard]] bool isMonitorEnabled() const noexcept { return monitorEnabled_; }
    void setColorRgba(std::uint32_t rgba) noexcept { colorRgba_ = rgba; }
    [[nodiscard]] std::uint32_t colorRgba() const noexcept { return colorRgba_; }

protected:
    std::uint32_t trackId_{0};
    std::string name_;
    TrackType type_{TrackType::AudioTrack};
    bool muted_{false};
    bool soloed_{false};
    bool recordArmed_{false};
    bool monitorEnabled_{false};
    std::uint32_t colorRgba_{0x3D7EB8FFU};
};

class AudioTrack final : public TrackBase {
public:
    AudioTrack(std::uint32_t id, std::string name, ChannelStrip* strip);

    [[nodiscard]] ChannelStrip* channelStrip() noexcept { return strip_; }
    void setRecordInputFromBus(bool fromBus, std::uint32_t sourceGroupOrFXBusId) noexcept {
        recordFromBus_ = fromBus;
        sourceBusId_ = sourceGroupOrFXBusId;
    }
    [[nodiscard]] bool recordsFromBus() const noexcept { return recordFromBus_; }
    [[nodiscard]] std::uint32_t recordSourceBusId() const noexcept { return sourceBusId_; }

    void setLanesExpanded(bool expanded) noexcept { lanesExpanded_ = expanded; }
    [[nodiscard]] bool lanesExpanded() const noexcept { return lanesExpanded_; }

    AudioClipEvent& commitRecordedAudioClip(AudioClipEvent clip, RecordMode mode, std::uint32_t cycleTakeCounter);
    [[nodiscard]] std::vector<AudioClipEvent>& clips() noexcept { return clips_; }
    [[nodiscard]] const std::vector<AudioClipEvent>& clips() const noexcept { return clips_; }

private:
    ChannelStrip* strip_{nullptr};
    bool recordFromBus_{false};
    std::uint32_t sourceBusId_{0};
    bool lanesExpanded_{false};
    std::uint32_t nextClipId_{1};
    std::vector<AudioClipEvent> clips_;
};

struct MidiSendSlot {
    bool enabled{false};
    bool preInsertsAndExpression{false}; // Pre/Post button for MIDI Sends!
    std::uint8_t outputPortIndex{0};
    std::uint8_t midiChannel{0};
    std::shared_ptr<IMidiInsertProcessor> sendEffect;
};

class MIDITrack : public TrackBase {
public:
    static constexpr std::size_t kNumMidiInserts = 4;
    static constexpr std::size_t kNumMidiSends   = 4;

    MIDITrack(std::uint32_t id, std::string name, TrackType concreteType = TrackType::MIDITrack);

    void setMidiChannel(std::uint8_t ch0To15) noexcept { midiChannel_ = ch0To15 & 0x0FU; }
    [[nodiscard]] std::uint8_t midiChannel() const noexcept { return midiChannel_; }

    void setMidiInsert(std::size_t slot0To3, std::shared_ptr<IMidiInsertProcessor> proc);
    [[nodiscard]] IMidiInsertProcessor* midiInsert(std::size_t slot0To3) const noexcept;

    void configureMidiSend(std::size_t slot0To3, bool enabled, bool preInserts,
                           std::uint8_t outPort, std::uint8_t ch) noexcept;
    [[nodiscard]] const MidiSendSlot& midiSend(std::size_t slot0To3) const noexcept { return midiSends_[slot0To3]; }

    MidiPartContainer& commitRecordedMidiPart(MidiPartContainer incoming, RecordMode mode,
                                              std::uint32_t cycleTakeCounter);
    [[nodiscard]] std::vector<MidiPartContainer>& parts() noexcept { return parts_; }
    [[nodiscard]] const std::vector<MidiPartContainer>& parts() const noexcept { return parts_; }

protected:
    std::uint8_t midiChannel_{0};
    std::uint32_t nextPartId_{1};
    std::array<std::shared_ptr<IMidiInsertProcessor>, kNumMidiInserts> midiInserts_{};
    std::array<MidiSendSlot, kNumMidiSends> midiSends_{};
    std::vector<MidiPartContainer> parts_;
};

class InstrumentTrack final : public MIDITrack {
public:
    InstrumentTrack(std::uint32_t id, std::string name, ChannelStrip* audioStrip,
                    std::string instrumentPluginName);

    [[nodiscard]] ChannelStrip* channelStrip() noexcept { return audioStrip_; }
    [[nodiscard]] const std::string& instrumentPluginName() const noexcept { return instrumentName_; }
    void setInstrumentPluginName(std::string n) { instrumentName_ = std::move(n); }

private:
    ChannelStrip* audioStrip_{nullptr};
    std::string instrumentName_;
};

class GroupChannelTrack final : public TrackBase {
public:
    GroupChannelTrack(std::uint32_t id, std::string name, ChannelStrip* groupStrip)
        : TrackBase(id, std::move(name), TrackType::GroupChannelTrack), strip_(groupStrip) {}
    [[nodiscard]] ChannelStrip* channelStrip() noexcept { return strip_; }
private:
    ChannelStrip* strip_{nullptr};
};

class FXChannelTrack final : public TrackBase {
public:
    FXChannelTrack(std::uint32_t id, std::string name, ChannelStrip* fxStrip)
        : TrackBase(id, std::move(name), TrackType::FXChannelTrack), strip_(fxStrip) {}
    [[nodiscard]] ChannelStrip* channelStrip() noexcept { return strip_; }
private:
    ChannelStrip* strip_{nullptr};
};

class FolderTrack final : public TrackBase {
public:
    FolderTrack(std::uint32_t id, std::string name)
        : TrackBase(id, std::move(name), TrackType::FolderTrack) {}

    void addChildTrack(TrackBase* child);
    void setGroupMute(bool mute);
    void setGroupSolo(bool solo);
    void setGroupRecordArm(bool arm);
    [[nodiscard]] const std::vector<TrackBase*>& children() const noexcept { return children_; }

private:
    std::vector<TrackBase*> children_;
};

class MarkerTrack final : public TrackBase {
public:
    MarkerTrack(std::uint32_t id, std::string name = "Markers")
        : TrackBase(id, std::move(name), TrackType::MarkerTrack) {}

    std::uint32_t addPositionMarker(std::int64_t tick, const std::string& desc);
    std::uint32_t addCycleMarker(std::int64_t startTick, std::int64_t endTick, const std::string& desc);
    [[nodiscard]] const std::vector<MarkerEvent>& markers() const noexcept { return markers_; }

private:
    std::uint32_t nextMarkerId_{1};
    std::vector<MarkerEvent> markers_;
};

class RulerTrack final : public TrackBase {
public:
    RulerTrack(std::uint32_t id, std::string name, RulerDisplayMode mode)
        : TrackBase(id, std::move(name), TrackType::RulerTrack), mode_(mode) {}
    [[nodiscard]] RulerDisplayMode displayMode() const noexcept { return mode_; }
    void setDisplayMode(RulerDisplayMode m) noexcept { mode_ = m; }
private:
    RulerDisplayMode mode_{RulerDisplayMode::Seconds};
};

class TempoTrack final : public TrackBase {
public:
    TempoTrack(std::uint32_t id, MusicalClock& clock)
        : TrackBase(id, "Tempo", TrackType::TempoTrack), clock_(clock) {}
    [[nodiscard]] MusicalClock& clock() noexcept { return clock_; }
private:
    MusicalClock& clock_;
};

class SignatureTrack final : public TrackBase {
public:
    SignatureTrack(std::uint32_t id, MusicalClock& clock)
        : TrackBase(id, "Signature", TrackType::SignatureTrack), clock_(clock) {}
    [[nodiscard]] MusicalClock& clock() noexcept { return clock_; }
private:
    MusicalClock& clock_;
};

class ArrangerTrack final : public TrackBase {
public:
    ArrangerTrack(std::uint32_t id, std::string name = "Arranger")
        : TrackBase(id, std::move(name), TrackType::ArrangerTrack) {}

    std::uint32_t addRegion(const std::string& label, std::int64_t startTick, std::int64_t endTick);
    void appendChainStep(std::uint32_t regionId, std::uint32_t repeatCount);
    [[nodiscard]] const std::vector<ArrangerRegion>& regions() const noexcept { return regions_; }
    [[nodiscard]] const std::vector<ArrangerChainStep>& chain() const noexcept { return chain_; }

    // Computes flattened linear playback intervals [sourceStartTick, sourceEndTick]
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> buildFlattenedSchedule() const;

private:
    std::uint32_t nextRegionId_{1};
    std::vector<ArrangerRegion> regions_;
    std::vector<ArrangerChainStep> chain_;
};

class TransposeTrack final : public TrackBase {
public:
    TransposeTrack(std::uint32_t id, std::string name = "Transpose")
        : TrackBase(id, std::move(name), TrackType::TransposeTrack) {}

    void addTransposeChange(std::int64_t startTick, std::int32_t semitones);
    [[nodiscard]] std::int32_t transposeAtTick(std::int64_t tick) const noexcept;
    [[nodiscard]] const std::vector<GlobalTransposeEvent>& events() const noexcept { return events_; }

private:
    std::vector<GlobalTransposeEvent> events_;
};

// ============================================================================
// RetrospectiveRecord: Permanent Lock-Free Circular MIDI Buffer (10,000+ events)
// Captures incoming MIDI notes & controllers even while Transport is stopped
// or playing without Record Armed, materializing them into a MidiPartContainer.
// ============================================================================
class RetrospectiveRecordBuffer {
public:
    static constexpr std::size_t kCapacity = 16384; // Power of two (>= 10,000 events)

    // Real-time safe capture from MIDI input thread
    bool captureMidiEventRT(const MidiSequenceEvent& ev) noexcept;

    // Drains captured events into a timeline MidiPartContainer
    MidiPartContainer materializeCapturedPart(const std::string& partName = "Retrospective Capture");
    void clear() noexcept { ring_.reset(); }
    [[nodiscard]] std::size_t capturedCount() const noexcept { return ring_.readAvailable(); }

private:
    LockFreeRingBuffer<MidiSequenceEvent, kCapacity> ring_;
};

} // namespace AlphaStudio::Core
