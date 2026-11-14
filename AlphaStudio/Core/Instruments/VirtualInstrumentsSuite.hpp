#pragma once

#include "AlphaStudio/Core/DSP/AudioWarpAndSampleEditor.hpp"
#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"
#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AlphaStudio::Core::Instruments {

// ============================================================================
// Clean-Room Native Virtual Instrument Interface (Zero Allocation in RT)
// ============================================================================
class IVirtualInstrument {
public:
    virtual ~IVirtualInstrument() = default;

    [[nodiscard]] virtual const char* instrumentName() const noexcept = 0;
    virtual void prepare(double sampleRate, std::uint32_t maxBlockFrames) = 0;
    virtual void reset() noexcept = 0;

    virtual void noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept = 0;
    virtual void noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept = 0;
    virtual void controlChangeRT(std::uint8_t midiChannel, std::uint8_t cc, float value01) noexcept {
        (void)midiChannel;
        (void)cc;
        (void)value01;
    }

    virtual void renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept = 0;

    virtual void setParameterNormalized(std::uint32_t paramId, float value01) noexcept = 0;
    [[nodiscard]] virtual float getParameterNormalized(std::uint32_t paramId) const noexcept = 0;
};

// ============================================================================
// 1. HALion ONE: 16-Channel Multitimbral Sample Player
//    (Envelopes, Resonant Filter, LFO, Bank & Program Management)
// ============================================================================
struct HalionSampleBankProgram {
    std::string bankName{"Factory Acoustic & Synth ROM"};
    std::string programName{"Concert Grand Piano"};
    float attackSec{0.005f};
    float decaySec{0.35f};
    float sustainLevel{0.70f};
    float releaseSec{0.25f};
    float cutoffHz{6500.0f};
    float resonance{1.2f};
    float lfoRateHz{4.5f};
    float lfoPitchDepth{0.08f};
    std::vector<float> singleCycleOrPcmWave;
};

class HALionOneInstrument final : public IVirtualInstrument {
public:
    static constexpr std::size_t kMidiChannels = 16;
    static constexpr std::size_t kMaxVoices = 32;

    HALionOneInstrument();

    [[nodiscard]] const char* instrumentName() const noexcept override { return "HALion ONE"; }
    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept override;
    void noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept override;
    void renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept override;
    void setParameterNormalized(std::uint32_t paramId, float value01) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;

    void selectChannelProgram(std::uint8_t ch0To15, std::size_t bankProgramIdx) noexcept;
    [[nodiscard]] std::size_t bankProgramCount() const noexcept { return bank_.size(); }

private:
    struct Voice {
        bool active{false};
        bool releasing{false};
        std::uint8_t channel{0};
        std::uint8_t pitch{60};
        float velocity{0.8f};
        double phase{0.0};
        double lfoPhase{0.0};
        float env{0.0f};
        float lpState{0.0f};
    };

    double sampleRate_{48000.0};
    float masterCutoffScale_{1.0f};
    float masterReleaseScale_{1.0f};
    std::vector<HalionSampleBankProgram> bank_;
    std::array<std::size_t, kMidiChannels> channelProgramIdx_{};
    std::array<Voice, kMaxVoices> voices_{};
};

// ============================================================================
// 2. Groove Agent ONE: Drum Pad Sampler
//    (WAV/AIFF/MPC/Slice Import, Sample Start/End Trim, Pad Replace/Swap, Kit Archive)
// ============================================================================
struct GrooveAgentPad {
    std::uint8_t triggerMidiNote{36};
    std::string padName{"Kick 1"};
    std::vector<float> monoSamples;
    std::size_t sampleStartFrame{0};
    std::size_t sampleEndFrame{0};
    float gainLinear{1.0f};
    float tuneSemitones{0.0f};
};

class GrooveAgentOneInstrument final : public IVirtualInstrument {
public:
    static constexpr std::size_t kNumPads = 64;
    static constexpr std::size_t kMaxPolyTriggers = 24;

    GrooveAgentOneInstrument();

    [[nodiscard]] const char* instrumentName() const noexcept override { return "Groove Agent ONE"; }
    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept override;
    void noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept override;
    void renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept override;
    void setParameterNormalized(std::uint32_t paramId, float value01) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;

    // Import WAV / AIFF / MPC .pgm / SampleEditor Slices + Start/End Trim + Pad Replacement + Kit Archive
    bool importSampleToPad(std::size_t padIdx, const std::string& name, std::span<const float> monoPcm);
    bool importAiffBytesToPad(std::size_t padIdx, std::span<const std::uint8_t> aiffBytes);
    std::size_t importMpcProgramDescriptor(const std::string& mpcProgramText);
    std::size_t importSlicesAcrossPads(std::span<const DSP::AudioSliceRegion> slices, std::size_t firstPadIdx = 0);
    void setPadStartEndFrames(std::size_t padIdx, std::size_t startFrame, std::size_t endFrame) noexcept;
    void replaceOrSwapPads(std::size_t padA, std::size_t padB);
    [[nodiscard]] std::string serializeKitArchive() const;
    bool loadKitArchive(const std::string& archiveText);
    [[nodiscard]] const GrooveAgentPad& pad(std::size_t idx) const noexcept { return pads_[idx]; }

private:
    struct ActiveHit {
        bool active{false};
        std::size_t padIndex{0};
        double cursor{0.0};
        double step{1.0};
        float velocity{1.0f};
    };

    double sampleRate_{48000.0};
    float masterGain_{1.0f};
    std::array<GrooveAgentPad, kNumPads> pads_{};
    std::array<ActiveHit, kMaxPolyTriggers> hits_{};
};

// ============================================================================
// 3. LoopMash: Synchronous Real-Time 8-Loop Slice Mashup Engine
//    (Automatable SimilarityThreshold & per-track TrackVolume[0..7])
// ============================================================================
class LoopMashInstrument final : public IVirtualInstrument {
public:
    static constexpr std::size_t kMaxLoopTracks = 8;
    static constexpr std::size_t kSlicesPerLoop = 8;

    LoopMashInstrument();

    [[nodiscard]] const char* instrumentName() const noexcept override { return "LoopMash"; }
    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept override;
    void noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept override;
    void renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept override;

    // Automatable SimilarityThreshold (param 0) and TrackVolume 1..8 (params 1..8)
    void setParameterNormalized(std::uint32_t paramId, float value01) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;

    void loadLoopIntoTrack(std::size_t trackIdx0To7, std::span<const float> monoLoop);
    void setSimilarityThreshold(float thresh01) noexcept { similarityThreshold_ = thresh01; }
    [[nodiscard]] float similarityThreshold() const noexcept { return similarityThreshold_; }
    void setTrackVolume(std::size_t trackIdx0To7, float vol01) noexcept;

private:
    struct SliceFeature {
        float rmsEnergy{0.0f};
        float zeroCrossingRate{0.0f};
        std::vector<float> audio;
    };

    static SliceFeature analyzeSlice(std::span<const float> audio);
    [[nodiscard]] static float computeSimilarity01(const SliceFeature& a, const SliceFeature& b) noexcept;

    double sampleRate_{48000.0};
    bool transportPlaying_{true};
    float similarityThreshold_{0.65f};
    std::array<float, kMaxLoopTracks> trackVolumes_{1.0f, 0.85f, 0.80f, 0.75f, 0.7f, 0.7f, 0.7f, 0.7f};
    std::array<bool, kMaxLoopTracks> trackLoaded_{};
    std::array<std::array<SliceFeature, kSlicesPerLoop>, kMaxLoopTracks> slices_{};
    std::size_t currentSliceIndex_{0};
    std::size_t frameWithinSlice_{0};
};

// ============================================================================
// 4..8. Prologue, Mystic, Spector, Embracer & Monologue Synthesizers
// ============================================================================
enum class NativeSynthModel : std::uint8_t {
    Prologue_3OscPolySubtractive   = 0, // 3 Oscillators + Multimode Filter + Modulation Matrix
    Mystic_3CombPhysicalModel      = 1, // 3 Damped Comb Filters / Resonant Delay Lines
    Spector_2OscSpectralFilterBank = 2, // 2 Oscillators + 6-Band Morphing Spectral Filter Bank
    Embracer_SurroundPadSynth      = 3, // Polyphonic Pad Synth with Stereo/5.1 Surround Width
    Monologue_MonoAnalogOverdrive  = 4  // Monophonic Virtual-Analog Bass/Lead + Glide + Overdrive
};

struct ModMatrixSlot {
    bool enabled{false};
    std::uint8_t source{0};      // 0=LFO, 1=ModEnv, 2=Velocity
    std::uint8_t destination{0}; // 0=Cutoff, 1=OscPitch, 2=SurroundWidth
    float depth{0.0f};           // -1.0 .. +1.0
};

class UnifiedSynthesisInstrument final : public IVirtualInstrument {
public:
    static constexpr std::size_t kMaxVoices = 16;
    static constexpr std::size_t kCombBufLen = 2048;

    UnifiedSynthesisInstrument(std::string exactName, NativeSynthModel model);

    [[nodiscard]] const char* instrumentName() const noexcept override { return name_.c_str(); }
    [[nodiscard]] NativeSynthModel model() const noexcept { return model_; }

    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept override;
    void noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept override;
    void renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept override;
    void setParameterNormalized(std::uint32_t paramId, float value01) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;

    void configureModMatrixSlot(std::size_t slot0To3, std::uint8_t src, std::uint8_t dst, float depth) noexcept;

private:
    struct SynthVoice {
        bool active{false};
        bool releasing{false};
        std::uint8_t pitch{60};
        float velocity{0.8f};
        double currentFreqHz{261.63};
        double targetFreqHz{261.63};
        double phase1{0.0}, phase2{0.0}, phase3{0.0};
        float ampEnv{0.0f};
        float modEnv{0.0f};
        float svfIc1{0.0f}, svfIc2{0.0f};
        std::size_t combWrite{0};
        std::array<float, kCombBufLen> combLine1{};
        std::array<float, kCombBufLen> combLine2{};
        std::array<float, kCombBufLen> combLine3{};
        float combLp1{0.0f}, combLp2{0.0f}, combLp3{0.0f};
        std::array<float, 6> spectralBpIc1{};
        std::array<float, 6> spectralBpIc2{};
    };

    std::string name_;
    NativeSynthModel model_;
    double sampleRate_{48000.0};
    double globalLfoPhase_{0.0};
    std::uint32_t noiseRng_{0x778899AAU};

    // Normalized parameters: [0]=Cutoff, [1]=Resonance, [2]=EnvMod/Morph,
    //                        [3]=Drive/Damping, [4]=StereoSurroundWidth, [5]=Glide/Detune
    std::array<float, 6> params_{0.62f, 0.35f, 0.50f, 0.45f, 0.70f, 0.25f};
    std::array<ModMatrixSlot, 4> modMatrix_{};
    std::array<SynthVoice, kMaxVoices> voices_{};
};

// ============================================================================
// InstrumentRegistry: Registers All 8 Native Virtual Instruments
// ============================================================================
struct InstrumentRegistryEntry {
    std::string name;
    std::function<std::shared_ptr<IVirtualInstrument>()> factory;
};

class InstrumentRegistry {
public:
    InstrumentRegistry();

    [[nodiscard]] std::shared_ptr<IVirtualInstrument> createByName(std::string_view exactName) const;
    [[nodiscard]] const std::vector<InstrumentRegistryEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

private:
    std::vector<InstrumentRegistryEntry> entries_;
};

} // namespace AlphaStudio::Core::Instruments
