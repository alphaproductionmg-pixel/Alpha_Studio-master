#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// Musical Clock (480 PPQ internal engine, 1/16 = 120 ticks, Display Resolution
// configurable from 24 to 960 PPQ, Bars.Beats.Subbeats.Ticks formatting)
// ============================================================================
struct BarBeatSubbeatTick {
    std::int32_t bars{1};     // 1-based
    std::int32_t beats{1};    // 1-based
    std::int32_t subbeats{1}; // 1-based sixteenth subdivision
    std::int32_t ticks{0};    // 0-based in active display PPQ resolution
};

struct TempoEventNode {
    std::int64_t tickPosition{0};
    double bpm{120.0};
    bool rampToNext{false}; // False = Jump/Step, True = Linear Ramp
};

struct TimeSignatureNode {
    std::int32_t barNumber{1}; // 1-based bar number
    std::int64_t tickPosition{0};
    std::int32_t numerator{4};
    std::int32_t denominator{4};
};

class MusicalClock {
public:
    static constexpr std::int32_t kDefaultInternalPPQ = 480;
    static constexpr std::int32_t kSixteenthNoteTicks = 120; // 480 / 4 = 120 ticks per 1/16

    MusicalClock();

    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] constexpr std::int32_t internalPPQ() const noexcept { return kDefaultInternalPPQ; }

    // Configurable MIDI Display Resolution [24 .. 960 PPQ]
    void setMidiDisplayResolutionPPQ(std::int32_t displayPpq) noexcept;
    [[nodiscard]] std::int32_t midiDisplayResolutionPPQ() const noexcept { return displayPpq_; }

    void setBaseTempo(double bpm);
    void addOrUpdateTempoNode(std::int64_t tickPos, double bpm, bool ramp = false);
    [[nodiscard]] double tempoAtTick(std::int64_t tickPos) const noexcept;
    [[nodiscard]] const std::vector<TempoEventNode>& tempoNodes() const noexcept { return tempoMap_; }

    // TimeWarp Tool: Warps the tempo of the preceding tempo node so that musicalTick
    // lands precisely at targetSampleFrame without shifting absolute audio events!
    bool applyTimeWarpAtTick(std::int64_t musicalTick, std::int64_t targetSampleFrame);

    void addOrUpdateSignatureNode(std::int32_t barNumber, std::int32_t numerator, std::int32_t denominator);
    [[nodiscard]] TimeSignatureNode signatureAtTick(std::int64_t tickPos) const noexcept;
    [[nodiscard]] const std::vector<TimeSignatureNode>& signatureNodes() const noexcept { return signatureMap_; }

    [[nodiscard]] std::int64_t ticksToSamples(std::int64_t tickPos) const noexcept;
    [[nodiscard]] std::int64_t samplesToTicks(std::int64_t samplePos) const noexcept;

    [[nodiscard]] BarBeatSubbeatTick tickToBBST(std::int64_t internalTick) const noexcept;
    [[nodiscard]] std::int64_t bbstToTick(const BarBeatSubbeatTick& bbst) const noexcept;
    [[nodiscard]] std::string formatBBST(std::int64_t internalTick) const;
    [[nodiscard]] std::int64_t parseBBST(const std::string& formatted) const noexcept;

private:
    void recalculateSignatureTicks() noexcept;

    double sampleRate_{48000.0};
    std::int32_t displayPpq_{kDefaultInternalPPQ};
    std::vector<TempoEventNode> tempoMap_;
    std::vector<TimeSignatureNode> signatureMap_;
};

// ============================================================================
// Sequencer MIDI Events, MIDI XF Metadata & Abstract MIDI Insert Interface
// ============================================================================
enum class SequencerMidiKind : std::uint8_t {
    Note          = 0,
    ControlChange = 1,
    ProgramChange = 2,
    PitchBend     = 3,
    Aftertouch    = 4,
    PolyPressure  = 5,
    SysExOrXF     = 6
};

struct MidiSequenceEvent {
    SequencerMidiKind kind{SequencerMidiKind::Note};
    std::int64_t startTick{0};
    std::int64_t lengthTicks{120};
    std::uint8_t channel{0};     // 0..15
    std::uint8_t pitchOrCC{60};  // Note 0..127 or CC number 0..127
    std::uint8_t velocityOrVal{100};
    std::uint8_t offVelocity{64};
    std::int16_t pitchBend14Bit{0}; // -8192 .. +8191
    std::uint16_t articulationSlotId{0}; // VST Expression SoundSlot Color/Articulation link
    bool selected{false};
    bool muted{false};
};

// MIDI XF (Extended Format: chords, lyrics, rehearsal marks)
struct MidiXFChordEvent {
    std::int64_t tickPosition{0};
    std::uint8_t rootNote{0};     // XF root encoding
    std::uint8_t chordType{0};    // Maj, Min7, Maj7, Dim, Sus4, etc.
    std::uint8_t bassNote{0};
    std::string lyricText;
};

class IMidiInsertProcessor {
public:
    virtual ~IMidiInsertProcessor() = default;

    [[nodiscard]] virtual const char* pluginName() const noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual void processMidiEvents(std::vector<MidiSequenceEvent>& events,
                                   std::int64_t blockStartTick,
                                   std::int64_t blockEndTick,
                                   const MusicalClock& clock) = 0;

    virtual void setParameterNormalized(std::uint32_t paramIdx, float value) noexcept {
        (void)paramIdx;
        (void)value;
    }
    [[nodiscard]] virtual float getParameterNormalized(std::uint32_t paramIdx) const noexcept {
        (void)paramIdx;
        return 0.0f;
    }
};

// ============================================================================
// VirtualPCKeyboard: Computer QWERTY/AZERTY Keyboard to Real-Time MIDI Mapper
// ============================================================================
class VirtualPCKeyboard {
public:
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    void setBaseOctave(int octave0To8) noexcept;
    [[nodiscard]] int baseOctave() const noexcept { return baseOctave_; }
    void setVelocity(std::uint8_t vel1To127) noexcept;
    [[nodiscard]] std::uint8_t velocity() const noexcept { return velocity_; }

    // Translates an ASCII character key press/release into a MidiSequenceEvent
    bool translateKeyToMidi(char keyAscii, bool keyDown, std::int64_t currentTick,
                            MidiSequenceEvent& outEvent) const noexcept;

private:
    bool enabled_{false};
    int baseOctave_{3}; // C3 = MIDI 60
    std::uint8_t velocity_{100};
};

} // namespace AlphaStudio::Core
