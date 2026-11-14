#pragma once

#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::DSP {

// ============================================================================
// REVerence (Clean-Room):
// - Partitioned Uniform Overlap-Save FFT Convolution Reverb (VST 3)
// - True Stereo Support (4-channel IR: LL, LR, RL, RR) + Standard Stereo
// - Integrated Output ChannelEQ
// - Parameter Fading Toggle (click-free crossfade when changing IR/Size)
// - DryWetLock (locks Dry/Wet mix e.g. 100% Wet on Group / FX Return channels)
// ============================================================================
class Radix2FFT {
public:
    // In-place Cooley-Tukey Radix-2 Complex FFT (power-of-two N, zero allocations!)
    static void transformInPlace(std::span<std::complex<float>> data, bool inverse) noexcept;
};

struct TrueStereoImpulseResponse {
    std::string name{"Grand Concert Hall True Stereo"};
    double sampleRate{48000.0};
    bool isTrueStereo4Ch{true}; // LL, LR, RL, RR
    std::vector<float> irLL;
    std::vector<float> irLR;
    std::vector<float> irRL;
    std::vector<float> irRR;
};

class REVerenceProcessor final : public IAudioInsertProcessor {
public:
    static constexpr std::size_t kPartitionSize = 256;
    static constexpr std::size_t kFftSize = kPartitionSize * 2; // 512

    REVerenceProcessor();

    [[nodiscard]] const char* effectName() const noexcept override { return "REVerence"; }
    void prepare(double sampleRate, std::uint32_t maxBlockFrames) override;
    void reset() noexcept override;
    void processAudioRT(AudioBlock32& ioBuffer, const AudioBlock32* sidechainIn,
                        std::uint32_t numFrames) noexcept override;

    // Impulse Response Loading & Synthetic Factory Hall Generator
    static TrueStereoImpulseResponse synthesizeFactoryHallIR(
        double sampleRate, double reverbTimeSeconds = 1.6, bool trueStereo = true);
    void loadImpulseResponse(const TrueStereoImpulseResponse& ir);

    // Parameter Fading & DryWetLock (for Group/FX channels)
    void setParameterFadingEnabled(bool enabled) noexcept { parameterFading_ = enabled; }
    [[nodiscard]] bool parameterFadingEnabled() const noexcept { return parameterFading_; }

    void setDryWetLock(bool locked) noexcept { dryWetLocked_ = locked; }
    [[nodiscard]] bool isDryWetLocked() const noexcept { return dryWetLocked_; }
    void setDryWetMix(float wet01, bool forceIgnoreLock = false) noexcept;
    [[nodiscard]] float dryWetMix() const noexcept { return wetMix_; }

    void setPreDelayMs(float ms) noexcept;
    [[nodiscard]] ChannelEQ& outputEQ() noexcept { return postEQ_; }

    void setParameterNormalized(std::uint32_t paramId, float normValue) noexcept override;
    [[nodiscard]] float getParameterNormalized(std::uint32_t paramId) const noexcept override;
    [[nodiscard]] std::uint32_t parameterCount() const noexcept override { return 5U; }
    [[nodiscard]] const char* parameterName(std::uint32_t paramId) const noexcept override;

private:
    struct PartitionedBank {
        bool trueStereo{true};
        std::size_t numPartitions{0};
        // Frequency-domain IR spectra: [partition][kFftSize]
        std::vector<std::array<std::complex<float>, kFftSize>> H_LL;
        std::vector<std::array<std::complex<float>, kFftSize>> H_LR;
        std::vector<std::array<std::complex<float>, kFftSize>> H_RL;
        std::vector<std::array<std::complex<float>, kFftSize>> H_RR;
    };

    static PartitionedBank buildPartitionedSpectra(const TrueStereoImpulseResponse& ir);
    void convolvePartitionBlockRT() noexcept;

    double sampleRate_{48000.0};
    bool parameterFading_{true};
    bool dryWetLocked_{false};
    float wetMix_{0.35f};
    float preDelayMs_{12.0f};
    std::uint32_t preDelaySamples_{576};
    float erTailBalance_{0.5f};

    PartitionedBank activeBank_{};
    PartitionedBank fadingOldBank_{};
    float crossfadeProgress_{1.0f};

    // Pre-allocated RT Frequency-Delay Line (FDL) & overlap-save rings (zero heap alloc in RT!)
    std::size_t maxPartitions_{64};
    std::size_t fdlPos_{0};
    std::vector<std::array<std::complex<float>, kFftSize>> fdlLeft_;
    std::vector<std::array<std::complex<float>, kFftSize>> fdlRight_;

    std::array<float, kFftSize> timeOverlapInL_{};
    std::array<float, kFftSize> timeOverlapInR_{};
    std::array<float, kPartitionSize> convOutBlockL_{};
    std::array<float, kPartitionSize> convOutBlockR_{};
    std::size_t frameInPartition_{0};

    std::array<std::complex<float>, kFftSize> fftWorkL_{};
    std::array<std::complex<float>, kFftSize> fftWorkR_{};
    std::array<std::complex<float>, kFftSize> accumOutL_{};
    std::array<std::complex<float>, kFftSize> accumOutR_{};

    DelayCompensationLine preDelayLine_;
    AudioBlock32 wetScratchBlock_;
    ChannelEQ postEQ_;
};

} // namespace AlphaStudio::Core::DSP
