#pragma once

#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"
#include "AlphaStudio/Core/Workflow/LogicalEditors.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// Complete 18 MIDI Plug-ins Suite & MidiPluginRegistry:
//   Arpache 5, Arpache SX, Autopan, Beat Designer [64-step pattern sequencer],
//   Chorder, Compress, Context Gate, Density, Micro Tuner, MidiControl,
//   MidiEcho, MIDI Monitor, Note 2 CC, Quantizer, StepDesigner,
//   Track Control, Track FX, Transformer
// ============================================================================
enum class MidiPluginAlgorithm : std::uint8_t {
    Arpache5      = 0,
    ArpacheSX     = 1,
    Autopan       = 2,
    BeatDesigner  = 3, // Up to 64 steps drum pattern sequencer!
    Chorder       = 4,
    Compress      = 5,
    ContextGate   = 6,
    Density       = 7,
    MicroTuner    = 8,
    MidiControl   = 9,
    MidiEcho      = 10,
    MIDIMonitor   = 11,
    Note2CC       = 12,
    Quantizer     = 13,
    StepDesigner  = 14,
    TrackControl  = 15,
    TrackFX       = 16,
    Transformer   = 17
};

struct BeatDesignerDrumLane {
    std::uint8_t drumNotePitch{36}; // e.g. 36 Kick, 38 Snare, 42 Closed Hat
    std::array<std::uint8_t, 64> stepVelocities{}; // Up to 64 steps! (0 = off, 1..127 = hit)
    std::array<bool, 64> stepFlam{};
};

class SuiteMidiPlugin final : public IMidiInsertProcessor {
public:
    static constexpr std::size_t kMaxBeatDesignerSteps = 64;
    static constexpr std::size_t kNumDrumLanes = 8;

    SuiteMidiPlugin(std::string exactName, MidiPluginAlgorithm algo);

    [[nodiscard]] const char* pluginName() const noexcept override { return name_.c_str(); }
    [[nodiscard]] MidiPluginAlgorithm algorithm() const noexcept { return algo_; }

    void reset() noexcept override;
    void processMidiEvents(std::vector<MidiSequenceEvent>& events,
                           std::int64_t blockStartTick,
                           std::int64_t blockEndTick,
                           const MusicalClock& clock) override;

    void setParameterNormalized(std::uint32_t paramIdx, float value) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramIdx) const noexcept override;

    // Beat Designer 64-step programming API
    void setBeatDesignerStepCount(std::uint32_t steps1To64) noexcept;
    [[nodiscard]] std::uint32_t beatDesignerStepCount() const noexcept { return beatDesignerSteps_; }
    void setBeatDesignerStep(std::size_t lane0To7, std::size_t step0To63,
                             std::uint8_t velocity, bool flam = false) noexcept;
    [[nodiscard]] const std::array<BeatDesignerDrumLane, kNumDrumLanes>& beatDesignerLanes() const noexcept {
        return drumLanes_;
    }

    // Micro Tuner cent offsets [-100 .. +100] per pitch class C..B
    void setMicroTuningCents(std::size_t pitchClass0To11, float cents) noexcept;

    // MIDI Monitor log inspection
    [[nodiscard]] const std::vector<MidiSequenceEvent>& monitorLog() const noexcept { return monitorLog_; }

    // Transformer rule preset access
    [[nodiscard]] MidiLogicalPreset& transformerPreset() noexcept { return transformerPreset_; }

private:
    std::string name_;
    MidiPluginAlgorithm algo_;
    std::array<float, 8> params_{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

    std::uint32_t beatDesignerSteps_{64};
    std::array<BeatDesignerDrumLane, kNumDrumLanes> drumLanes_{};
    std::array<std::int8_t, 64> stepDesignerPitches_{};
    std::array<std::uint8_t, 64> stepDesignerVelocities_{};
    std::array<float, 12> microTuneCents_{};
    std::vector<MidiSequenceEvent> monitorLog_;
    MidiLogicalPreset transformerPreset_{};
    std::uint32_t rng_{0xCAFEBABEU};
};

struct MidiPluginRegistryEntry {
    std::string name;
    MidiPluginAlgorithm algorithm;
    std::function<std::shared_ptr<IMidiInsertProcessor>()> factory;
};

class MidiPluginRegistry {
public:
    MidiPluginRegistry();

    [[nodiscard]] std::shared_ptr<IMidiInsertProcessor> createByName(std::string_view exactName) const;
    [[nodiscard]] const std::vector<MidiPluginRegistryEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

private:
    void registerAll18MidiPlugins();
    std::vector<MidiPluginRegistryEntry> entries_;
};

} // namespace AlphaStudio::Core::DSP
