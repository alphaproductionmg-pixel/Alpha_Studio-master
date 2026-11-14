#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// AudioWarp & MPEX (Clean-Room):
// - Real-time WSOLA + Offline Transient-Preserving Spectral/Phase-Locked MPEX
// - Musical Warp Markers anchoring musical beats to sample frames
// ============================================================================
enum class MPEXQualityAlgorithm : std::uint8_t {
    RealtimeMusicalStandard = 0,
    MPEX_SoloMusical        = 1,
    MPEX_SoloFast           = 2,
    MPEX_PolyComplex        = 3
};

struct WarpMarker {
    double musicalBeatPosition{0.0};
    std::size_t sampleFramePosition{0};
};

class AudioWarpAndMPEX {
public:
    // Time-stretches & pitch-shifts interleaved audio while preserving sharp transients
    static std::vector<float> processStretchAndPitch(
        std::span<const float> interleavedIn,
        std::uint32_t channels,
        double sampleRate,
        double timeStretchRatio,       // e.g. 1.25 = 25% longer
        double pitchShiftSemitones,    // e.g. +3.0 semitones
        MPEXQualityAlgorithm algorithm = MPEXQualityAlgorithm::MPEX_PolyComplex);

    // Applies piecewise non-linear musical AudioWarp markers
    static std::vector<float> applyWarpMarkers(
        std::span<const float> interleavedIn,
        std::uint32_t channels,
        double sampleRate,
        double targetTempoBpm,
        std::span<const WarpMarker> markers);
};

// ============================================================================
// Sample Editor Engine:
// - HitpointDetection (transient energy/flux detector with threshold & beat filter)
// - Slicing (zero-crossing snap + de-click fades)
// - Arrow-key navigation (nextHitpoint / prevHitpoint)
// - Local Definition Mode Ruler (clip bars, signature, inferred tempo)
// ============================================================================
struct HitpointMarker {
    std::size_t framePosition{0};
    float transientStrength{0.0f}; // [0.0 .. 1.0]
    bool locked{false};
};

struct AudioSliceRegion {
    std::uint32_t sliceIndex{0};
    std::size_t startFrame{0};
    std::size_t endFrame{0};
    double musicalStartBeat{0.0};
    std::vector<float> interleavedSliceSamples;
};

struct DefinitionModeRuler {
    bool definitionModeActive{false};
    std::uint32_t numBars{1};
    std::uint32_t numerator{4};
    std::uint32_t denominator{4};
    double inferredTempoBpm{120.0};
    std::size_t gridStartOffsetFrames{0};
};

class SampleEditorEngine {
public:
    SampleEditorEngine() = default;

    void loadSampleBuffer(std::span<const float> interleaved, std::uint32_t channels, double sampleRate);

    // HitpointDetection & Slicing
    std::size_t detectHitpoints(float threshold01 = 0.30f, double minIntervalMs = 45.0);
    std::vector<AudioSliceRegion> createSlicesFromHitpoints(std::size_t declickFadeFrames = 32) const;

    // Arrow-key Hitpoint Navigation
    std::size_t selectNextHitpoint() noexcept;
    std::size_t selectPreviousHitpoint() noexcept;
    [[nodiscard]] std::size_t selectedHitpointIndex() const noexcept { return selectedHitpointIdx_; }
    [[nodiscard]] const std::vector<HitpointMarker>& hitpoints() const noexcept { return hitpoints_; }

    // Local Definition Mode Ruler
    void configureDefinitionRuler(std::uint32_t bars, std::uint32_t num = 4, std::uint32_t den = 4,
                                  std::size_t startOffsetFrames = 0) noexcept;
    [[nodiscard]] const DefinitionModeRuler& definitionRuler() const noexcept { return definitionRuler_; }

private:
    [[nodiscard]] std::size_t snapToNearestZeroCrossing(std::size_t framePos) const noexcept;

    std::vector<float> interleaved_;
    std::uint32_t channels_{2};
    double sampleRate_{48000.0};
    std::vector<HitpointMarker> hitpoints_;
    std::size_t selectedHitpointIdx_{0};
    DefinitionModeRuler definitionRuler_{};
};

} // namespace AlphaStudio::Core::DSP
