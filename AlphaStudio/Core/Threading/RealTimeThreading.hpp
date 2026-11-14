#pragma once

#include "AlphaStudio/Core/Threading/LockFreeRingBuffer.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace AlphaStudio::Core {

enum class AudioPriority : std::uint8_t {
    Normal = 0,
    Boost  = 1
};

// ============================================================================
// PowerOptimizationHook: Neutralizes CPU C-states and frequency scaling during
// active real-time audio processing.
// ============================================================================
class PowerOptimizationHook {
public:
    static bool DisableCPU_EnergySaving(bool disable) noexcept;
    [[nodiscard]] static bool isEnergySavingDisabled() noexcept;

private:
    static std::atomic<bool> active_;
    static std::atomic<int> pmQosFd_;
};

class ScopedPowerOptimization {
public:
    ScopedPowerOptimization() noexcept {
        PowerOptimizationHook::DisableCPU_EnergySaving(true);
    }
    ~ScopedPowerOptimization() noexcept {
        PowerOptimizationHook::DisableCPU_EnergySaving(false);
    }
    ScopedPowerOptimization(const ScopedPowerOptimization&) = delete;
    ScopedPowerOptimization& operator=(const ScopedPowerOptimization&) = delete;
};

// ============================================================================
// AudioThreadManager: Real-time audio thread lifecycle, OS priority elevation,
// denormal FTZ/DAZ protection, and CPU load metering.
// ============================================================================
class AudioThreadManager {
public:
    using AudioRenderCallback = void (*)(void* userData, std::uint32_t numFrames) noexcept;

    AudioThreadManager() noexcept;
    ~AudioThreadManager() noexcept;

    AudioThreadManager(const AudioThreadManager&) = delete;
    AudioThreadManager& operator=(const AudioThreadManager&) = delete;

    void setPriority(AudioPriority priority) noexcept;
    [[nodiscard]] AudioPriority priority() const noexcept;

    static void enableDenormalProtection() noexcept;
    static bool applyCurrentThreadRealtimePriority(AudioPriority priority) noexcept;

    bool startDrivenLoop(AudioRenderCallback callback, void* userData,
                         double sampleRate, std::uint32_t blockSize);
    void stopDrivenLoop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void recordBlockExecutionTime(double elapsedMicroseconds, double budgetMicroseconds) noexcept;
    [[nodiscard]] float averageCpuLoadPercent() const noexcept {
        return avgLoadPercent_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float peakCpuLoadPercent() const noexcept {
        return peakLoadPercent_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t xrunCount() const noexcept {
        return xrunCount_.load(std::memory_order_relaxed);
    }
    void resetMeters() noexcept;

private:
    std::atomic<AudioPriority> priority_{AudioPriority::Boost};
    std::atomic<bool> running_{false};
    std::atomic<float> avgLoadPercent_{0.0f};
    std::atomic<float> peakLoadPercent_{0.0f};
    std::atomic<std::uint64_t> xrunCount_{0};
    std::thread audioThread_;
};

// ============================================================================
// MultiProcessingPool: Lock-free parallel execution of independent DAG layers
// across multi-core processors (> 4 cores) at low ASIO latency.
// Zero heap allocations and zero mutexes on the real-time dispatch path.
// ============================================================================
struct ParallelAudioTask {
    using TaskFn = void (*)(void* context, std::uint32_t nodeIndex, std::uint32_t numFrames) noexcept;
    TaskFn fn{nullptr};
    void* context{nullptr};
    std::uint32_t nodeIndex{0};
    std::uint32_t numFrames{0};
};

class MultiProcessingPool {
public:
    static constexpr std::size_t kMaxTasksPerLayer = 512;
    static constexpr std::size_t kMaxWorkerThreads = 32;

    MultiProcessingPool() noexcept;
    ~MultiProcessingPool() noexcept;

    MultiProcessingPool(const MultiProcessingPool&) = delete;
    MultiProcessingPool& operator=(const MultiProcessingPool&) = delete;

    void initialize(std::uint32_t requestedThreads = 0, AudioPriority priority = AudioPriority::Boost);
    void shutdown() noexcept;

    void setEnabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_release);
    }
    [[nodiscard]] bool isEnabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t workerCount() const noexcept {
        return activeWorkerCount_;
    }
    [[nodiscard]] bool isMultiCoreOptimized() const noexcept {
        return hardwareCores_ > 4 && activeWorkerCount_ > 1;
    }

    // Real-time safe: dispatches a batch of independent nodes belonging to the
    // same topological DAG depth and waits lock-free until completion.
    void executeLayerParallel(std::span<const ParallelAudioTask> tasks) noexcept;

private:
    void workerLoop(std::uint32_t workerId) noexcept;

    std::uint32_t hardwareCores_{1};
    std::uint32_t activeWorkerCount_{0};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> shuttingDown_{false};

    alignas(64) std::array<ParallelAudioTask, kMaxTasksPerLayer> currentBatch_{};
    alignas(64) std::atomic<std::uint32_t> batchSize_{0};
    alignas(64) std::atomic<std::uint32_t> nextTaskIndex_{0};
    alignas(64) std::atomic<std::uint32_t> remainingTasks_{0};
    alignas(64) std::atomic<std::uint64_t> epoch_{0};

    std::vector<std::thread> workers_;
};

// ============================================================================
// DiskIOThread: Asynchronous Disk Preload & Record Flush engine.
// Supports configurable DiskPreload duration (e.g., 2.0 seconds) for glitch-free
// multi-track streaming.
// ============================================================================
struct DiskStreamSlot {
    static constexpr std::size_t kRingSamples = 524288; // Power of two (~5.46s stereo @ 48kHz)

    std::atomic<bool> active{false};
    std::atomic<bool> recordMode{false};
    std::atomic<std::uint64_t> fileSampleCursor{0};
    std::atomic<std::uint64_t> totalFileSamples{0};
    std::uint32_t channels{2};
    std::string filePath;
    std::vector<float> BackingMemoryCache; // Pre-mapped or file-backed source frames
    LockFreeRingBuffer<float, kRingSamples> sampleRing;
};

class DiskIOThread {
public:
    static constexpr std::size_t kMaxStreams = 128;

    DiskIOThread();
    ~DiskIOThread() noexcept;

    DiskIOThread(const DiskIOThread&) = delete;
    DiskIOThread& operator=(const DiskIOThread&) = delete;

    void setDiskPreloadSeconds(double seconds) noexcept;
    [[nodiscard]] double diskPreloadSeconds() const noexcept {
        return preloadSeconds_.load(std::memory_order_relaxed);
    }

    void start(double sampleRate);
    void stop() noexcept;

    // Non-RT setup methods (called from project/UI thread)
    int registerPlaybackStream(const std::string& path, std::uint32_t channels,
                               std::span<const float> interleavedAudio);
    int registerRecordStream(const std::string& path, std::uint32_t channels);
    void unregisterStream(int streamId) noexcept;
    void seekStream(int streamId, std::uint64_t framePosition) noexcept;

    // Real-time safe audio thread methods (zero allocation, zero locks)
    std::size_t readPlaybackFramesRT(int streamId, float* interleavedOut,
                                     std::size_t frames, std::uint32_t channels) noexcept;
    std::size_t writeRecordFramesRT(int streamId, const float* interleavedIn,
                                    std::size_t frames, std::uint32_t channels) noexcept;

    [[nodiscard]] float diskCacheLoadPercent() const noexcept {
        return diskLoadPercent_.load(std::memory_order_relaxed);
    }

private:
    void ioWorkerLoop();
    void refillPlaybackSlot(DiskStreamSlot& slot, std::size_t targetPreloadSamples);
    void drainRecordSlot(DiskStreamSlot& slot);

    std::atomic<double> preloadSeconds_{2.0};
    std::atomic<double> sampleRate_{48000.0};
    std::atomic<bool> running_{false};
    std::atomic<float> diskLoadPercent_{0.0f};
    std::unique_ptr<std::array<DiskStreamSlot, kMaxStreams>> slots_;
    std::thread ioThread_;
};

} // namespace AlphaStudio::Core
