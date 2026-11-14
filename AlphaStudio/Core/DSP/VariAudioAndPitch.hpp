#pragma once

#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// VariAudio (Clean-Room):
// - Monophonic Pitch Detection via YIN / Autocorrelation
// - Editable Note Segmentation (Pitch, Micro-Cents, Tilt/Drift, Vibrato Scale, Length)
// - Real-Time & Offline TD-PSOLA Resynthesis
// - Audio-to-MIDI Note + PitchBend Extraction
// ============================================================================
struct VariAudioSegment {
    std::uint32_t segmentId{0};
    std::size_t startFrame{0};
    std::size_t endFrame{0};
    float detectedMidiPitch{60.0f};   // Continuous detected pitch (e.g. 60.34)
    std::int32_t targetMidiNote{60};  // Quantized/edited semitone
    float microPitchCents{0.0f};      // -100 .. +100 cents
    float pitchTiltSemitones{0.0f};   // Start-to-end drift/tilt adjustment
    float vibratoScale{1.0f};         // 0.0 = flat/straighten, 1.0 = natural, 2.0 = exaggerate
    float timeStretchRatio{1.0f};     // Segment length scaling
    std::vector<float> frameContourHz;
};

class VariAudioEngine {
public:
    explicit VariAudioEngine(double sampleRate = 48000.0);

    void setSampleRate(double sr) noexcept { sampleRate_ = sr; }

    // Clean-Room YIN Fundamental Frequency Estimator
    [[nodiscard]] static float detectPitchYIN(std::span<const float> windowSamples,
                                              double sampleRate,
                                              float yinThreshold = 0.15f,
                                              float* outConfidence = nullptr) noexcept;

    // Analyzes monophonic audio into editable VariAudioSegments
    void analyzeAudioClip(std::span<const float> monoSamples);

    [[nodiscard]] std::vector<VariAudioSegment>& segments() noexcept { return segments_; }
    [[nodiscard]] const std::vector<VariAudioSegment>& segments() const noexcept { return segments_; }

    void quantizePitchAll(float strength01 = 1.0f) noexcept;
    void straightenVibratoAll(float straightenAmount01 = 0.8f) noexcept;

    // Resynthesizes edited segments using Pitch-Synchronous Overlap-Add (TD-PSOLA)
    [[nodiscard]] std::vector<float> resynthesizePSOLA(std::span<const float> originalMono) const;

    // Extracts VariAudio segments to MIDI Note + PitchBend sequence events
    [[nodiscard]] std::vector<MidiSequenceEvent> extractAudioToMidi(
        const MusicalClock& clock, std::int64_t clipStartSample = 0,
        bool includePitchBend = true) const;

private:
    double sampleRate_{48000.0};
    std::vector<VariAudioSegment> segments_;
};

// ============================================================================
// PitchCorrect (Clean-Room): Automatic Scale-Quantized Pitch Correction
// with Phase-Coherent Pitch-Synchronous Synthesis (Zero Allocation in RT)
// ============================================================================
class PitchCorrectProcessor final : public IAudioInsertProcessor {
public:
    static constexpr std::size_t kAnalysisRingSize = 4096; // Power of two
    static constexpr std::size_t kRingMask = kAnalysisRingSize - 1;

    PitchCorrectProcessor();

    [[nodiscard]] const char* effectName() const noexcept override { return "PitchCorrect"; }
    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void processAudioRT(AudioBlock32& ioBuffer, const AudioBlock32* sidechainIn,
                        std::uint32_t numFrames) noexcept override;

    void setScaleMask(std::array<bool, 12> chromaticScaleActive) noexcept {
        scaleMask_ = chromaticScaleActive;
    }
    void setCorrectionSpeed(float speed01) noexcept { speed_ = speed01; }
    void setToleranceCents(float cents) noexcept { toleranceCents_ = cents; }
    void setTransposeSemitones(float semi) noexcept { transposeSemitones_ = semi; }

    void setParameterNormalized(std::uint32_t paramId, float normValue) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;
    [[nodiscard]] std::uint32_t parameterCount() const noexcept override { return 4U; }
    [[nodiscard]] const char* parameterName(std::uint32_t paramId) const noexcept override;

private:
    [[nodiscard]] float snapMidiNoteToActiveScale(float rawMidi) const noexcept;

    double sampleRate_{48000.0};
    float speed_{0.85f};
    float toleranceCents_{15.0f};
    float transposeSemitones_{0.0f};
    float dryWet_{1.0f};
    std::array<bool, 12> scaleMask_{true, false, true, false, true, true,
                                    false, true, false, true, false, true}; // C Major default

    std::array<float, kAnalysisRingSize> inputRing_{};
    std::array<float, 1024> yinScratch_{};
    std::size_t writePos_{0};
    double readPhaseA_{0.0};
    double readPhaseB_{512.0};
    float smoothedRatio_{1.0f};
    std::uint32_t hopCounter_{0};
};

} // namespace AlphaStudio::Core::DSP
