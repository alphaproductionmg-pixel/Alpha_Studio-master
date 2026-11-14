#pragma once

#include "AlphaStudio/Core/Threading/LockFreeRingBuffer.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace AlphaStudio::SDK::ASIO {

using ASIOSampleRate = double;
using ASIOBool = long;
using ASIOError = long;

inline constexpr ASIOBool ASIOFalse = 0L;
inline constexpr ASIOBool ASIOTrue  = 1L;

inline constexpr ASIOError ASE_OK               = 0L;
inline constexpr ASIOError ASE_SUCCESS          = 0x3f4847a0L;
inline constexpr ASIOError ASE_NotPresent       = -1000L;
inline constexpr ASIOError ASE_HWMalfunction    = -999L;
inline constexpr ASIOError ASE_InvalidParameter = -998L;
inline constexpr ASIOError ASE_InvalidMode      = -997L;
inline constexpr ASIOError ASE_SPNotAdvancing   = -996L;
inline constexpr ASIOError ASE_NoClock          = -995L;
inline constexpr ASIOError ASE_NoMemory         = -994L;

enum ASIOSampleType : long {
    ASIOSTInt16LSB   = 16L,
    ASIOSTInt24LSB   = 17L,
    ASIOSTInt32LSB   = 18L,
    ASIOSTFloat32LSB = 19L,
    ASIOSTFloat64LSB = 20L
};

enum ASIOMessageSelector : long {
    kAsioSelectorSupported    = 1L,
    kAsioEngineVersion        = 2L,
    kAsioResetRequest         = 3L,
    kAsioBufferSizeChange     = 4L,
    kAsioResyncRequest        = 5L,
    kAsioLatenciesChanged     = 6L,
    kAsioSupportsTimeInfo     = 7L,
    kAsioSupportsTimeCode     = 8L,
    kAsioSupportsInputMonitor = 10L
};

struct ASIOSamples {
    std::uint32_t hi{0};
    std::uint32_t lo{0};

    [[nodiscard]] std::uint64_t toUInt64() const noexcept {
        return (static_cast<std::uint64_t>(hi) << 32U) | static_cast<std::uint64_t>(lo);
    }
    static ASIOSamples fromUInt64(std::uint64_t v) noexcept {
        return ASIOSamples{static_cast<std::uint32_t>(v >> 32U),
                           static_cast<std::uint32_t>(v & 0xFFFFFFFFULL)};
    }
};

struct ASIOTimeStamp {
    std::uint32_t hi{0};
    std::uint32_t lo{0};
};

struct ASIOTimeInfo {
    double speed{1.0};
    ASIOTimeStamp systemTime{};
    ASIOSamples samplePosition{};
    ASIOSampleRate sampleRate{48000.0};
    unsigned long flags{0};
    char reserved[12]{};
};

struct ASIOTimeCode {
    double speed{1.0};
    ASIOSamples timeCodeSamples{};
    unsigned long flags{0};
    char future[64]{};
};

struct ASIOTime {
    long reserved[4]{};
    ASIOTimeInfo timeInfo{};
    ASIOTimeCode timeCode{};
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, long directProcess){nullptr};
    void (*sampleRateDidChange)(ASIOSampleRate sRate){nullptr};
    long (*asioMessage)(long selector, long value, void* message, double* opt){nullptr};
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, long directProcess){nullptr};
};

struct ASIODriverInfo {
    long asioVersion{2};
    long driverVersion{1};
    char name[32]{"AlphaStudio ASIO HAL"};
    char errorMessage[124]{};
    void* sysRef{nullptr};
};

struct ASIOBufferInfo {
    ASIOBool isInput{ASIOFalse};
    long channelNum{0};
    void* buffers[2]{nullptr, nullptr}; // Hardware Double Buffer [0] and [1]
};

struct ASIOChannelInfo {
    long channel{0};
    ASIOBool isInput{ASIOFalse};
    ASIOBool isActive{ASIOFalse};
    long channelGroup{0};
    ASIOSampleType type{ASIOSTFloat32LSB};
    char name[32]{};
};

// ============================================================================
// Abstract ASIO Driver Adapter & Clean-Room Platform Bridges
// ============================================================================
class IASIODriverAdapter {
public:
    virtual ~IASIODriverAdapter() = default;

    [[nodiscard]] virtual const char* driverName() const noexcept = 0;
    virtual ASIOError init(ASIODriverInfo* info) = 0;
    virtual ASIOError exit() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) const = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) const = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sRate) const = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* currentRate) const = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sRate) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) const = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                    long bufferSize, const ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) const = 0;
    virtual ASIOError outputReady() = 0;

    // Deterministic single-step callback pump for offline bounce & deterministic unit verification
    virtual bool pumpSingleCycle() noexcept = 0;
};

class BaseDoubleBufferedASIOAdapter : public IASIODriverAdapter {
public:
    static constexpr long kMaxPhysicalChannels = 256;
    static constexpr long kMaxBufferFrames = 4096;

    BaseDoubleBufferedASIOAdapter(std::string name, long numInputs, long numOutputs,
                                  long baseInLatency, long baseOutLatency);
    ~BaseDoubleBufferedASIOAdapter() override;

    [[nodiscard]] const char* driverName() const noexcept override { return name_.c_str(); }
    ASIOError init(ASIODriverInfo* info) override;
    ASIOError exit() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) const override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) const override;
    ASIOError canSampleRate(ASIOSampleRate sRate) const override;
    ASIOError getSampleRate(ASIOSampleRate* currentRate) const override;
    ASIOError setSampleRate(ASIOSampleRate sRate) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) const override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                            long bufferSize, const ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) const override;
    ASIOError outputReady() override;

    bool pumpSingleCycle() noexcept override;

protected:
    std::string name_;
    long numInputs_{32};
    long numOutputs_{32};
    long baseInputLatency_{32};
    long baseOutputLatency_{32};
    ASIOSampleRate sampleRate_{48000.0};
    long bufferSize_{256};
    bool initialized_{false};
    bool buffersCreated_{false};
    std::atomic<bool> running_{false};
    ASIOCallbacks callbacks_{};
    ASIOTime timeState_{};
    long activeDoubleBufferIndex_{0};
    std::uint64_t samplePosition_{0};

    // Contiguous pre-allocated slab: [numAllocatedChannels][2][bufferSize_]
    std::vector<float> doubleBufferSlab_;
    std::vector<ASIOBufferInfo> activeBuffers_;
    std::thread realtimePumpThread_;
};

// Concrete Clean-Room Platform Adapters
class CoreAudio2ASIO final : public BaseDoubleBufferedASIOAdapter {
public:
    CoreAudio2ASIO();
};

class WASAPIAdapter final : public BaseDoubleBufferedASIOAdapter {
public:
    explicit WASAPIAdapter(bool exclusiveMode = true);
    [[nodiscard]] bool isExclusiveMode() const noexcept { return exclusiveMode_; }
private:
    bool exclusiveMode_{true};
};

class DirectXAdapter final : public BaseDoubleBufferedASIOAdapter {
public:
    DirectXAdapter();
};

class LinuxALSA2ASIOAdapter final : public BaseDoubleBufferedASIOAdapter {
public:
    LinuxALSA2ASIOAdapter();
};

// ============================================================================
// Official Steinberg ASIO SDK 2.3.4 (2025-10-15) Hardware Host Adapter
// - Uses official Steinberg ASIO SDK headers/sources (ThirdParty/ASIOSDK):
//   * Windows: Enumerates HKLM\SOFTWARE\ASIO via AsioDrivers, loads COM IASIO
//     drivers (loadAsioDriver / ASIOInit / ASIOCreateBuffers / ASIOStart /
//     ASIOControlPanel), with seamless fallback to WASAPI Exclusive when no
//     physical ASIO hardware interface is attached (e.g. headless CI).
//   * Cross-platform: Uses Steinberg's official ASIOConvertSamples engine for
//     zero-allocation Int16 / Int24 / Int32 <-> Float32 hardware buffer conversion.
// ============================================================================
class SteinbergASIOSDKHostAdapter final : public IASIODriverAdapter {
public:
    explicit SteinbergASIOSDKHostAdapter(std::string preferredDriverName = "");
    ~SteinbergASIOSDKHostAdapter() override;

    [[nodiscard]] static const char* sdkVersion() noexcept {
        return "Steinberg ASIO SDK 2.3.4 (2025-10-15)";
    }
    [[nodiscard]] static std::vector<std::string> enumerateSystemDrivers();
    [[nodiscard]] static bool selfTestSampleConverters() noexcept;

    [[nodiscard]] bool isUsingHardwareCOMDriver() const noexcept { return hardwareDriverActive_; }
    bool openControlPanel() noexcept;

    [[nodiscard]] const char* driverName() const noexcept override { return activeDriverName_.c_str(); }
    ASIOError init(ASIODriverInfo* info) override;
    ASIOError exit() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) const override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) const override;
    ASIOError canSampleRate(ASIOSampleRate sRate) const override;
    ASIOError getSampleRate(ASIOSampleRate* currentRate) const override;
    ASIOError setSampleRate(ASIOSampleRate sRate) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) const override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                            long bufferSize, const ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) const override;
    ASIOError outputReady() override;
    bool pumpSingleCycle() noexcept override;

    // Real-time trampoline entry points invoked by official ::ASIOCallbacks
    void handleHardwareBufferSwitch(long doubleBufferIndex, long directProcess) noexcept;

private:
    std::string requestedDriverName_;
    std::string activeDriverName_{"Steinberg ASIO SDK 2.3.4 Host"};
    bool hardwareDriverActive_{false};
    long bufferSize_{256};
    ASIOCallbacks hostCallbacks_{};
    std::vector<float> floatShadowSlab_;
    std::vector<ASIOBufferInfo> hostBufferInfos_;
    std::vector<long> hwSampleTypes_;
    std::vector<void*> hwBufferPtrs0_;
    std::vector<void*> hwBufferPtrs1_;
    std::unique_ptr<BaseDoubleBufferedASIOAdapter> fallbackAdapter_;
};

// ============================================================================
// Global ASIO C-Compatible HAL Entry Points
// ============================================================================
void ASIOSelectDriverAdapter(std::shared_ptr<IASIODriverAdapter> adapter);
std::shared_ptr<IASIODriverAdapter> ASIOGetActiveDriverAdapter();

ASIOError ASIOInit(ASIODriverInfo* info);
ASIOError ASIOExit();
ASIOError ASIOGetChannels(long* numInputChannels, long* numOutputChannels);
ASIOError ASIOGetBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity);
ASIOError ASIOCanSampleRate(ASIOSampleRate sRate);
ASIOError ASIOGetSampleRate(ASIOSampleRate* currentRate);
ASIOError ASIOSetSampleRate(ASIOSampleRate sRate);
ASIOError ASIOGetChannelInfo(ASIOChannelInfo* info);
ASIOError ASIOCreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                            long bufferSize, const ASIOCallbacks* callbacks);
ASIOError ASIODisposeBuffers();
ASIOError ASIOStart();
ASIOError ASIOStop();
ASIOError ASIOGetLatencies(long* inputLatency, long* outputLatency);
ASIOError ASIOOutputReady();

// ============================================================================
// Hardware MIDI Port Abstraction (Sample-Accurate Timestamped Lock-Free I/O)
// ============================================================================
struct MidiHardwarePacket {
    std::uint64_t timestampSamples{0};
    std::int32_t sampleOffsetInBlock{0};
    std::uint8_t status{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::uint8_t portIndex{0};
};

class MidiHardwarePort {
public:
    MidiHardwarePort(std::uint8_t index, std::string portName, bool isInputPort);

    [[nodiscard]] std::uint8_t index() const noexcept { return index_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool isInput() const noexcept { return isInput_; }
    void setActive(bool active) noexcept { active_.store(active, std::memory_order_release); }
    [[nodiscard]] bool isActive() const noexcept { return active_.load(std::memory_order_acquire); }

    bool enqueuePacketRT(const MidiHardwarePacket& pkt) noexcept;
    bool dequeuePacketRT(MidiHardwarePacket& outPkt) noexcept;

private:
    std::uint8_t index_{0};
    std::string name_;
    bool isInput_{true};
    std::atomic<bool> active_{true};
    Core::LockFreeRingBuffer<MidiHardwarePacket, 4096> ring_;
};

} // namespace AlphaStudio::SDK::ASIO
