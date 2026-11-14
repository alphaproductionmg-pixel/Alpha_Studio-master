#include "AlphaStudio/Core/AudioSystem/VSTAudioSystem.hpp"

#include <algorithm>
#include <cstring>

namespace AlphaStudio::Core {

// ============================================================================
// DelayCompensationLine
// ============================================================================
DelayCompensationLine::DelayCompensationLine() {
    prepare(2);
}

void DelayCompensationLine::prepare(std::uint32_t channels) {
    channels_ = std::clamp<std::uint32_t>(channels, 1U, static_cast<std::uint32_t>(kMaxChannels));
    for (std::uint32_t c = 0; c < channels_; ++c) {
        ringStorage_[c].assign(kMaxCompensationSamples, 0.0f);
    }
    writePos_ = 0;
}

void DelayCompensationLine::setDelaySamples(std::uint32_t delaySamples) noexcept {
    delaySamples_ = std::min<std::uint32_t>(
        delaySamples, static_cast<std::uint32_t>(kMaxCompensationSamples - 1));
}

void DelayCompensationLine::reset() noexcept {
    for (std::uint32_t c = 0; c < channels_; ++c) {
        std::fill(ringStorage_[c].begin(), ringStorage_[c].end(), 0.0f);
    }
    writePos_ = 0;
}

void DelayCompensationLine::processInPlaceRT(float* const* channelBuffers,
                                             std::uint32_t numChannels,
                                             std::uint32_t numFrames) noexcept {
    if (delaySamples_ == 0 || channelBuffers == nullptr || numFrames == 0) {
        return;
    }
    const std::uint32_t chCount = std::min(numChannels, channels_);
    std::size_t wp = writePos_;

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        const std::size_t rp = (wp + kMaxCompensationSamples - delaySamples_) & kMask;
        for (std::uint32_t c = 0; c < chCount; ++c) {
            if (channelBuffers[c] != nullptr) {
                const float inSample = channelBuffers[c][f];
                const float delayedSample = ringStorage_[c][rp];
                ringStorage_[c][wp] = inSample;
                channelBuffers[c][f] = delayedSample;
            }
        }
        wp = (wp + 1U) & kMask;
    }
    writePos_ = wp;
}

// ============================================================================
// VSTAudioSystem
// ============================================================================
VSTAudioSystem* VSTAudioSystem::activeSystemInstance_ = nullptr;

VSTAudioSystem::VSTAudioSystem() = default;

VSTAudioSystem::~VSTAudioSystem() {
    shutdown();
}

bool VSTAudioSystem::initialize(std::shared_ptr<SDK::ASIO::IASIODriverAdapter> driver,
                                double sampleRate, std::uint32_t blockSize,
                                long activeInputs, long activeOutputs) {
    shutdown();
    if (!driver) {
        driver = SDK::ASIO::ASIOGetActiveDriverAdapter();
    }
    driver_ = std::move(driver);
    SDK::ASIO::ASIOSelectDriverAdapter(driver_);

    SDK::ASIO::ASIODriverInfo drvInfo{};
    if (SDK::ASIO::ASIOInit(&drvInfo) != SDK::ASIO::ASE_OK) {
        return false;
    }

    long hwIn = 0;
    long hwOut = 0;
    SDK::ASIO::ASIOGetChannels(&hwIn, &hwOut);
    activeInputs_ = std::clamp(activeInputs, 0L, hwIn);
    activeOutputs_ = std::clamp(activeOutputs, 2L, std::max(2L, hwOut));

    sampleRate_ = sampleRate;
    SDK::ASIO::ASIOSetSampleRate(sampleRate_);
    blockSize_ = std::clamp<std::uint32_t>(blockSize, 32U, 4096U);

    const long totalCh = activeInputs_ + activeOutputs_;
    asioBuffers_.resize(static_cast<std::size_t>(totalCh));
    for (long i = 0; i < activeInputs_; ++i) {
        asioBuffers_[static_cast<std::size_t>(i)].isInput = SDK::ASIO::ASIOTrue;
        asioBuffers_[static_cast<std::size_t>(i)].channelNum = i;
    }
    for (long o = 0; o < activeOutputs_; ++o) {
        const std::size_t idx = static_cast<std::size_t>(activeInputs_ + o);
        asioBuffers_[idx].isInput = SDK::ASIO::ASIOFalse;
        asioBuffers_[idx].channelNum = o;
    }

    inputChannelPtrs_.resize(static_cast<std::size_t>(activeInputs_), nullptr);
    outputChannelPtrs_.resize(static_cast<std::size_t>(activeOutputs_), nullptr);

    activeSystemInstance_ = this;
    asioCallbacks_.bufferSwitch = &VSTAudioSystem::onAsioBufferSwitch;
    asioCallbacks_.sampleRateDidChange = &VSTAudioSystem::onAsioSampleRateChanged;
    asioCallbacks_.asioMessage = &VSTAudioSystem::onAsioMessage;
    asioCallbacks_.bufferSwitchTimeInfo = &VSTAudioSystem::onAsioBufferSwitchTimeInfo;

    if (SDK::ASIO::ASIOCreateBuffers(asioBuffers_.data(), totalCh,
                                     static_cast<long>(blockSize_),
                                     &asioCallbacks_) != SDK::ASIO::ASE_OK) {
        return false;
    }

    refreshHardwareLatencies();
    initialized_ = true;
    return true;
}

void VSTAudioSystem::shutdown() {
    stopAudio();
    if (initialized_) {
        SDK::ASIO::ASIODisposeBuffers();
        SDK::ASIO::ASIOExit();
        initialized_ = false;
    }
    if (activeSystemInstance_ == this) {
        activeSystemInstance_ = nullptr;
    }
}

bool VSTAudioSystem::setSampleRate(double sampleRate) {
    if (!initialized_) {
        sampleRate_ = sampleRate;
        return true;
    }
    if (SDK::ASIO::ASIOSetSampleRate(sampleRate) != SDK::ASIO::ASE_OK) {
        return false;
    }
    sampleRate_ = sampleRate;
    refreshHardwareLatencies();
    return true;
}

bool VSTAudioSystem::setBlockSize(std::uint32_t blockSize) {
    if (!initialized_) {
        blockSize_ = blockSize;
        return true;
    }
    return initialize(driver_, sampleRate_, blockSize, activeInputs_, activeOutputs_);
}

double VSTAudioSystem::inputLatencyMilliseconds() const noexcept {
    return (sampleRate_ > 0.0) ? (static_cast<double>(inputLatencySamples_) * 1000.0 / sampleRate_) : 0.0;
}

double VSTAudioSystem::outputLatencyMilliseconds() const noexcept {
    return (sampleRate_ > 0.0) ? (static_cast<double>(outputLatencySamples_) * 1000.0 / sampleRate_) : 0.0;
}

double VSTAudioSystem::roundTripLatencyMilliseconds() const noexcept {
    return inputLatencyMilliseconds() + outputLatencyMilliseconds();
}

void VSTAudioSystem::setConstrainDelayCompensation(bool constrain, double thresholdMs) noexcept {
    constrainPDC_ = constrain;
    constrainThresholdMs_ = std::max(0.0, thresholdMs);
}

std::int64_t VSTAudioSystem::computeRecordPlacementCompensationSamples(
    std::uint32_t inputChainPluginLatencySamples) const noexcept {
    std::int64_t offset = recordShiftSamples_.load(std::memory_order_acquire);
    if (adjustForRecordLatency_.load(std::memory_order_acquire)) {
        offset -= static_cast<std::int64_t>(roundTripLatencySamples());
        offset -= static_cast<std::int64_t>(inputChainPluginLatencySamples);
    }
    return offset;
}

void VSTAudioSystem::setProcessHook(AudioProcessHook hook) {
    processHook_ = std::move(hook);
}

bool VSTAudioSystem::startAudio() {
    if (!initialized_) {
        return false;
    }
    return SDK::ASIO::ASIOStart() == SDK::ASIO::ASE_OK;
}

void VSTAudioSystem::stopAudio() {
    if (initialized_) {
        SDK::ASIO::ASIOStop();
    }
}

bool VSTAudioSystem::processOfflineCycle() noexcept {
    if (!initialized_ || !driver_) {
        return false;
    }
    return driver_->pumpSingleCycle();
}

void VSTAudioSystem::onAsioBufferSwitch(long doubleBufferIndex, long /*directProcess*/) {
    if (activeSystemInstance_ != nullptr) {
        activeSystemInstance_->handleBufferSwitchRT(doubleBufferIndex);
    }
}

void VSTAudioSystem::onAsioSampleRateChanged(SDK::ASIO::ASIOSampleRate sRate) {
    if (activeSystemInstance_ != nullptr) {
        activeSystemInstance_->sampleRate_ = sRate;
        activeSystemInstance_->refreshHardwareLatencies();
    }
}

long VSTAudioSystem::onAsioMessage(long selector, long /*value*/, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case SDK::ASIO::kAsioSelectorSupported:
        case SDK::ASIO::kAsioSupportsTimeInfo:
        case SDK::ASIO::kAsioSupportsTimeCode:
            return 1L;
        case SDK::ASIO::kAsioEngineVersion:
            return 2L;
        case SDK::ASIO::kAsioLatenciesChanged:
            if (activeSystemInstance_ != nullptr) {
                activeSystemInstance_->refreshHardwareLatencies();
            }
            return 1L;
        default:
            return 0L;
    }
}

SDK::ASIO::ASIOTime* VSTAudioSystem::onAsioBufferSwitchTimeInfo(
    SDK::ASIO::ASIOTime* params, long doubleBufferIndex, long /*directProcess*/) {
    if (activeSystemInstance_ != nullptr) {
        activeSystemInstance_->handleBufferSwitchRT(doubleBufferIndex);
    }
    return params;
}

void VSTAudioSystem::handleBufferSwitchRT(long doubleBufferIndex) noexcept {
    const long bufIdx = (doubleBufferIndex == 1L) ? 1L : 0L;
    for (long i = 0; i < activeInputs_; ++i) {
        inputChannelPtrs_[static_cast<std::size_t>(i)] =
            static_cast<const float*>(asioBuffers_[static_cast<std::size_t>(i)].buffers[bufIdx]);
    }
    for (long o = 0; o < activeOutputs_; ++o) {
        float* outBuf = static_cast<float*>(
            asioBuffers_[static_cast<std::size_t>(activeInputs_ + o)].buffers[bufIdx]);
        outputChannelPtrs_[static_cast<std::size_t>(o)] = outBuf;
        if (outBuf != nullptr) {
            std::memset(outBuf, 0, static_cast<std::size_t>(blockSize_) * sizeof(float));
        }
    }

    if (processHook_) {
        processHook_(inputChannelPtrs_.data(), activeInputs_,
                     outputChannelPtrs_.data(), activeOutputs_,
                     static_cast<long>(blockSize_));
    }

    SDK::ASIO::ASIOOutputReady();
}

void VSTAudioSystem::refreshHardwareLatencies() noexcept {
    long inLat = 0;
    long outLat = 0;
    if (SDK::ASIO::ASIOGetLatencies(&inLat, &outLat) == SDK::ASIO::ASE_OK) {
        inputLatencySamples_ = inLat;
        outputLatencySamples_ = outLat;
    }
}

} // namespace AlphaStudio::Core
