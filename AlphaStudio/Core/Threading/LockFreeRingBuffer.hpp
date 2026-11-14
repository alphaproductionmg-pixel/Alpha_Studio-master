#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace AlphaStudio::Core {

// Wait-free / Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
// Strictly zero heap allocation, zero mutexes, zero syscalls.
// Capacity must be a power of two for bitmask wrap-around.
template <typename T, std::size_t Capacity>
    requires(std::is_trivially_copyable_v<T> && Capacity >= 2 && ((Capacity & (Capacity - 1)) == 0))
class LockFreeRingBuffer {
public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kCacheLineSize = 64;

    constexpr LockFreeRingBuffer() noexcept = default;
    ~LockFreeRingBuffer() noexcept = default;

    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    [[nodiscard]] bool tryPush(const T& item) noexcept {
        const std::size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t currentRead = readIndex_.load(std::memory_order_acquire);
        if ((currentWrite - currentRead) >= kCapacity) {
            return false;
        }
        storage_[currentWrite & kMask] = item;
        writeIndex_.store(currentWrite + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPop(T& outItem) noexcept {
        const std::size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const std::size_t currentWrite = writeIndex_.load(std::memory_order_acquire);
        if (currentRead == currentWrite) {
            return false;
        }
        outItem = storage_[currentRead & kMask];
        readIndex_.store(currentRead + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool peek(T& outItem) const noexcept {
        const std::size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const std::size_t currentWrite = writeIndex_.load(std::memory_order_acquire);
        if (currentRead == currentWrite) {
            return false;
        }
        outItem = storage_[currentRead & kMask];
        return true;
    }

    std::size_t pushBatch(std::span<const T> items) noexcept {
        const std::size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const std::size_t currentRead = readIndex_.load(std::memory_order_acquire);
        const std::size_t freeSlots = kCapacity - (currentWrite - currentRead);
        const std::size_t toWrite = (items.size() < freeSlots) ? items.size() : freeSlots;
        if (toWrite == 0) {
            return 0;
        }

        const std::size_t firstPos = currentWrite & kMask;
        const std::size_t firstChunk = ((kCapacity - firstPos) < toWrite) ? (kCapacity - firstPos) : toWrite;
        std::memcpy(&storage_[firstPos], items.data(), firstChunk * sizeof(T));

        const std::size_t secondChunk = toWrite - firstChunk;
        if (secondChunk > 0) {
            std::memcpy(&storage_[0], items.data() + firstChunk, secondChunk * sizeof(T));
        }

        writeIndex_.store(currentWrite + toWrite, std::memory_order_release);
        return toWrite;
    }

    std::size_t popBatch(std::span<T> outItems) noexcept {
        const std::size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const std::size_t currentWrite = writeIndex_.load(std::memory_order_acquire);
        const std::size_t available = currentWrite - currentRead;
        const std::size_t toRead = (outItems.size() < available) ? outItems.size() : available;
        if (toRead == 0) {
            return 0;
        }

        const std::size_t firstPos = currentRead & kMask;
        const std::size_t firstChunk = ((kCapacity - firstPos) < toRead) ? (kCapacity - firstPos) : toRead;
        std::memcpy(outItems.data(), &storage_[firstPos], firstChunk * sizeof(T));

        const std::size_t secondChunk = toRead - firstChunk;
        if (secondChunk > 0) {
            std::memcpy(outItems.data() + firstChunk, &storage_[0], secondChunk * sizeof(T));
        }

        readIndex_.store(currentRead + toRead, std::memory_order_release);
        return toRead;
    }

    [[nodiscard]] std::size_t readAvailable() const noexcept {
        const std::size_t w = writeIndex_.load(std::memory_order_acquire);
        const std::size_t r = readIndex_.load(std::memory_order_acquire);
        return w - r;
    }

    [[nodiscard]] std::size_t writeAvailable() const noexcept {
        return kCapacity - readAvailable();
    }

    [[nodiscard]] bool empty() const noexcept {
        return readAvailable() == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return writeAvailable() == 0;
    }

    void reset() noexcept {
        readIndex_.store(0, std::memory_order_relaxed);
        writeIndex_.store(0, std::memory_order_release);
    }

private:
    alignas(kCacheLineSize) std::atomic<std::size_t> writeIndex_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> readIndex_{0};
    alignas(kCacheLineSize) std::array<T, kCapacity> storage_{};
};

} // namespace AlphaStudio::Core
