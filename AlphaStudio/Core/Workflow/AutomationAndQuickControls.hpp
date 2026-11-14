#pragma once

#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// AutomationEngine:
// - 4 Write Modes: Touch, Auto-Latch, Cross-Over, Trim (with last-value hold!)
// - Suspend Read / Suspend Write per parameter category
// - 4 Fill Modes: ToPunch, ToStart, ToEnd, Loop
// - RuleBasedConsolidation reconciling Part MIDI CCs with Track Automation
// - TrackQuickControls (8 assignable slots across Audio, Instrument, MIDI,
//   Group, FX, Input, and Output channels)
// ============================================================================
enum class AutomationWriteMode : std::uint8_t {
    Touch     = 0,
    AutoLatch = 1,
    CrossOver = 2,
    Trim      = 3 // Applies delta offset & preserves last trim value on release
};

enum class AutomationFillFlags : std::uint8_t {
    None    = 0,
    ToPunch = 1U << 0,
    ToStart = 1U << 1,
    ToEnd   = 1U << 2,
    Loop    = 1U << 3
};

enum class AutomationCategory : std::uint8_t {
    Volume  = 0,
    Pan     = 1,
    EQ      = 2,
    Sends   = 3,
    Plugins = 4,
    Mute    = 5
};

enum class MidiAutomationMergeRule : std::uint8_t {
    AutomationOnly     = 0,
    MidiPartPriority   = 1,
    AverageBoth        = 2,
    RelativeOffsetSum  = 3,
    ExtractCCToCurve   = 4
};

struct AutomationPoint {
    std::int64_t samplePosition{0};
    float normalizedValue{0.5f}; // [0.0 .. 1.0]
};

class AutomationLane {
public:
    AutomationLane(std::uint32_t paramId, std::string paramName,
                   AutomationCategory category = AutomationCategory::Volume,
                   float initialValue = 0.5f);

    [[nodiscard]] std::uint32_t parameterId() const noexcept { return paramId_; }
    [[nodiscard]] const std::string& parameterName() const noexcept { return paramName_; }
    [[nodiscard]] AutomationCategory category() const noexcept { return category_; }

    void setReadEnabled(bool r) noexcept { readEnabled_ = r; }
    [[nodiscard]] bool isReadEnabled() const noexcept { return readEnabled_; }
    void setWriteEnabled(bool w) noexcept { writeEnabled_ = w; }
    [[nodiscard]] bool isWriteEnabled() const noexcept { return writeEnabled_; }

    void insertOrReplacePoint(std::int64_t samplePos, float normVal);
    [[nodiscard]] float evaluateAtSample(std::int64_t samplePos) const noexcept;
    [[nodiscard]] const std::vector<AutomationPoint>& points() const noexcept { return points_; }

    // Real-time/Transport pass handling Touch, Auto-Latch, Cross-Over, Trim (holding last delta),
    // and the 4 Fill Modes!
    float processAutomationPass(std::int64_t currentSample,
                                std::int64_t projectLengthSamples,
                                std::int64_t loopStartSample,
                                std::int64_t loopEndSample,
                                bool controlTouched,
                                float liveNormalizedInput,
                                AutomationWriteMode mode,
                                std::uint8_t fillMask,
                                bool suspendRead,
                                bool suspendWrite);

    void commitTrimPass(std::int64_t passEndSample);

private:
    std::uint32_t paramId_{0};
    std::string paramName_;
    AutomationCategory category_{AutomationCategory::Volume};
    bool readEnabled_{true};
    bool writeEnabled_{false};
    float staticFallbackValue_{0.5f};

    bool latched_{false};
    std::int64_t punchInSample_{0};
    float crossOverInitialSide_{0.0f};
    float activeTrimDelta_{0.0f}; // Preserves last Trim value even after untouching!
    bool trimPassActive_{false};

    std::vector<AutomationPoint> points_;
};

class RuleBasedConsolidation {
public:
    // Reconciles MIDI CC events inside a MidiPartContainer with a Track AutomationLane
    // targeting the same destination parameter (e.g., CC7 <-> Volume, CC74 <-> Filter Cutoff).
    static float evaluateConsolidatedValue(
        AutomationLane& trackLane,
        MidiPartContainer& midiPart,
        std::uint8_t targetCCNumber,
        std::int64_t currentTick,
        std::int64_t currentSample,
        const MusicalClock& clock,
        MidiAutomationMergeRule rule);
};

// ============================================================================
// TrackQuickControls: 8 Assignable Quick Control Slots Extended Across
// Audio, Instrument, MIDI, Group, FX, Input, and Output Channels!
// ============================================================================
struct QuickControlAssignment {
    bool assigned{false};
    std::string label{"Unassigned"};
    std::uint32_t targetParameterId{0};
    float normalizedValue{0.5f};
    std::function<void(float)> setterCallback;
};

class TrackQuickControls {
public:
    static constexpr std::size_t kNumQuickControls = 8;

    explicit TrackQuickControls(MixerChannelKind hostKind = MixerChannelKind::AudioChannel);

    [[nodiscard]] MixerChannelKind hostChannelKind() const noexcept { return hostKind_; }
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    bool assignSlot(std::size_t slot0To7, const std::string& label,
                    std::uint32_t paramId, float initialNormalized,
                    std::function<void(float)> setter);
    void setQuickControlValue(std::size_t slot0To7, float normalized01);
    [[nodiscard]] float getQuickControlValue(std::size_t slot0To7) const noexcept;
    [[nodiscard]] const QuickControlAssignment& slot(std::size_t slot0To7) const noexcept {
        return slots_[slot0To7];
    }

    // Binds standard channel strip parameters (Volume, Pan, EQ Gains, Send 1..2)
    // for Audio, Instrument, Group, FX, Input, and Output channels!
    void bindDefaultChannelStripTargets(ChannelStrip& strip);

private:
    MixerChannelKind hostKind_{MixerChannelKind::AudioChannel};
    bool enabled_{true};
    std::array<QuickControlAssignment, kNumQuickControls> slots_{};
};

} // namespace AlphaStudio::Core
