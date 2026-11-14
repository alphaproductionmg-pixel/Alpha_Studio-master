#pragma once

#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"
#include "AlphaStudio/Core/Engine/AudioPool.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// OfflineProcessHistory: Non-destructive / Reversible Stack of Offline Clip
// Operations (Gain, Normalize, PhaseInvert, Reverse, FadeIn, FadeOut,
// DC Offset Removal, Silence, and Offline Plugin DSP Process)
// ============================================================================
enum class OfflineOpType : std::uint8_t {
    Gain             = 0,
    Normalize        = 1,
    PhaseInvert      = 2,
    Reverse          = 3,
    FadeIn           = 4,
    FadeOut          = 5,
    RemoveDCOffset   = 6,
    SilenceRegion    = 7,
    OfflinePluginDSP = 8
};

struct OfflineProcessStep {
    using OfflinePluginCallback = std::function<void(std::vector<float>& interleaved,
                                                     std::uint32_t channels,
                                                     double sampleRate)>;

    std::uint32_t stepId{0};
    OfflineOpType type{OfflineOpType::Gain};
    std::string description;
    float parameterDbOrRatio{0.0f}; // e.g. +3.0 dB for Gain, -0.1 dBFS target for Normalize
    std::size_t startFrame{0};
    std::size_t frameCount{0}; // 0 == entire clip
    bool enabled{true};
    OfflinePluginCallback pluginProcessor;
};

class OfflineProcessHistory {
public:
    OfflineProcessHistory(std::span<const float> originalInterleaved,
                          std::uint32_t channels, double sampleRate);

    std::uint32_t appendStep(OfflineProcessStep step);
    bool removeStepAt(std::size_t stackIndex);
    bool modifyStepParameter(std::size_t stackIndex, float newParameterDbOrRatio);
    bool toggleStepBypass(std::size_t stackIndex, bool enabled);
    void revertAll();

    [[nodiscard]] const std::vector<float>& renderedSamples() const noexcept { return currentRendered_; }
    [[nodiscard]] const std::vector<OfflineProcessStep>& steps() const noexcept { return steps_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

private:
    void recomputeFromPristine();
    static void applySingleStep(std::vector<float>& buf, std::uint32_t channels,
                                double sampleRate, const OfflineProcessStep& step);

    std::vector<float> pristineOriginal_;
    std::vector<float> currentRendered_;
    std::uint32_t channels_{2};
    double sampleRate_{48000.0};
    std::uint32_t nextStepId_{1};
    std::vector<OfflineProcessStep> steps_;
};

// ============================================================================
// ExtendedFreezeManager: Freezes & Unfreezes Audio Tracks and Instrument Tracks
// with exact preservation & restoration of Output Bus Routing Assignments
// ============================================================================
struct SavedBusRoutingSnapshot {
    std::uint32_t primaryOutputBusId{1};
    SpeakerConfig speakerConfig{SpeakerConfig::Stereo};
    std::vector<std::uint32_t> multiOutputBusIds; // For multi-out VSTis
    std::vector<bool> insertActiveStates;
    bool synthRackActive{true};
};

struct FrozenTrackRecord {
    std::uint32_t trackId{0};
    std::string trackName;
    bool isInstrumentTrack{false};
    bool unloadPluginsOnFreeze{true};
    double tailDurationSeconds{2.0};
    std::uint32_t freezePoolAssetId{0};
    std::string freezeFilePath;
    SavedBusRoutingSnapshot savedRouting{};
    bool currentlyFrozen{false};
};

class ExtendedFreezeManager {
public:
    using OfflineTrackRenderFn = std::function<std::vector<float>(std::size_t totalFrames,
                                                                  std::uint32_t channels)>;

    explicit ExtendedFreezeManager(AudioPool& pool);

    // Freezes an Audio or Virtual Instrument track, renders tail, deactivates CPU load,
    // and stores exact output bus routing snapshot.
    bool freezeTrack(std::uint32_t trackId,
                     const std::string& trackName,
                     bool isInstrumentTrack,
                     std::size_t contentFrames,
                     double tailSeconds,
                     bool unloadPlugins,
                     const SavedBusRoutingSnapshot& currentRouting,
                     const OfflineTrackRenderFn& renderFn);

    // Unfreezes track and returns the exact SavedBusRoutingSnapshot to restore
    bool unfreezeTrack(std::uint32_t trackId, SavedBusRoutingSnapshot& restoredRoutingOut,
                       bool deleteFreezeFile = true);

    [[nodiscard]] bool isTrackFrozen(std::uint32_t trackId) const noexcept;
    [[nodiscard]] const FrozenTrackRecord* getFrozenRecord(std::uint32_t trackId) const noexcept;

private:
    AudioPool& pool_;
    std::vector<FrozenTrackRecord> records_;
};

} // namespace AlphaStudio::Core
