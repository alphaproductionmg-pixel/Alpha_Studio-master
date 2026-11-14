#pragma once

#include "AlphaStudio/SDK/ASIO/ASIOHal.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// DelayCompensationLine: Pre-allocated real-time safe delay buffer for PDC
// ============================================================================
class DelayCompensationLine {
public:
    static constexpr std::size_t kMaxCompensationSamples = 65536; // Power of two
    static constexpr std::size_t kMask = kMaxCompensationSamples - 1;
    static constexpr std::size_t kMaxChannels = 6; // Up to Surround 5.1

    DelayCompensationLine();

    void prepare(std::uint32_t channels);
    void setDelaySamples(std::uint32_t delaySamples) noexcept;
    [[nodiscard]] std::uint32_t delaySamples() const noexcept { return delaySamples_; }
    void reset() noexcept;

    // Real-time safe in-place block delay compensation (zero heap alloc)
    void processInPlaceRT(float* const* channelBuffers, std::uint32_t numChannels,
                          std::uint32_t numFrames) noexcept;

private:
    std::uint32_t channels_{2};
    std::uint32_t delaySamples_{0};
    std::size_t writePos_{0};
    std::array<std::vector<float>, kMaxChannels> ringStorage_{};
};

// ============================================================================
// VSTAudioSystem: Manages ASIO Driver, Sample Rate, Block Size, Round-Trip
// Latency, Automatic Plugin Delay Compensation (PDC), and Record Latency Offset
// ============================================================================
class VSTAudioSystem {
public:
    using AudioProcessHook = std::function<void(const float* const* inputs, long numIn,
                                                float* const* outputs, long numOut,
                                                long sampleFrames)>;

    VSTAudioSystem();
    ~VSTAudioSystem();

    VSTAudioSystem(const VSTAudioSystem&) = delete;
    VSTAudioSystem& operator=(const VSTAudioSystem&) = delete;

    bool initialize(std::shared_ptr<SDK::ASIO::IASIODriverAdapter> driver,
                    double sampleRate = 48000.0,
                    std::uint32_t blockSize = 256,
                    long activeInputs = 16,
                    long activeOutputs = 16);
    void shutdown();

    bool setSampleRate(double sampleRate);
    bool setBlockSize(std::uint32_t blockSize);

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint32_t blockSize() const noexcept { return blockSize_; }

    // Hardware & Round-Trip Latency
    [[nodiscard]] long inputLatencySamples() const noexcept { return inputLatencySamples_; }
    [[nodiscard]] long outputLatencySamples() const noexcept { return outputLatencySamples_; }
    [[nodiscard]] long roundTripLatencySamples() const noexcept {
        return inputLatencySamples_ + outputLatencySamples_;
    }
    [[nodiscard]] double inputLatencyMilliseconds() const noexcept;
    [[nodiscard]] double outputLatencyMilliseconds() const noexcept;
    [[nodiscard]] double roundTripLatencyMilliseconds() const noexcept;

    // Automatic Plugin Delay Compensation (PDC)
    void setConstrainDelayCompensation(bool constrain, double thresholdMs = 0.0) noexcept;
    [[nodiscard]] bool constrainDelayCompensation() const noexcept { return constrainPDC_; }
    [[nodiscard]] double constrainThresholdMs() const noexcept { return constrainThresholdMs_; }

    void updateGlobalSystemPDCLatency(std::uint32_t maxGraphLatencySamples) noexcept {
        globalPDCLatencySamples_.store(maxGraphLatencySamples, std::memory_order_release);
    }
    [[nodiscard]] std::uint32_t globalSystemPDCLatencySamples() const noexcept {
        return globalPDCLatencySamples_.load(std::memory_order_acquire);
    }

    // AdjustForRecordLatency & Manual Record Shift
    void setAdjustForRecordLatency(bool enabled) noexcept {
        adjustForRecordLatency_.store(enabled, std::memory_order_release);
    }
    [[nodiscard]] bool adjustForRecordLatency() const noexcept {
        return adjustForRecordLatency_.load(std::memory_order_acquire);
    }
    void setRecordShiftSamples(std::int64_t manualShiftSamples) noexcept {
        recordShiftSamples_.store(manualShiftSamples, std::memory_order_release);
    }
    [[nodiscard]] std::int64_t recordShiftSamples() const noexcept {
        return recordShiftSamples_.load(std::memory_order_acquire);
    }

    // Computes exact sample position adjustment applied to newly recorded events
    [[nodiscard]] std::int64_t computeRecordPlacementCompensationSamples(
        std::uint32_t inputChainPluginLatencySamples = 0) const noexcept;

    void setProcessHook(AudioProcessHook hook);
    bool startAudio();
    void stopAudio();
    bool processOfflineCycle() noexcept;

private:
    static void onAsioBufferSwitch(long doubleBufferIndex, long directProcess);
    static void onAsioSampleRateChanged(SDK::ASIO::ASIOSampleRate sRate);
    static long onAsioMessage(long selector, long value, void* message, double* opt);
    static SDK::ASIO::ASIOTime* onAsioBufferSwitchTimeInfo(
        SDK::ASIO::ASIOTime* params, long doubleBufferIndex, long directProcess);

    void handleBufferSwitchRT(long doubleBufferIndex) noexcept;
    void refreshHardwareLatencies() noexcept;

    static VSTAudioSystem* activeSystemInstance_;

    std::shared_ptr<SDK::ASIO::IASIODriverAdapter> driver_;
    double sampleRate_{48000.0};
    std::uint32_t blockSize_{256};
    long activeInputs_{16};
    long activeOutputs_{16};
    long inputLatencySamples_{0};
    long outputLatencySamples_{0};

    bool constrainPDC_{false};
    double constrainThresholdMs_{0.0};
    std::atomic<std::uint32_t> globalPDCLatencySamples_{0};
    std::atomic<bool> adjustForRecordLatency_{true};
    std::atomic<std::int64_t> recordShiftSamples_{0};

    std::vector<SDK::ASIO::ASIOBufferInfo> asioBuffers_;
    std::vector<const float*> inputChannelPtrs_;
    std::vector<float*> outputChannelPtrs_;
    SDK::ASIO::ASIOCallbacks asioCallbacks_{};
    AudioProcessHook processHook_;
    bool initialized_{false};
};

} // namespace AlphaStudio::Core
