#pragma once

#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// Logical Editor (MIDI, PPQ-Resolution Aware) & Project Logical Editor (PLE)
// Boolean Rule Engine: (FilterTarget + Condition + AND/OR) -> Action Mode
// (Transform, Select, Delete, Extract)
// ============================================================================
enum class LogicalBoolOp : std::uint8_t {
    And = 0,
    Or  = 1
};

enum class LogicalComparison : std::uint8_t {
    Equal        = 0,
    NotEqual     = 1,
    Greater      = 2,
    GreaterEqual = 3,
    Less         = 4,
    LessEqual    = 5,
    InsideRange  = 6,
    ContainsText = 7
};

enum class LogicalExecutionMode : std::uint8_t {
    Transform = 0,
    Select    = 1,
    Delete    = 2,
    Extract   = 3
};

// ---------------- MIDI Logical Editor ----------------
enum class MidiFilterTarget : std::uint8_t {
    EventType             = 0,
    Value1_PitchOrCC      = 1,
    Value2_VelocityOrVal  = 2,
    Channel               = 3,
    LengthInDisplayTicks  = 4, // Explicitly scaled by MIDI Display PPQ Resolution!
    PositionInBarTicks    = 5  // Explicitly scaled by MIDI Display PPQ Resolution!
};

enum class MidiTransformTarget : std::uint8_t {
    Value1_PitchOrCC     = 0,
    Value2_VelocityOrVal = 1,
    Channel              = 2,
    LengthTicks          = 3,
    PositionTicks        = 4
};

enum class TransformMathOp : std::uint8_t {
    SetFixedValue = 0,
    Add           = 1,
    Subtract      = 2,
    MultiplyRatio = 3
};

struct MidiLogicalFilterClause {
    LogicalBoolOp joinWithPrevious{LogicalBoolOp::And};
    MidiFilterTarget target{MidiFilterTarget::EventType};
    LogicalComparison comparison{LogicalComparison::Equal};
    std::int64_t paramA{0};
    std::int64_t paramB{0};
};

struct MidiLogicalTransformAction {
    MidiTransformTarget target{MidiTransformTarget::Value2_VelocityOrVal};
    TransformMathOp mathOp{TransformMathOp::Add};
    double operand{0.0};
};

struct MidiLogicalPreset {
    std::string presetName{"Accent Downbeats"};
    LogicalExecutionMode mode{LogicalExecutionMode::Transform};
    std::vector<MidiLogicalFilterClause> filters;
    std::vector<MidiLogicalTransformAction> actions;
};

class LogicalEditor {
public:
    // Evaluates MIDI rules taking into account the active clock Display Resolution PPQ!
    static std::vector<MidiSequenceEvent> applyPreset(
        const MidiLogicalPreset& preset,
        std::vector<MidiSequenceEvent>& targetEvents,
        const MusicalClock& clock);

    [[nodiscard]] static bool matchesAllClauses(
        const MidiSequenceEvent& ev,
        std::span<const MidiLogicalFilterClause> clauses,
        const MusicalClock& clock) noexcept;

private:
    static void applyTransform(MidiSequenceEvent& ev,
                               const MidiLogicalTransformAction& act,
                               const MusicalClock& clock) noexcept;
};

// ---------------- Project Logical Editor (PLE) ----------------
enum class PLEMediaTarget : std::uint8_t {
    TrackName        = 0,
    TrackMediaType   = 1, // AudioTrack, MIDITrack, InstrumentTrack, GroupChannelTrack, etc.
    PropertyIsMuted  = 2,
    PropertyIsSoloed = 3,
    PropertyIsArmed  = 4,
    ContainerCount   = 5
};

enum class PLEActionTarget : std::uint8_t {
    AppendTrackName = 0,
    PrependTrackName = 1,
    ReplaceTrackName = 2,
    SetTrackMute     = 3,
    SetTrackSolo     = 4,
    SetTrackColor    = 5
};

struct PLEFilterClause {
    LogicalBoolOp joinWithPrevious{LogicalBoolOp::And};
    PLEMediaTarget target{PLEMediaTarget::TrackName};
    LogicalComparison comparison{LogicalComparison::ContainsText};
    std::int64_t numericParam{0};
    std::string textParam;
};

struct PLETransformAction {
    PLEActionTarget target{PLEActionTarget::AppendTrackName};
    std::uint32_t numericOperand{0};
    std::string textOperand;
};

struct ProjectLogicalPreset {
    std::string name{"Color Vocal Tracks"};
    LogicalExecutionMode mode{LogicalExecutionMode::Transform};
    std::vector<PLEFilterClause> filters;
    std::vector<PLETransformAction> actions;
};

class ProjectLogicalEditor {
public:
    // Executes PLE preset across project tracks; returns matched/extracted track IDs
    static std::vector<std::uint32_t> executeOnProjectTracks(
        const ProjectLogicalPreset& preset,
        std::vector<std::unique_ptr<TrackBase>>& tracks);

    [[nodiscard]] static bool matchesTrack(
        const TrackBase& track,
        std::span<const PLEFilterClause> clauses);
};

} // namespace AlphaStudio::Core
