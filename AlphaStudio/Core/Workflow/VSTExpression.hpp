#pragma once

#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// VST Expression: ExpressionMap with SoundSlots
// - Key Switches, Program Changes, and up to 2 MIDI CC messages per SoundSlot
// - Per-slot coloration for notes in KeyEditor
// - Pre/Post button routing support for MIDI Sends
// ============================================================================
enum class ArticulationBehavior : std::uint8_t {
    AttributePerNote   = 0, // Applies strictly to the tagged note
    DirectionPersistent = 1 // Persists until the next direction slot
};

struct SoundSlotOutputCC {
    bool enabled{false};
    std::uint8_t ccNumber{1};
    std::uint8_t ccValue{64};
};

struct ExpressionSoundSlot {
    std::uint16_t slotId{1};
    std::string name{"Sustain"};
    ArticulationBehavior behavior{ArticulationBehavior::DirectionPersistent};
    std::uint32_t noteColorRgba{0x4AA3FFFFU}; // Coloration in KeyEditor

    // Remote input trigger (e.g., live keyswitch played on MIDI controller)
    std::optional<std::uint8_t> remoteTriggerKeySwitch;

    // Output MIDI mapping sent to VSTi / External Synth:
    std::optional<std::uint8_t> outputKeySwitchNote;
    std::uint8_t outputKeySwitchVelocity{100};
    std::optional<std::uint8_t> outputProgramChange;
    std::optional<std::uint8_t> outputChannelOverride;
    std::array<SoundSlotOutputCC, 2> outputControllers{}; // Up to 2 MIDI CC per slot!
    std::int32_t velocityScalePercent{100};
    std::int32_t lengthScalePercent{100};
};

class ExpressionMap {
public:
    explicit ExpressionMap(std::string mapName = "Orchestral Strings Expression");

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    std::uint16_t addSoundSlot(ExpressionSoundSlot slot);
    [[nodiscard]] const ExpressionSoundSlot* findSlot(std::uint16_t slotId) const noexcept;
    [[nodiscard]] const ExpressionSoundSlot* findSlotByRemoteKeySwitch(std::uint8_t midiNote) const noexcept;
    [[nodiscard]] std::uint32_t colorForArticulationSlot(std::uint16_t slotId) const noexcept;
    [[nodiscard]] const std::vector<ExpressionSoundSlot>& slots() const noexcept { return slots_; }

    // Translates articulated sequencer notes into concrete KeySwitches, ProgramChanges,
    // and up to 2 MIDI CCs, while dispatching Pre-Insert vs Post-Insert MIDI Sends!
    std::vector<MidiSequenceEvent> evaluateTrackMidiWithExpression(
        MIDITrack& track,
        std::span<const MidiSequenceEvent> rawEvents,
        std::int64_t blockStartTick,
        std::int64_t blockEndTick,
        const MusicalClock& clock,
        std::array<std::vector<MidiSequenceEvent>, MIDITrack::kNumMidiSends>* routedMidiSendsOut = nullptr) const;

private:
    std::string name_;
    std::uint16_t nextSlotId_{1};
    std::vector<ExpressionSoundSlot> slots_;
};

} // namespace AlphaStudio::Core
