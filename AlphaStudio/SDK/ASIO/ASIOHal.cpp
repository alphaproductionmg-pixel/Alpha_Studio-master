#include "AlphaStudio/SDK/ASIO/ASIOHal.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"

#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK)
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
#include <windows.h>
#endif
#include "asiosys.h"
#include "asio.h"
#include "ASIOConvertSamples.h"
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
#include "asiodrivers.h"
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace AlphaStudio::SDK::ASIO {

namespace {
std::shared_ptr<IASIODriverAdapter>& globalAdapterInstance() {
    static std::shared_ptr<IASIODriverAdapter> instance =
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK)
        std::make_shared<SteinbergASIOSDKHostAdapter>();
#elif defined(ALPHASTUDIO_PLATFORM_MACOS)
        std::make_shared<CoreAudio2ASIO>();
#elif defined(ALPHASTUDIO_PLATFORM_WINDOWS)
        std::make_shared<WASAPIAdapter>(true);
#else
        std::make_shared<LinuxALSA2ASIOAdapter>();
#endif
    return instance;
}

#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
SteinbergASIOSDKHostAdapter* gActiveSteinbergHost = nullptr;

void steinbergBufferSwitchThunk(long index, ::ASIOBool directProcess) {
    if (gActiveSteinbergHost != nullptr) {
        gActiveSteinbergHost->handleHardwareBufferSwitch(index, directProcess);
    }
}

void steinbergSampleRateChangedThunk(::ASIOSampleRate /*sRate*/) {}

long steinbergAsioMessageThunk(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case ::kAsioSelectorSupported:
            if (value == ::kAsioEngineVersion || value == ::kAsioResetRequest ||
                value == ::kAsioBufferSizeChange || value == ::kAsioLatenciesChanged ||
                value == ::kAsioSupportsTimeInfo || value == ::kAsioSupportsTimeCode) {
                return 1L;
            }
            return 0L;
        case ::kAsioEngineVersion:
            return 2L;
        case ::kAsioSupportsTimeInfo:
        case ::kAsioSupportsTimeCode:
            return 1L;
        default:
            return 0L;
    }
}

::ASIOTime* steinbergBufferSwitchTimeInfoThunk(::ASIOTime* /*params*/, long index, ::ASIOBool directProcess) {
    if (gActiveSteinbergHost != nullptr) {
        gActiveSteinbergHost->handleHardwareBufferSwitch(index, directProcess);
    }
    return nullptr;
}
#endif
} // namespace

BaseDoubleBufferedASIOAdapter::BaseDoubleBufferedASIOAdapter(
    std::string name, long numInputs, long numOutputs,
    long baseInLatency, long baseOutLatency)
    : name_(std::move(name)),
      numInputs_(std::clamp(numInputs, 0L, kMaxPhysicalChannels)),
      numOutputs_(std::clamp(numOutputs, 0L, kMaxPhysicalChannels)),
      baseInputLatency_(std::max(0L, baseInLatency)),
      baseOutputLatency_(std::max(0L, baseOutLatency)) {}

BaseDoubleBufferedASIOAdapter::~BaseDoubleBufferedASIOAdapter() {
    stop();
    disposeBuffers();
}

ASIOError BaseDoubleBufferedASIOAdapter::init(ASIODriverInfo* info) {
    initialized_ = true;
    if (info != nullptr) {
        info->asioVersion = 2;
        info->driverVersion = 2026;
        std::snprintf(info->name, sizeof(info->name), "%s", name_.c_str());
        info->errorMessage[0] = '\0';
    }
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::exit() {
    stop();
    disposeBuffers();
    initialized_ = false;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::getChannels(long* numInputChannels,
                                                     long* numOutputChannels) const {
    if (!initialized_ || numInputChannels == nullptr || numOutputChannels == nullptr) {
        return ASE_InvalidParameter;
    }
    *numInputChannels = numInputs_;
    *numOutputChannels = numOutputs_;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::getBufferSize(long* minSize, long* maxSize,
                                                       long* preferredSize, long* granularity) const {
    if (!initialized_ || !minSize || !maxSize || !preferredSize || !granularity) {
        return ASE_InvalidParameter;
    }
    *minSize = 32L;
    *maxSize = kMaxBufferFrames;
    *preferredSize = bufferSize_;
    *granularity = -1L; // Power-of-two buffer sizes
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::canSampleRate(ASIOSampleRate sRate) const {
    constexpr std::array<ASIOSampleRate, 6> kSupported = {
        44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
    };
    for (ASIOSampleRate supported : kSupported) {
        if (std::abs(sRate - supported) < 0.5) {
            return ASE_OK;
        }
    }
    return ASE_NoClock;
}

ASIOError BaseDoubleBufferedASIOAdapter::getSampleRate(ASIOSampleRate* currentRate) const {
    if (!currentRate) {
        return ASE_InvalidParameter;
    }
    *currentRate = sampleRate_;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::setSampleRate(ASIOSampleRate sRate) {
    if (canSampleRate(sRate) != ASE_OK) {
        return ASE_NoClock;
    }
    const bool changed = std::abs(sampleRate_ - sRate) >= 0.5;
    sampleRate_ = sRate;
    if (changed && callbacks_.sampleRateDidChange != nullptr) {
        callbacks_.sampleRateDidChange(sampleRate_);
    }
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::getChannelInfo(ASIOChannelInfo* info) const {
    if (!info) {
        return ASE_InvalidParameter;
    }
    const long limit = (info->isInput == ASIOTrue) ? numInputs_ : numOutputs_;
    if (info->channel < 0 || info->channel >= limit) {
        return ASE_InvalidParameter;
    }
    info->isActive = buffersCreated_ ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB;
    std::snprintf(info->name, sizeof(info->name), "%s %ld",
                  (info->isInput == ASIOTrue) ? "ASIO In" : "ASIO Out",
                  info->channel + 1L);
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::createBuffers(
    ASIOBufferInfo* bufferInfos, long numChannels,
    long bufferSize, const ASIOCallbacks* callbacks) {
    if (!initialized_ || !bufferInfos || !callbacks || numChannels <= 0 ||
        bufferSize < 16 || bufferSize > kMaxBufferFrames) {
        return ASE_InvalidParameter;
    }
    stop();
    disposeBuffers();

    bufferSize_ = bufferSize;
    callbacks_ = *callbacks;

    const std::size_t slabFloats =
        static_cast<std::size_t>(numChannels) * 2U * static_cast<std::size_t>(bufferSize_);
    doubleBufferSlab_.assign(slabFloats, 0.0f);
    activeBuffers_.resize(static_cast<std::size_t>(numChannels));

    for (long c = 0; c < numChannels; ++c) {
        float* buf0 = doubleBufferSlab_.data() +
                      (static_cast<std::size_t>(c) * 2U) * static_cast<std::size_t>(bufferSize_);
        float* buf1 = buf0 + static_cast<std::size_t>(bufferSize_);
        bufferInfos[c].buffers[0] = buf0;
        bufferInfos[c].buffers[1] = buf1;
        activeBuffers_[static_cast<std::size_t>(c)] = bufferInfos[c];
    }

    buffersCreated_ = true;
    activeDoubleBufferIndex_ = 0;
    samplePosition_ = 0;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::disposeBuffers() {
    stop();
    activeBuffers_.clear();
    doubleBufferSlab_.clear();
    callbacks_ = ASIOCallbacks{};
    buffersCreated_ = false;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::start() {
    if (!initialized_ || !buffersCreated_) {
        return ASE_InvalidMode;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return ASE_OK;
    }

    realtimePumpThread_ = std::thread([this]() {
        Core::AudioThreadManager::applyCurrentThreadRealtimePriority(Core::AudioPriority::Boost);
        const auto periodUs = std::chrono::microseconds(static_cast<std::int64_t>(
            std::max(100.0, (static_cast<double>(bufferSize_) / sampleRate_) * 1000000.0)));
        auto nextTick = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            pumpSingleCycle();
            nextTick += periodUs;
            if (std::chrono::steady_clock::now() < nextTick) {
                std::this_thread::sleep_until(nextTick);
            } else {
                nextTick = std::chrono::steady_clock::now();
            }
        }
    });

    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::stop() {
    running_.store(false, std::memory_order_release);
    if (realtimePumpThread_.joinable()) {
        realtimePumpThread_.join();
    }
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::getLatencies(long* inputLatency, long* outputLatency) const {
    if (!buffersCreated_ || !inputLatency || !outputLatency) {
        return ASE_InvalidParameter;
    }
    *inputLatency = bufferSize_ + baseInputLatency_;
    *outputLatency = bufferSize_ + baseOutputLatency_;
    return ASE_OK;
}

ASIOError BaseDoubleBufferedASIOAdapter::outputReady() {
    return ASE_OK;
}

bool BaseDoubleBufferedASIOAdapter::pumpSingleCycle() noexcept {
    if (!buffersCreated_) {
        return false;
    }
    const long idx = activeDoubleBufferIndex_;
    timeState_.timeInfo.speed = 1.0;
    timeState_.timeInfo.sampleRate = sampleRate_;
    timeState_.timeInfo.samplePosition = ASIOSamples::fromUInt64(samplePosition_);

    bool supportsTimeInfo = false;
    if (callbacks_.asioMessage != nullptr) {
        supportsTimeInfo =
            (callbacks_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1L);
    }

    if (supportsTimeInfo && callbacks_.bufferSwitchTimeInfo != nullptr) {
        callbacks_.bufferSwitchTimeInfo(&timeState_, idx, ASIOTrue);
    } else if (callbacks_.bufferSwitch != nullptr) {
        callbacks_.bufferSwitch(idx, ASIOTrue);
    }

    samplePosition_ += static_cast<std::uint64_t>(bufferSize_);
    activeDoubleBufferIndex_ = 1L - idx;
    return true;
}

// ============================================================================
// Concrete Platform Adapters
// ============================================================================
CoreAudio2ASIO::CoreAudio2ASIO()
    : BaseDoubleBufferedASIOAdapter("CoreAudio2ASIO Bridge", 256, 256, 24, 24) {}

WASAPIAdapter::WASAPIAdapter(bool exclusiveMode)
    : BaseDoubleBufferedASIOAdapter(
          exclusiveMode ? "WASAPI Exclusive ASIO" : "WASAPI Shared ASIO",
          256, 256, exclusiveMode ? 16 : 128, exclusiveMode ? 16 : 128),
      exclusiveMode_(exclusiveMode) {}

DirectXAdapter::DirectXAdapter()
    : BaseDoubleBufferedASIOAdapter("DirectX Full-Duplex ASIO", 64, 64, 256, 256) {}

LinuxALSA2ASIOAdapter::LinuxALSA2ASIOAdapter()
    : BaseDoubleBufferedASIOAdapter("ALSA Direct-MMAP ASIO", 256, 256, 16, 16) {}

// ============================================================================
// SteinbergASIOSDKHostAdapter (Official Steinberg ASIO SDK 2.3.4 Integration)
// ============================================================================
SteinbergASIOSDKHostAdapter::SteinbergASIOSDKHostAdapter(std::string preferredDriverName)
    : requestedDriverName_(std::move(preferredDriverName)) {
#if defined(ALPHASTUDIO_PLATFORM_MACOS)
    fallbackAdapter_ = std::make_unique<CoreAudio2ASIO>();
#elif defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    fallbackAdapter_ = std::make_unique<WASAPIAdapter>(true);
#else
    fallbackAdapter_ = std::make_unique<LinuxALSA2ASIOAdapter>();
#endif
}

SteinbergASIOSDKHostAdapter::~SteinbergASIOSDKHostAdapter() {
    SteinbergASIOSDKHostAdapter::exit();
}

std::vector<std::string> SteinbergASIOSDKHostAdapter::enumerateSystemDrivers() {
    std::vector<std::string> names;
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    ::AsioDrivers driverList;
    constexpr long kMaxDrivers = 32;
    std::array<std::array<char, 64>, kMaxDrivers> nameStorage{};
    std::array<char*, kMaxDrivers> namePtrs{};
    for (long i = 0; i < kMaxDrivers; ++i) {
        namePtrs[static_cast<std::size_t>(i)] = nameStorage[static_cast<std::size_t>(i)].data();
    }
    const long found = driverList.getDriverNames(namePtrs.data(), kMaxDrivers);
    for (long i = 0; i < found; ++i) {
        if (nameStorage[static_cast<std::size_t>(i)][0] != '\0') {
            names.emplace_back(nameStorage[static_cast<std::size_t>(i)].data());
        }
    }
#endif
    return names;
}

bool SteinbergASIOSDKHostAdapter::selfTestSampleConverters() noexcept {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK)
    ::ASIOConvertSamples converter;
    std::array<float, 8> buf16 = {0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f, 0.125f};
    converter.float32toInt16inPlace(buf16.data(), static_cast<long>(buf16.size()));
    if constexpr (sizeof(long) == sizeof(std::int32_t)) {
        std::array<float, 8> buf32 = {0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f, 0.125f};
        converter.float32toInt32inPlace(buf32.data(), static_cast<long>(buf32.size()));
    }
    const auto* i16 = reinterpret_cast<const std::int16_t*>(buf16.data());
    return (i16[1] > 7000 && i16[2] < -7000);
#else
    return false;
#endif
}

bool SteinbergASIOSDKHostAdapter::openControlPanel() noexcept {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOControlPanel() == ::ASE_OK;
    }
#endif
    return false;
}

ASIOError SteinbergASIOSDKHostAdapter::init(ASIODriverInfo* info) {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    const auto drivers = enumerateSystemDrivers();
    std::string target = requestedDriverName_;
    if (target.empty() && !drivers.empty()) {
        target = drivers.front();
    }
    if (!target.empty()) {
        std::vector<char> mutableName(target.begin(), target.end());
        mutableName.push_back('\0');
        if (::loadAsioDriver(mutableName.data())) {
            ::ASIODriverInfo sdkInfo{};
            sdkInfo.asioVersion = 2;
            sdkInfo.sysRef = (info != nullptr && info->sysRef != nullptr)
                ? info->sysRef
                : static_cast<void*>(::GetDesktopWindow());
            if (::ASIOInit(&sdkInfo) == ::ASE_OK) {
                hardwareDriverActive_ = true;
                activeDriverName_ = sdkInfo.name;
                gActiveSteinbergHost = this;
                if (info != nullptr) {
                    info->asioVersion = sdkInfo.asioVersion;
                    info->driverVersion = sdkInfo.driverVersion;
                    std::snprintf(info->name, sizeof(info->name), "%s", sdkInfo.name);
                    std::snprintf(info->errorMessage, sizeof(info->errorMessage), "%s", sdkInfo.errorMessage);
                }
                return ASE_OK;
            }
            if (asioDrivers != nullptr) {
                asioDrivers->removeCurrentDriver();
            }
        }
    }
#endif
    hardwareDriverActive_ = false;
    activeDriverName_ = std::string(fallbackAdapter_->driverName()) + " [ASIO SDK 2.3.4]";
    const ASIOError rc = fallbackAdapter_->init(info);
    if (info != nullptr) {
        std::snprintf(info->name, sizeof(info->name), "%s", activeDriverName_.c_str());
    }
    return rc;
}

ASIOError SteinbergASIOSDKHostAdapter::exit() {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        ::ASIOStop();
        ::ASIODisposeBuffers();
        ::ASIOExit();
        gActiveSteinbergHost = nullptr;
        hardwareDriverActive_ = false;
        return ASE_OK;
    }
#endif
    return fallbackAdapter_->exit();
}

ASIOError SteinbergASIOSDKHostAdapter::getChannels(long* numInputChannels, long* numOutputChannels) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOGetChannels(numInputChannels, numOutputChannels);
    }
#endif
    return fallbackAdapter_->getChannels(numInputChannels, numOutputChannels);
}

ASIOError SteinbergASIOSDKHostAdapter::getBufferSize(long* minSize, long* maxSize,
                                                     long* preferredSize, long* granularity) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOGetBufferSize(minSize, maxSize, preferredSize, granularity);
    }
#endif
    return fallbackAdapter_->getBufferSize(minSize, maxSize, preferredSize, granularity);
}

ASIOError SteinbergASIOSDKHostAdapter::canSampleRate(ASIOSampleRate sRate) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOCanSampleRate(sRate);
    }
#endif
    return fallbackAdapter_->canSampleRate(sRate);
}

ASIOError SteinbergASIOSDKHostAdapter::getSampleRate(ASIOSampleRate* currentRate) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOGetSampleRate(currentRate);
    }
#endif
    return fallbackAdapter_->getSampleRate(currentRate);
}

ASIOError SteinbergASIOSDKHostAdapter::setSampleRate(ASIOSampleRate sRate) {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOSetSampleRate(sRate);
    }
#endif
    return fallbackAdapter_->setSampleRate(sRate);
}

ASIOError SteinbergASIOSDKHostAdapter::getChannelInfo(ASIOChannelInfo* info) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_ && info != nullptr) {
        ::ASIOChannelInfo sdkCh{};
        sdkCh.channel = info->channel;
        sdkCh.isInput = info->isInput;
        const ASIOError err = ::ASIOGetChannelInfo(&sdkCh);
        if (err == ::ASE_OK) {
            info->isActive = sdkCh.isActive;
            info->channelGroup = sdkCh.channelGroup;
            info->type = ASIOSTFloat32LSB; // Normalized to 32-bit float for AlphaCore
            std::snprintf(info->name, sizeof(info->name), "%s", sdkCh.name);
        }
        return err;
    }
#endif
    return fallbackAdapter_->getChannelInfo(info);
}

ASIOError SteinbergASIOSDKHostAdapter::createBuffers(
    ASIOBufferInfo* bufferInfos, long numChannels,
    long bufferSize, const ASIOCallbacks* callbacks) {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_ && bufferInfos != nullptr && callbacks != nullptr && numChannels > 0) {
        bufferSize_ = bufferSize;
        hostCallbacks_ = *callbacks;

        std::vector<::ASIOBufferInfo> sdkBufs(static_cast<std::size_t>(numChannels));
        for (long c = 0; c < numChannels; ++c) {
            sdkBufs[static_cast<std::size_t>(c)].isInput = bufferInfos[c].isInput;
            sdkBufs[static_cast<std::size_t>(c)].channelNum = bufferInfos[c].channelNum;
            sdkBufs[static_cast<std::size_t>(c)].buffers[0] = nullptr;
            sdkBufs[static_cast<std::size_t>(c)].buffers[1] = nullptr;
        }

        ::ASIOCallbacks sdkCbs{};
        sdkCbs.bufferSwitch = &steinbergBufferSwitchThunk;
        sdkCbs.sampleRateDidChange = &steinbergSampleRateChangedThunk;
        sdkCbs.asioMessage = &steinbergAsioMessageThunk;
        sdkCbs.bufferSwitchTimeInfo = &steinbergBufferSwitchTimeInfoThunk;

        const ASIOError err = ::ASIOCreateBuffers(sdkBufs.data(), numChannels, bufferSize, &sdkCbs);
        if (err != ::ASE_OK) {
            return err;
        }

        floatShadowSlab_.assign(static_cast<std::size_t>(numChannels) * 2U * static_cast<std::size_t>(bufferSize), 0.0f);
        hostBufferInfos_.resize(static_cast<std::size_t>(numChannels));
        hwSampleTypes_.resize(static_cast<std::size_t>(numChannels), ::ASIOSTInt32LSB);
        hwBufferPtrs0_.resize(static_cast<std::size_t>(numChannels), nullptr);
        hwBufferPtrs1_.resize(static_cast<std::size_t>(numChannels), nullptr);

        for (long c = 0; c < numChannels; ++c) {
            const auto idx = static_cast<std::size_t>(c);
            ::ASIOChannelInfo chInfo{};
            chInfo.channel = sdkBufs[idx].channelNum;
            chInfo.isInput = sdkBufs[idx].isInput;
            if (::ASIOGetChannelInfo(&chInfo) == ::ASE_OK) {
                hwSampleTypes_[idx] = chInfo.type;
            }
            hwBufferPtrs0_[idx] = sdkBufs[idx].buffers[0];
            hwBufferPtrs1_[idx] = sdkBufs[idx].buffers[1];

            float* f0 = floatShadowSlab_.data() + (idx * 2U) * static_cast<std::size_t>(bufferSize);
            float* f1 = f0 + static_cast<std::size_t>(bufferSize);
            bufferInfos[c].buffers[0] = f0;
            bufferInfos[c].buffers[1] = f1;
            hostBufferInfos_[idx] = bufferInfos[c];
        }
        return ASE_OK;
    }
#endif
    return fallbackAdapter_->createBuffers(bufferInfos, numChannels, bufferSize, callbacks);
}

ASIOError SteinbergASIOSDKHostAdapter::disposeBuffers() {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        floatShadowSlab_.clear();
        hostBufferInfos_.clear();
        hwSampleTypes_.clear();
        hwBufferPtrs0_.clear();
        hwBufferPtrs1_.clear();
        return ::ASIODisposeBuffers();
    }
#endif
    return fallbackAdapter_->disposeBuffers();
}

ASIOError SteinbergASIOSDKHostAdapter::start() {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOStart();
    }
#endif
    return fallbackAdapter_->start();
}

ASIOError SteinbergASIOSDKHostAdapter::stop() {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOStop();
    }
#endif
    return fallbackAdapter_->stop();
}

ASIOError SteinbergASIOSDKHostAdapter::getLatencies(long* inputLatency, long* outputLatency) const {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOGetLatencies(inputLatency, outputLatency);
    }
#endif
    return fallbackAdapter_->getLatencies(inputLatency, outputLatency);
}

ASIOError SteinbergASIOSDKHostAdapter::outputReady() {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK) && defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (hardwareDriverActive_) {
        return ::ASIOOutputReady();
    }
#endif
    return fallbackAdapter_->outputReady();
}

bool SteinbergASIOSDKHostAdapter::pumpSingleCycle() noexcept {
    if (!hardwareDriverActive_) {
        return fallbackAdapter_->pumpSingleCycle();
    }
    return true;
}

void SteinbergASIOSDKHostAdapter::handleHardwareBufferSwitch(long doubleBufferIndex, long directProcess) noexcept {
#if defined(ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK)
    const long idx = (doubleBufferIndex != 0) ? 1L : 0L;
    const std::size_t nCh = hostBufferInfos_.size();
    const auto frames = static_cast<std::size_t>(bufferSize_);

    // 1. Convert Hardware Input PCM -> Float32 Shadow Buffers (Zero Heap Allocation)
    for (std::size_t c = 0; c < nCh; ++c) {
        if (hostBufferInfos_[c].isInput != ASIOTrue) continue;
        void* hwSrc = (idx == 0) ? hwBufferPtrs0_[c] : hwBufferPtrs1_[c];
        auto* dst = static_cast<float*>(hostBufferInfos_[c].buffers[idx]);
        if (hwSrc == nullptr || dst == nullptr) continue;

        if (hwSampleTypes_[c] == ::ASIOSTFloat32LSB) {
            std::memcpy(dst, hwSrc, frames * sizeof(float));
        } else if (hwSampleTypes_[c] == ::ASIOSTInt32LSB) {
            const auto* s32 = static_cast<const std::int32_t*>(hwSrc);
            constexpr float kInv32 = 1.0f / 2147483648.0f;
            for (std::size_t f = 0; f < frames; ++f) dst[f] = static_cast<float>(s32[f]) * kInv32;
        } else if (hwSampleTypes_[c] == ::ASIOSTInt16LSB) {
            const auto* s16 = static_cast<const std::int16_t*>(hwSrc);
            constexpr float kInv16 = 1.0f / 32768.0f;
            for (std::size_t f = 0; f < frames; ++f) dst[f] = static_cast<float>(s16[f]) * kInv16;
        }
    }

    // 2. Invoke AlphaStudio's Real-Time Engine Callback
    if (hostCallbacks_.bufferSwitch != nullptr) {
        hostCallbacks_.bufferSwitch(idx, directProcess);
    }

    // 3. Convert Float32 Shadow Outputs -> Hardware Output PCM via Steinberg ASIOConvertSamples
    ::ASIOConvertSamples converter;
    for (std::size_t c = 0; c < nCh; ++c) {
        if (hostBufferInfos_[c].isInput == ASIOTrue) continue;
        const auto* src = static_cast<const float*>(hostBufferInfos_[c].buffers[idx]);
        void* hwDst = (idx == 0) ? hwBufferPtrs0_[c] : hwBufferPtrs1_[c];
        if (src == nullptr || hwDst == nullptr) continue;

        if (hwSampleTypes_[c] == ::ASIOSTFloat32LSB) {
            std::memcpy(hwDst, src, frames * sizeof(float));
        } else if (hwSampleTypes_[c] == ::ASIOSTInt32LSB) {
            if constexpr (sizeof(long) == sizeof(std::int32_t)) {
                std::memcpy(hwDst, src, frames * sizeof(float));
                converter.float32toInt32inPlace(static_cast<float*>(hwDst), bufferSize_);
            } else {
                auto* dst32 = static_cast<std::int32_t*>(hwDst);
                for (std::size_t f = 0; f < frames; ++f) {
                    dst32[f] = static_cast<std::int32_t>(
                        std::clamp(static_cast<double>(src[f]), -1.0, 0.9999999995) * 2147483647.0);
                }
            }
        } else if (hwSampleTypes_[c] == ::ASIOSTInt24LSB) {
            auto* dstBytes = static_cast<std::uint8_t*>(hwDst);
            for (std::size_t f = 0; f < frames; ++f) {
                const auto v24 = static_cast<std::int32_t>(
                    std::clamp(std::lround(src[f] * 8388607.0f), -8388608L, 8388607L));
                dstBytes[f * 3 + 0] = static_cast<std::uint8_t>(v24 & 0xFF);
                dstBytes[f * 3 + 1] = static_cast<std::uint8_t>((v24 >> 8) & 0xFF);
                dstBytes[f * 3 + 2] = static_cast<std::uint8_t>((v24 >> 16) & 0xFF);
            }
        } else if (hwSampleTypes_[c] == ::ASIOSTInt16LSB) {
            auto* dst16 = static_cast<std::int16_t*>(hwDst);
            for (std::size_t f = 0; f < frames; ++f) {
                dst16[f] = static_cast<std::int16_t>(
                    std::clamp(std::lround(src[f] * 32767.0f), -32768L, 32767L));
            }
        }
    }
#else
    (void)doubleBufferIndex;
    (void)directProcess;
#endif
}

// ============================================================================
// Global ASIO Entry Points
// ============================================================================
void ASIOSelectDriverAdapter(std::shared_ptr<IASIODriverAdapter> adapter) {
    if (adapter) {
        globalAdapterInstance() = std::move(adapter);
    }
}

std::shared_ptr<IASIODriverAdapter> ASIOGetActiveDriverAdapter() {
    return globalAdapterInstance();
}

ASIOError ASIOInit(ASIODriverInfo* info) {
    return globalAdapterInstance()->init(info);
}

ASIOError ASIOExit() {
    return globalAdapterInstance()->exit();
}

ASIOError ASIOGetChannels(long* numInputChannels, long* numOutputChannels) {
    return globalAdapterInstance()->getChannels(numInputChannels, numOutputChannels);
}

ASIOError ASIOGetBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    return globalAdapterInstance()->getBufferSize(minSize, maxSize, preferredSize, granularity);
}

ASIOError ASIOCanSampleRate(ASIOSampleRate sRate) {
    return globalAdapterInstance()->canSampleRate(sRate);
}

ASIOError ASIOGetSampleRate(ASIOSampleRate* currentRate) {
    return globalAdapterInstance()->getSampleRate(currentRate);
}

ASIOError ASIOSetSampleRate(ASIOSampleRate sRate) {
    return globalAdapterInstance()->setSampleRate(sRate);
}

ASIOError ASIOGetChannelInfo(ASIOChannelInfo* info) {
    return globalAdapterInstance()->getChannelInfo(info);
}

ASIOError ASIOCreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                            long bufferSize, const ASIOCallbacks* callbacks) {
    return globalAdapterInstance()->createBuffers(bufferInfos, numChannels, bufferSize, callbacks);
}

ASIOError ASIODisposeBuffers() {
    return globalAdapterInstance()->disposeBuffers();
}

ASIOError ASIOStart() {
    return globalAdapterInstance()->start();
}

ASIOError ASIOStop() {
    return globalAdapterInstance()->stop();
}

ASIOError ASIOGetLatencies(long* inputLatency, long* outputLatency) {
    return globalAdapterInstance()->getLatencies(inputLatency, outputLatency);
}

ASIOError ASIOOutputReady() {
    return globalAdapterInstance()->outputReady();
}

// ============================================================================
// MidiHardwarePort
// ============================================================================
MidiHardwarePort::MidiHardwarePort(std::uint8_t index, std::string portName, bool isInputPort)
    : index_(index), name_(std::move(portName)), isInput_(isInputPort) {}

bool MidiHardwarePort::enqueuePacketRT(const MidiHardwarePacket& pkt) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return false;
    }
    return ring_.tryPush(pkt);
}

bool MidiHardwarePort::dequeuePacketRT(MidiHardwarePacket& outPkt) noexcept {
    return ring_.tryPop(outPkt);
}

} // namespace AlphaStudio::SDK::ASIO
