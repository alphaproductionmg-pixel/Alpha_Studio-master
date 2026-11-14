#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#include <xmmintrin.h>
#endif

#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
#include <windows.h>
#include <avrt.h>
#elif defined(ALPHASTUDIO_PLATFORM_MACOS)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(ALPHASTUDIO_PLATFORM_LINUX)
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace AlphaStudio::Core {

// ============================================================================
// PowerOptimizationHook
// ============================================================================
std::atomic<bool> PowerOptimizationHook::active_{false};
std::atomic<int> PowerOptimizationHook::pmQosFd_{-1};

bool PowerOptimizationHook::DisableCPU_EnergySaving(bool disable) noexcept {
    if (disable) {
        bool expected = false;
        if (!active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return true;
        }
#if defined(ALPHASTUDIO_PLATFORM_LINUX)
        const int fd = ::open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            const std::int32_t zeroLatencyUs = 0;
            const ssize_t written = ::write(fd, &zeroLatencyUs, sizeof(zeroLatencyUs));
            (void)written;
            pmQosFd_.store(fd, std::memory_order_release);
        }
#elif defined(ALPHASTUDIO_PLATFORM_WINDOWS)
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
#endif
        return true;
    } else {
        bool expected = true;
        if (!active_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return true;
        }
#if defined(ALPHASTUDIO_PLATFORM_LINUX)
        const int fd = pmQosFd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            ::close(fd);
        }
#elif defined(ALPHASTUDIO_PLATFORM_WINDOWS)
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
        return true;
    }
}

bool PowerOptimizationHook::isEnergySavingDisabled() noexcept {
    return active_.load(std::memory_order_acquire);
}

// ============================================================================
// AudioThreadManager
// ============================================================================
AudioThreadManager::AudioThreadManager() noexcept = default;

AudioThreadManager::~AudioThreadManager() noexcept {
    stopDrivenLoop();
}

void AudioThreadManager::setPriority(AudioPriority priority) noexcept {
    priority_.store(priority, std::memory_order_release);
}

AudioPriority AudioThreadManager::priority() const noexcept {
    return priority_.load(std::memory_order_acquire);
}

void AudioThreadManager::enableDenormalProtection() noexcept {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _mm_setcsr(_mm_getcsr() | 0x8040U); // FTZ (bit 15) + DAZ (bit 6)
#endif
}

bool AudioThreadManager::applyCurrentThreadRealtimePriority(AudioPriority priority) noexcept {
    enableDenormalProtection();
#if defined(ALPHASTUDIO_PLATFORM_LINUX)
    sched_param param{};
    const int policy = SCHED_FIFO;
    const int minPrio = sched_get_priority_min(policy);
    const int maxPrio = sched_get_priority_max(policy);
    if (minPrio >= 0 && maxPrio >= minPrio) {
        param.sched_priority = (priority == AudioPriority::Boost)
            ? std::min(maxPrio, minPrio + ((maxPrio - minPrio) * 4) / 5)
            : std::min(maxPrio, minPrio + ((maxPrio - minPrio) * 2) / 5);
        if (pthread_setschedparam(pthread_self(), policy, &param) == 0) {
            return true;
        }
    }
    return false;
#elif defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    const int winPrio = (priority == AudioPriority::Boost)
        ? THREAD_PRIORITY_TIME_CRITICAL
        : THREAD_PRIORITY_HIGHEST;
    return ::SetThreadPriority(::GetCurrentThread(), winPrio) != 0;
#elif defined(ALPHASTUDIO_PLATFORM_MACOS)
    thread_time_constraint_policy_data_t policy{};
    policy.period = 2000000;
    policy.computation = (priority == AudioPriority::Boost) ? 1500000 : 1000000;
    policy.constraint = 1900000;
    policy.preemptible = 1;
    const kern_return_t kr = thread_policy_set(
        mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return kr == KERN_SUCCESS;
#else
    (void)priority;
    return false;
#endif
}

bool AudioThreadManager::startDrivenLoop(AudioRenderCallback callback, void* userData,
                                         double sampleRate, std::uint32_t blockSize) {
    if (callback == nullptr || sampleRate <= 0.0 || blockSize == 0) {
        return false;
    }
    stopDrivenLoop();
    running_.store(true, std::memory_order_release);
    PowerOptimizationHook::DisableCPU_EnergySaving(true);

    audioThread_ = std::thread([this, callback, userData, sampleRate, blockSize]() {
        applyCurrentThreadRealtimePriority(priority_.load(std::memory_order_acquire));
        const double budgetUs = (static_cast<double>(blockSize) / sampleRate) * 1000000.0;
        const auto period = std::chrono::microseconds(
            static_cast<std::int64_t>(std::max(100.0, budgetUs)));
        auto nextWake = std::chrono::steady_clock::now();

        AudioPriority appliedPrio = priority_.load(std::memory_order_relaxed);

        while (running_.load(std::memory_order_acquire)) {
            const AudioPriority targetPrio = priority_.load(std::memory_order_relaxed);
            if (targetPrio != appliedPrio) {
                applyCurrentThreadRealtimePriority(targetPrio);
                appliedPrio = targetPrio;
            }

            const auto t0 = std::chrono::steady_clock::now();
            callback(userData, blockSize);
            const auto t1 = std::chrono::steady_clock::now();

            const double elapsedUs =
                std::chrono::duration<double, std::micro>(t1 - t0).count();
            recordBlockExecutionTime(elapsedUs, budgetUs);

            nextWake += period;
            if (std::chrono::steady_clock::now() < nextWake) {
                std::this_thread::sleep_until(nextWake);
            } else {
                nextWake = std::chrono::steady_clock::now();
            }
        }
    });

    return true;
}

void AudioThreadManager::stopDrivenLoop() noexcept {
    running_.store(false, std::memory_order_release);
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    PowerOptimizationHook::DisableCPU_EnergySaving(false);
}

void AudioThreadManager::recordBlockExecutionTime(double elapsedMicroseconds,
                                                  double budgetMicroseconds) noexcept {
    if (budgetMicroseconds <= 0.0) {
        return;
    }
    const float instantLoad = static_cast<float>((elapsedMicroseconds / budgetMicroseconds) * 100.0);
    const float prevAvg = avgLoadPercent_.load(std::memory_order_relaxed);
    avgLoadPercent_.store(prevAvg * 0.92f + instantLoad * 0.08f, std::memory_order_relaxed);

    float prevPeak = peakLoadPercent_.load(std::memory_order_relaxed);
    while (instantLoad > prevPeak &&
           !peakLoadPercent_.compare_exchange_weak(prevPeak, instantLoad, std::memory_order_relaxed)) {
    }
    if (instantLoad > 100.0f) {
        xrunCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioThreadManager::resetMeters() noexcept {
    avgLoadPercent_.store(0.0f, std::memory_order_relaxed);
    peakLoadPercent_.store(0.0f, std::memory_order_relaxed);
    xrunCount_.store(0, std::memory_order_relaxed);
}

// ============================================================================
// MultiProcessingPool
// ============================================================================
MultiProcessingPool::MultiProcessingPool() noexcept {
    hardwareCores_ = std::max(1U, std::thread::hardware_concurrency());
}

MultiProcessingPool::~MultiProcessingPool() noexcept {
    shutdown();
}

void MultiProcessingPool::initialize(std::uint32_t requestedThreads, AudioPriority priority) {
    shutdown();
    hardwareCores_ = std::max(1U, std::thread::hardware_concurrency());

    std::uint32_t targetWorkers = requestedThreads;
    if (targetWorkers == 0) {
        targetWorkers = (hardwareCores_ > 1) ? (hardwareCores_ - 1) : 0;
    }
    targetWorkers = std::min<std::uint32_t>(targetWorkers, static_cast<std::uint32_t>(kMaxWorkerThreads));

    shuttingDown_.store(false, std::memory_order_release);
    epoch_.store(0, std::memory_order_relaxed);
    remainingTasks_.store(0, std::memory_order_relaxed);
    activeWorkerCount_ = targetWorkers;

    workers_.reserve(targetWorkers);
    for (std::uint32_t i = 0; i < targetWorkers; ++i) {
        workers_.emplace_back([this, i, priority]() {
            AudioThreadManager::applyCurrentThreadRealtimePriority(priority);
            workerLoop(i);
        });
    }
}

void MultiProcessingPool::shutdown() noexcept {
    shuttingDown_.store(true, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    epoch_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
    activeWorkerCount_ = 0;
}

void MultiProcessingPool::executeLayerParallel(std::span<const ParallelAudioTask> tasks) noexcept {
    if (tasks.empty()) {
        return;
    }

    if (!enabled_.load(std::memory_order_relaxed) || activeWorkerCount_ == 0 || tasks.size() == 1) {
        for (const auto& task : tasks) {
            if (task.fn != nullptr) {
                task.fn(task.context, task.nodeIndex, task.numFrames);
            }
        }
        return;
    }

    std::size_t offset = 0;
    while (offset < tasks.size()) {
        const std::size_t count = std::min<std::size_t>(tasks.size() - offset, kMaxTasksPerLayer);
        for (std::size_t i = 0; i < count; ++i) {
            currentBatch_[i] = tasks[offset + i];
        }

        nextTaskIndex_.store(0, std::memory_order_relaxed);
        batchSize_.store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);
        remainingTasks_.store(static_cast<std::uint32_t>(count), std::memory_order_release);

        epoch_.fetch_add(1, std::memory_order_acq_rel);
        epoch_.notify_all();

        // Calling audio thread actively participates in task execution (zero idle wait)
        while (true) {
            const std::uint32_t idx = nextTaskIndex_.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= count) {
                break;
            }
            const auto& task = currentBatch_[idx];
            if (task.fn != nullptr) {
                task.fn(task.context, task.nodeIndex, task.numFrames);
            }
            remainingTasks_.fetch_sub(1, std::memory_order_acq_rel);
        }

        // Brief hardware-friendly spin until worker threads complete remaining tasks in this layer
        while (remainingTasks_.load(std::memory_order_acquire) > 0) {
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }

        offset += count;
    }
}

void MultiProcessingPool::workerLoop(std::uint32_t /*workerId*/) noexcept {
    AudioThreadManager::enableDenormalProtection();
    std::uint64_t seenEpoch = epoch_.load(std::memory_order_acquire);

    while (!shuttingDown_.load(std::memory_order_acquire)) {
        epoch_.wait(seenEpoch, std::memory_order_acquire);
        seenEpoch = epoch_.load(std::memory_order_acquire);
        if (shuttingDown_.load(std::memory_order_acquire)) {
            break;
        }

        const std::uint32_t total = batchSize_.load(std::memory_order_acquire);
        while (true) {
            const std::uint32_t idx = nextTaskIndex_.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= total) {
                break;
            }
            const auto& task = currentBatch_[idx];
            if (task.fn != nullptr) {
                task.fn(task.context, task.nodeIndex, task.numFrames);
            }
            remainingTasks_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

// ============================================================================
// DiskIOThread
// ============================================================================
DiskIOThread::DiskIOThread()
    : slots_(std::make_unique<std::array<DiskStreamSlot, kMaxStreams>>()) {}

DiskIOThread::~DiskIOThread() noexcept {
    stop();
}

void DiskIOThread::setDiskPreloadSeconds(double seconds) noexcept {
    const double clamped = std::clamp(seconds, 0.25, 10.0);
    preloadSeconds_.store(clamped, std::memory_order_release);
}

void DiskIOThread::start(double sampleRate) {
    stop();
    sampleRate_.store(std::max(8000.0, sampleRate), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    ioThread_ = std::thread([this]() { ioWorkerLoop(); });
}

void DiskIOThread::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
}

int DiskIOThread::registerPlaybackStream(const std::string& path, std::uint32_t channels,
                                         std::span<const float> interleavedAudio) {
    const std::uint32_t ch = std::max(1U, channels);
    auto& slotsArr = *slots_;
    for (std::size_t i = 0; i < kMaxStreams; ++i) {
        bool expected = false;
        if (slotsArr[i].active.compare_exchange_strong(expected, false, std::memory_order_acquire)) {
            auto& slot = slotsArr[i];
            slot.recordMode.store(false, std::memory_order_relaxed);
            slot.channels = ch;
            slot.filePath = path;
            slot.BackingMemoryCache.assign(interleavedAudio.begin(), interleavedAudio.end());
            slot.totalFileSamples.store(slot.BackingMemoryCache.size(), std::memory_order_relaxed);
            slot.fileSampleCursor.store(0, std::memory_order_relaxed);
            slot.sampleRing.reset();

            const std::size_t targetSamples = static_cast<std::size_t>(
                preloadSeconds_.load(std::memory_order_relaxed) *
                sampleRate_.load(std::memory_order_relaxed) * static_cast<double>(ch));
            refillPlaybackSlot(slot, std::min(targetSamples, DiskStreamSlot::kRingSamples - ch));

            slot.active.store(true, std::memory_order_release);
            return static_cast<int>(i);
        }
    }
    return -1;
}

int DiskIOThread::registerRecordStream(const std::string& path, std::uint32_t channels) {
    const std::uint32_t ch = std::max(1U, channels);
    auto& slotsArr = *slots_;
    for (std::size_t i = 0; i < kMaxStreams; ++i) {
        if (!slotsArr[i].active.load(std::memory_order_acquire)) {
            auto& slot = slotsArr[i];
            slot.recordMode.store(true, std::memory_order_relaxed);
            slot.channels = ch;
            slot.filePath = path;
            slot.BackingMemoryCache.clear();
            slot.BackingMemoryCache.reserve(static_cast<std::size_t>(sampleRate_.load() * ch * 60.0));
            slot.totalFileSamples.store(0, std::memory_order_relaxed);
            slot.fileSampleCursor.store(0, std::memory_order_relaxed);
            slot.sampleRing.reset();
            slot.active.store(true, std::memory_order_release);
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DiskIOThread::unregisterStream(int streamId) noexcept {
    if (streamId < 0 || static_cast<std::size_t>(streamId) >= kMaxStreams) {
        return;
    }
    auto& slot = (*slots_)[static_cast<std::size_t>(streamId)];
    if (slot.recordMode.load(std::memory_order_acquire)) {
        drainRecordSlot(slot);
    }
    slot.active.store(false, std::memory_order_release);
    slot.sampleRing.reset();
}

void DiskIOThread::seekStream(int streamId, std::uint64_t framePosition) noexcept {
    if (streamId < 0 || static_cast<std::size_t>(streamId) >= kMaxStreams) {
        return;
    }
    auto& slot = (*slots_)[static_cast<std::size_t>(streamId)];
    if (!slot.active.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t sampleOffset = framePosition * static_cast<std::uint64_t>(slot.channels);
    slot.fileSampleCursor.store(sampleOffset, std::memory_order_release);
    slot.sampleRing.reset();
}

std::size_t DiskIOThread::readPlaybackFramesRT(int streamId, float* interleavedOut,
                                               std::size_t frames, std::uint32_t channels) noexcept {
    if (streamId < 0 || static_cast<std::size_t>(streamId) >= kMaxStreams || interleavedOut == nullptr) {
        return 0;
    }
    auto& slot = (*slots_)[static_cast<std::size_t>(streamId)];
    if (!slot.active.load(std::memory_order_acquire) || slot.recordMode.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::size_t totalSamples = frames * static_cast<std::size_t>(channels);
    const std::size_t popped = slot.sampleRing.popBatch(std::span<float>(interleavedOut, totalSamples));
    if (popped < totalSamples) {
        std::fill(interleavedOut + popped, interleavedOut + totalSamples, 0.0f);
    }
    return popped / std::max<std::size_t>(1, channels);
}

std::size_t DiskIOThread::writeRecordFramesRT(int streamId, const float* interleavedIn,
                                              std::size_t frames, std::uint32_t channels) noexcept {
    if (streamId < 0 || static_cast<std::size_t>(streamId) >= kMaxStreams || interleavedIn == nullptr) {
        return 0;
    }
    auto& slot = (*slots_)[static_cast<std::size_t>(streamId)];
    if (!slot.active.load(std::memory_order_acquire) || !slot.recordMode.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::size_t totalSamples = frames * static_cast<std::size_t>(channels);
    const std::size_t pushed = slot.sampleRing.pushBatch(std::span<const float>(interleavedIn, totalSamples));
    return pushed / std::max<std::size_t>(1, channels);
}

void DiskIOThread::refillPlaybackSlot(DiskStreamSlot& slot, std::size_t targetPreloadSamples) {
    const std::size_t available = slot.sampleRing.readAvailable();
    if (available >= targetPreloadSamples) {
        return;
    }
    const std::size_t writable = slot.sampleRing.writeAvailable();
    const std::size_t wantToPush = std::min(targetPreloadSamples - available, writable);
    const std::uint64_t cursor = slot.fileSampleCursor.load(std::memory_order_acquire);
    const std::uint64_t total = slot.totalFileSamples.load(std::memory_order_acquire);
    if (cursor >= total || wantToPush == 0) {
        return;
    }
    const std::size_t canRead = static_cast<std::size_t>(
        std::min<std::uint64_t>(wantToPush, total - cursor));
    const std::size_t pushed = slot.sampleRing.pushBatch(
        std::span<const float>(slot.BackingMemoryCache.data() + cursor, canRead));
    slot.fileSampleCursor.store(cursor + pushed, std::memory_order_release);
}

void DiskIOThread::drainRecordSlot(DiskStreamSlot& slot) {
    std::array<float, 4096> temp{};
    while (slot.sampleRing.readAvailable() > 0) {
        const std::size_t popped = slot.sampleRing.popBatch(std::span<float>(temp));
        if (popped == 0) {
            break;
        }
        slot.BackingMemoryCache.insert(slot.BackingMemoryCache.end(), temp.begin(), temp.begin() + popped);
        slot.totalFileSamples.store(slot.BackingMemoryCache.size(), std::memory_order_release);
    }

    if (!slot.filePath.empty() && !slot.BackingMemoryCache.empty()) {
        std::ofstream out(slot.filePath, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(slot.BackingMemoryCache.data()),
                      static_cast<std::streamsize>(slot.BackingMemoryCache.size() * sizeof(float)));
        }
    }
}

void DiskIOThread::ioWorkerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const auto t0 = std::chrono::steady_clock::now();
        const double preloadSec = preloadSeconds_.load(std::memory_order_relaxed);
        const double sr = sampleRate_.load(std::memory_order_relaxed);

        std::uint32_t activeStreams = 0;
        for (auto& slot : *slots_) {
            if (!slot.active.load(std::memory_order_acquire)) {
                continue;
            }
            ++activeStreams;
            if (slot.recordMode.load(std::memory_order_relaxed)) {
                drainRecordSlot(slot);
            } else {
                const std::size_t targetSamples = std::min<std::size_t>(
                    static_cast<std::size_t>(preloadSec * sr * static_cast<double>(slot.channels)),
                    DiskStreamSlot::kRingSamples - slot.channels);
                refillPlaybackSlot(slot, targetSamples);
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double busyMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const float load = (activeStreams == 0) ? 0.0f : static_cast<float>(std::min(100.0, (busyMs / 5.0) * 100.0));
        diskLoadPercent_.store(load, std::memory_order_relaxed);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace AlphaStudio::Core
