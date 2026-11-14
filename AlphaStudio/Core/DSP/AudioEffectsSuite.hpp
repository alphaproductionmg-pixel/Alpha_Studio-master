#pragma once

#include "AlphaStudio/Core/DSP/REVerenceConvolution.hpp"
#include "AlphaStudio/Core/DSP/VariAudioAndPitch.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// Complete Parametric Audio Effects Suite & EffectRegistry
// Organised into 6 Real-Time DSP Families covering every single named effect:
//   AmpSimulator, AutoPan, Bitcrusher, Chopper, Chorus, Cloner, Compressor,
//   DaTube, DeEsser, Distortion, DualFilter, EnvelopeShaper, Expander, Flanger,
//   Gate, GEQ-10, GEQ-30, Grungelizer, Limiter, Maximizer, Metalizer, MIDI Gate,
//   Mix6to2, MixConvert, ModMachine, MonoDelay, MonoToStereo, MultibandCompressor,
//   MultiScope, Octaver, Phaser, PingPongDelay, PitchCorrect, REVerence,
//   RingModulator, Roomworks, Roomworks SE, Rotary, SMPTE Generator, SoftClipper,
//   Standard Channel EQ, StepFilter, StereoDelay, StereoEnhancer, StudioChorus,
//   StudioEQ, Surround Panner, Test Generator, ToneBooster, Tonic, Tranceformer,
//   Tremolo, Tuner, UV22 HR, Vibrato, VintageCompressor, VSTDynamics, WahWah
// ============================================================================
enum class AudioEffectCategory : std::uint8_t {
    Dynamics               = 0,
    EQAndFilters           = 1,
    DelayAndModulation     = 2,
    SaturationAndDistortion = 3,
    Reverb                 = 4,
    SpatialToolsAndAnalysis = 5
};

enum class AudioEffectAlgorithm : std::uint8_t {
    AmpSimulator, AutoPan, Bitcrusher, Chopper, Chorus, Cloner, Compressor,
    DaTube, DeEsser, Distortion, DualFilter, EnvelopeShaper, Expander, Flanger,
    Gate, GEQ10, GEQ30, Grungelizer, Limiter, Maximizer, Metalizer, MIDIGate,
    Mix6to2, MixConvert, ModMachine, MonoDelay, MonoToStereo, MultibandCompressor,
    MultiScope, Octaver, Phaser, PingPongDelay, RingModulator, Roomworks,
    RoomworksSE, Rotary, SMPTEGenerator, SoftClipper, StandardChannelEQ,
    StepFilter, StereoDelay, StereoEnhancer, StudioChorus, StudioEQ,
    SurroundPanner, TestGenerator, ToneBooster, Tonic, Tranceformer,
    Tremolo, Tuner, UV22HR, Vibrato, VintageCompressor, VSTDynamics, WahWah
};

struct EffectParameterSpec {
    std::string name;
    std::string units;
    float minVal{0.0f};
    float maxVal{1.0f};
    float defaultVal{0.5f};
};

// Clean-Room UV22 HR Psychoacoustic Ultrasonic Bias + TPDF Dithering Engine
class UV22HRDitherEngine {
public:
    void prepare(double sampleRate) noexcept;
    // Wordlength reduction for 16, 20, or 24-bit export/insert with Normal/Low/Autoblack
    float processSampleRT(float input, std::uint32_t targetBitDepth,
                          bool lowLevelMode, bool autoBlackMute) noexcept;

private:
    double sampleRate_{48000.0};
    double carrierPhase_{0.0};
    std::uint32_t rngState_{0x13579BDFU};
    std::uint32_t silentCounter_{0};
};

class ParametricSuiteAudioEffect final : public IAudioInsertProcessor {
public:
    ParametricSuiteAudioEffect(std::string exactName,
                               AudioEffectCategory category,
                               AudioEffectAlgorithm algo,
                               std::vector<EffectParameterSpec> paramSpecs);

    [[nodiscard]] const char* effectName() const noexcept override { return name_.c_str(); }
    [[nodiscard]] AudioEffectCategory category() const noexcept { return category_; }
    [[nodiscard]] AudioEffectAlgorithm algorithm() const noexcept { return algo_; }

    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void processAudioRT(AudioBlock32& ioBuffer, const AudioBlock32* sidechainIn,
                        std::uint32_t numFrames) noexcept override;

    [[nodiscard]] bool supportsSidechain() const noexcept override;

    void setParameterNormalized(std::uint32_t paramId, float normValue) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;
    [[nodiscard]] float getParameterPlain(std::uint32_t paramId) const noexcept;
    [[nodiscard]] std::uint32_t parameterCount() const noexcept override {
        return static_cast<std::uint32_t>(specs_.size());
    }
    [[nodiscard]] const char* parameterName(std::uint32_t paramId) const noexcept override;

    // Telemetry getters for Analysis effects (Tuner & MultiScope)
    [[nodiscard]] float detectedTunerFrequencyHz() const noexcept { return tunerDetectedHz_; }
    [[nodiscard]] float scopeCorrelation() const noexcept { return scopeStereoCorrelation_; }
    void triggerMidiGateNote(bool noteActive, float velocity01 = 1.0f) noexcept {
        midiGateOpen_ = noteActive;
        midiGateVel_ = velocity01;
    }

private:
    static constexpr std::size_t kDelayBufLen = 131072; // Power of two (~2.7s @ 48 kHz)
    static constexpr std::size_t kDelayMask   = kDelayBufLen - 1;

    struct SVFilterState {
        float ic1eq{0.0f};
        float ic2eq{0.0f};
        void processSample(float x, float cutoffHz, float q, double sr,
                           float& lp, float& bp, float& hp) noexcept;
    };

    std::string name_;
    AudioEffectCategory category_;
    AudioEffectAlgorithm algo_;
    std::vector<EffectParameterSpec> specs_;
    std::vector<float> normValues_;

    double sampleRate_{48000.0};
    double lfoPhase_{0.0};
    double oscPhase_{0.0};
    float envFollower_{0.0f};
    float fastEnv_{0.0f};
    float slowEnv_{0.0f};
    bool gateOpenState_{false};
    bool midiGateOpen_{true};
    float midiGateVel_{1.0f};
    float octaverFlip1_{1.0f};
    float octaverFlip2_{1.0f};
    float prevSignSample_{0.0f};
    std::uint32_t rng_{0x2468ACE1U};
    float pinkB0_{0.0f}, pinkB1_{0.0f}, pinkB2_{0.0f};
    float tunerDetectedHz_{440.0f};
    float scopeStereoCorrelation_{1.0f};
    std::uint64_t sampleCounter_{0};

    std::size_t delayWrite_{0};
    std::vector<float> delayLineL_;
    std::vector<float> delayLineR_;
    std::array<std::array<float, 6>, 2> allpassStates_{};
    std::array<SVFilterState, 6> svfLeft_{};
    std::array<SVFilterState, 6> svfRight_{};
    std::array<float, 30> geqZ1L_{}, geqZ2L_{}, geqZ1R_{}, geqZ2R_{};
    std::array<float, 1024> tunerScratch_{};
    ChannelEQ internalEQ_;
    UV22HRDitherEngine uv22Engine_;
};

struct EffectRegistryEntry {
    std::string name;
    AudioEffectCategory category;
    std::function<std::shared_ptr<IAudioInsertProcessor>()> factory;
};

class EffectRegistry {
public:
    EffectRegistry();

    [[nodiscard]] std::shared_ptr<IAudioInsertProcessor> createByName(std::string_view exactName) const;
    [[nodiscard]] const std::vector<EffectRegistryEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

private:
    void registerAll58Effects();
    std::vector<EffectRegistryEntry> entries_;
};

} // namespace AlphaStudio::Core::DSP
