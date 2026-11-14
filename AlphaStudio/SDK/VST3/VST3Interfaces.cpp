#include "AlphaStudio/SDK/VST3/VST3Interfaces.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AlphaStudio::SDK::VST3 {

// ============================================================================
// MemoryBStream
// ============================================================================
tresult MemoryBStream::queryInterface(const FUID& targetIid, void** obj) {
    if (obj == nullptr) {
        return kInvalidArgument;
    }
    if (targetIid == FUnknown::iid || targetIid == IBStream::iid) {
        addRef();
        *obj = static_cast<IBStream*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

uint32 MemoryBStream::release() {
    const uint32 r = --refCount_;
    if (r == 0) {
        delete this;
    }
    return r;
}

tresult MemoryBStream::read(void* buffer, int32 numBytes, int32* numBytesRead) {
    if (buffer == nullptr || numBytes < 0) {
        return kInvalidArgument;
    }
    const std::size_t available = (cursor_ < data_.size()) ? (data_.size() - cursor_) : 0;
    const std::size_t toCopy = std::min<std::size_t>(available, static_cast<std::size_t>(numBytes));
    if (toCopy > 0) {
        std::memcpy(buffer, data_.data() + cursor_, toCopy);
        cursor_ += toCopy;
    }
    if (numBytesRead != nullptr) {
        *numBytesRead = static_cast<int32>(toCopy);
    }
    return kResultOk;
}

tresult MemoryBStream::write(const void* buffer, int32 numBytes, int32* numBytesWritten) {
    if (buffer == nullptr || numBytes < 0) {
        return kInvalidArgument;
    }
    const std::size_t count = static_cast<std::size_t>(numBytes);
    if (cursor_ + count > data_.size()) {
        data_.resize(cursor_ + count);
    }
    if (count > 0) {
        std::memcpy(data_.data() + cursor_, buffer, count);
        cursor_ += count;
    }
    if (numBytesWritten != nullptr) {
        *numBytesWritten = numBytes;
    }
    return kResultOk;
}

tresult MemoryBStream::seek(int64 pos, int32 mode, int64* result) {
    int64 nextPos = 0;
    if (mode == 0) { // SEEK_SET
        nextPos = pos;
    } else if (mode == 1) { // SEEK_CUR
        nextPos = static_cast<int64>(cursor_) + pos;
    } else if (mode == 2) { // SEEK_END
        nextPos = static_cast<int64>(data_.size()) + pos;
    } else {
        return kInvalidArgument;
    }
    if (nextPos < 0) {
        return kInvalidArgument;
    }
    cursor_ = static_cast<std::size_t>(nextPos);
    if (result != nullptr) {
        *result = static_cast<int64>(cursor_);
    }
    return kResultOk;
}

tresult MemoryBStream::tell(int64* pos) {
    if (pos == nullptr) {
        return kInvalidArgument;
    }
    *pos = static_cast<int64>(cursor_);
    return kResultOk;
}

void MemoryBStream::setData(std::span<const std::uint8_t> bytes) {
    data_.assign(bytes.begin(), bytes.end());
    cursor_ = 0;
}

// ============================================================================
// Preallocated Parameter Changes & EventList
// ============================================================================
tresult PreallocatedParamValueQueue::getPoint(int32 index, int32& sampleOffset,
                                              ParamValue& value) const noexcept {
    if (index < 0 || index >= count_) {
        return kInvalidArgument;
    }
    sampleOffset = points_[static_cast<std::size_t>(index)].offset;
    value = points_[static_cast<std::size_t>(index)].val;
    return kResultOk;
}

tresult PreallocatedParamValueQueue::addPoint(int32 sampleOffset, ParamValue value,
                                              int32& index) noexcept {
    if (count_ >= kMaxPoints) {
        return kInternalError;
    }
    index = count_;
    points_[static_cast<std::size_t>(count_)] = Point{sampleOffset, std::clamp(value, 0.0, 1.0)};
    ++count_;
    return kResultOk;
}

IParamValueQueue* PreallocatedParameterChanges::getParameterData(int32 index) noexcept {
    if (index < 0 || index >= count_) {
        return nullptr;
    }
    return &queues_[static_cast<std::size_t>(index)];
}

IParamValueQueue* PreallocatedParameterChanges::addParameterData(const ParamID& id,
                                                                 int32& index) noexcept {
    for (int32 i = 0; i < count_; ++i) {
        if (queues_[static_cast<std::size_t>(i)].getParameterId() == id) {
            index = i;
            return &queues_[static_cast<std::size_t>(i)];
        }
    }
    if (count_ >= kMaxQueues) {
        return nullptr;
    }
    index = count_;
    queues_[static_cast<std::size_t>(count_)].reset(id);
    ++count_;
    return &queues_[static_cast<std::size_t>(index)];
}

tresult PreallocatedEventList::getEvent(int32 index, Event& e) const noexcept {
    if (index < 0 || index >= count_) {
        return kInvalidArgument;
    }
    e = events_[static_cast<std::size_t>(index)];
    return kResultOk;
}

tresult PreallocatedEventList::addEvent(const Event& e) noexcept {
    if (count_ >= kMaxEvents) {
        return kInternalError;
    }
    events_[static_cast<std::size_t>(count_++)] = e;
    return kResultOk;
}

// ============================================================================
// VST3ProcessorComponentBase
// ============================================================================
VST3ProcessorComponentBase::VST3ProcessorComponentBase(FUID controllerId)
    : controllerClassId_(controllerId) {}

tresult VST3ProcessorComponentBase::queryInterface(const FUID& targetIid, void** obj) {
    if (obj == nullptr) {
        return kInvalidArgument;
    }
    if (targetIid == FUnknown::iid || targetIid == IComponent::iid) {
        addRef();
        *obj = static_cast<IComponent*>(this);
        return kResultOk;
    }
    if (targetIid == IAudioProcessor::iid) {
        addRef();
        *obj = static_cast<IAudioProcessor*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

uint32 VST3ProcessorComponentBase::release() {
    const uint32 r = --refCount_;
    if (r == 0) {
        delete this;
    }
    return r;
}

tresult VST3ProcessorComponentBase::initialize(FUnknown* /*context*/) {
    if (state_ != VST3LifecycleState::Uninitialized) {
        return kInvalidArgument;
    }
    state_ = VST3LifecycleState::Initialized;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::terminate() {
    if (state_ == VST3LifecycleState::Processing) {
        setProcessing(false);
    }
    if (state_ == VST3LifecycleState::Activated) {
        setActive(false);
    }
    state_ = VST3LifecycleState::Uninitialized;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::getControllerClassId(FUID& classId) const {
    classId = controllerClassId_;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::setIoMode(int32 mode) {
    ioMode_ = mode;
    return kResultOk;
}

int32 VST3ProcessorComponentBase::getBusCount(MediaTypes type, BusDirections dir) const {
    if (type == kAudio) {
        return static_cast<int32>(dir == kInput ? audioInputs_.size() : audioOutputs_.size());
    }
    if (type == kEvent) {
        return static_cast<int32>(dir == kInput ? eventInputs_.size() : eventOutputs_.size());
    }
    return 0;
}

tresult VST3ProcessorComponentBase::getBusInfo(MediaTypes type, BusDirections dir,
                                               int32 index, BusInfo& bus) const {
    const auto* vec = (type == kAudio)
        ? (dir == kInput ? &audioInputs_ : &audioOutputs_)
        : (dir == kInput ? &eventInputs_ : &eventOutputs_);
    if (index < 0 || static_cast<std::size_t>(index) >= vec->size()) {
        return kInvalidArgument;
    }
    bus = (*vec)[static_cast<std::size_t>(index)].info;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) const {
    if (inInfo.mediaType == kAudio && inInfo.busIndex == 0 && !audioOutputs_.empty()) {
        outInfo.mediaType = kAudio;
        outInfo.busIndex = 0;
        outInfo.channel = inInfo.channel;
        return kResultOk;
    }
    return kNotImplemented;
}

tresult VST3ProcessorComponentBase::activateBus(MediaTypes type, BusDirections dir,
                                                int32 index, bool state) {
    auto* vec = (type == kAudio)
        ? (dir == kInput ? &audioInputs_ : &audioOutputs_)
        : (dir == kInput ? &eventInputs_ : &eventOutputs_);
    if (index < 0 || static_cast<std::size_t>(index) >= vec->size()) {
        return kInvalidArgument;
    }
    (*vec)[static_cast<std::size_t>(index)].active = state;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::setupProcessing(ProcessSetup& setup) {
    // Allowed from Initialized or SetupDone (never while Activated or Processing)
    if (state_ != VST3LifecycleState::Initialized && state_ != VST3LifecycleState::SetupDone) {
        return kInvalidArgument;
    }
    if (setup.maxSamplesPerBlock <= 0 || setup.sampleRate <= 0.0) {
        return kInvalidArgument;
    }
    if (canProcessSampleSize(setup.symbolicSampleSize) != kResultOk) {
        return kInvalidArgument;
    }
    currentSetup_ = setup;
    onSetupProcessing(currentSetup_);
    state_ = VST3LifecycleState::SetupDone;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::setActive(bool state) {
    if (state) {
        if (state_ != VST3LifecycleState::SetupDone) {
            return kInvalidArgument;
        }
        state_ = VST3LifecycleState::Activated;
        return kResultOk;
    } else {
        if (state_ == VST3LifecycleState::Processing) {
            setProcessing(false);
        }
        if (state_ == VST3LifecycleState::Activated) {
            state_ = VST3LifecycleState::SetupDone;
        }
        return kResultOk;
    }
}

tresult VST3ProcessorComponentBase::setProcessing(bool state) {
    if (state) {
        if (state_ != VST3LifecycleState::Activated) {
            return kInvalidArgument;
        }
        state_ = VST3LifecycleState::Processing;
        return kResultOk;
    } else {
        if (state_ == VST3LifecycleState::Processing) {
            state_ = VST3LifecycleState::Activated;
        }
        return kResultOk;
    }
}

tresult VST3ProcessorComponentBase::setState(IBStream* /*state*/) {
    return kResultOk;
}

tresult VST3ProcessorComponentBase::getState(IBStream* /*state*/) {
    return kResultOk;
}

tresult VST3ProcessorComponentBase::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                       SpeakerArrangement* outputs, int32 numOuts) {
    if (state_ == VST3LifecycleState::Activated || state_ == VST3LifecycleState::Processing) {
        return kInvalidArgument;
    }
    if (numIns < 0 || numOuts < 0 ||
        static_cast<std::size_t>(numIns) > audioInputs_.size() ||
        static_cast<std::size_t>(numOuts) > audioOutputs_.size()) {
        return kResultFalse;
    }
    for (int32 i = 0; i < numIns; ++i) {
        audioInputs_[static_cast<std::size_t>(i)].arrangement = inputs[i];
        audioInputs_[static_cast<std::size_t>(i)].info.channelCount =
            SpeakerArr::getChannelCount(inputs[i]);
    }
    for (int32 o = 0; o < numOuts; ++o) {
        audioOutputs_[static_cast<std::size_t>(o)].arrangement = outputs[o];
        audioOutputs_[static_cast<std::size_t>(o)].info.channelCount =
            SpeakerArr::getChannelCount(outputs[o]);
    }
    return kResultOk;
}

tresult VST3ProcessorComponentBase::getBusArrangement(BusDirections dir, int32 index,
                                                      SpeakerArrangement& arr) const {
    const auto& vec = (dir == kInput) ? audioInputs_ : audioOutputs_;
    if (index < 0 || static_cast<std::size_t>(index) >= vec.size()) {
        return kInvalidArgument;
    }
    arr = vec[static_cast<std::size_t>(index)].arrangement;
    return kResultOk;
}

tresult VST3ProcessorComponentBase::canProcessSampleSize(int32 symbolicSampleSize) const {
    return (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
        ? kResultOk
        : kNotImplemented;
}

tresult VST3ProcessorComponentBase::process(ProcessData& data) {
    // Parameter flush without audio is permitted when Activated or Processing
    if (state_ != VST3LifecycleState::Processing && state_ != VST3LifecycleState::Activated) {
        return kInvalidArgument;
    }

    if (data.inputParameterChanges != nullptr) {
        const int32 numParams = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParams; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (queue != nullptr && queue->getPointCount() > 0) {
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk) {
                    onFlushParameterChange(queue->getParameterId(), value);
                }
            }
        }
    }

    // VST 3 Parameter Flush without audio (numSamples == 0)
    if (data.numSamples <= 0) {
        return kResultOk;
    }

    if (data.symbolicSampleSize == kSample64) {
        onProcessAudio64(data);
    } else {
        onProcessAudio32(data);
    }
    return kResultOk;
}

void VST3ProcessorComponentBase::onProcessAudio32(ProcessData& data) noexcept {
    // Safe pass-through handling inactive null buses and shared in-place buffers
    if (data.outputs == nullptr) {
        return;
    }
    for (int32 b = 0; b < data.numOutputs; ++b) {
        AudioBusBuffers& outBus = data.outputs[b];
        if (outBus.channelBuffers32 == nullptr) {
            continue;
        }
        const bool hasInput = (data.inputs != nullptr && b < data.numInputs &&
                               data.inputs[b].channelBuffers32 != nullptr);
        for (int32 c = 0; c < outBus.numChannels; ++c) {
            Sample32* dst = outBus.channelBuffers32[c];
            if (dst == nullptr) {
                continue;
            }
            const Sample32* src = (hasInput && c < data.inputs[b].numChannels)
                ? data.inputs[b].channelBuffers32[c]
                : nullptr;
            if (src != nullptr) {
                if (src != dst) { // Guard shared in-place buffers
                    std::memmove(dst, src, static_cast<std::size_t>(data.numSamples) * sizeof(Sample32));
                }
            } else {
                std::memset(dst, 0, static_cast<std::size_t>(data.numSamples) * sizeof(Sample32));
            }
        }
    }
}

void VST3ProcessorComponentBase::onProcessAudio64(ProcessData& data) noexcept {
    if (data.outputs == nullptr) {
        return;
    }
    for (int32 b = 0; b < data.numOutputs; ++b) {
        AudioBusBuffers& outBus = data.outputs[b];
        if (outBus.channelBuffers64 == nullptr) {
            continue;
        }
        const bool hasInput = (data.inputs != nullptr && b < data.numInputs &&
                               data.inputs[b].channelBuffers64 != nullptr);
        for (int32 c = 0; c < outBus.numChannels; ++c) {
            Sample64* dst = outBus.channelBuffers64[c];
            if (dst == nullptr) {
                continue;
            }
            const Sample64* src = (hasInput && c < data.inputs[b].numChannels)
                ? data.inputs[b].channelBuffers64[c]
                : nullptr;
            if (src != nullptr) {
                if (src != dst) {
                    std::memmove(dst, src, static_cast<std::size_t>(data.numSamples) * sizeof(Sample64));
                }
            } else {
                std::memset(dst, 0, static_cast<std::size_t>(data.numSamples) * sizeof(Sample64));
            }
        }
    }
}

void VST3ProcessorComponentBase::addAudioInput(const char* name, SpeakerArrangement arr, BusTypes busType) {
    InternalBus bus;
    bus.info.mediaType = kAudio;
    bus.info.direction = kInput;
    bus.info.channelCount = SpeakerArr::getChannelCount(arr);
    std::snprintf(bus.info.name, sizeof(bus.info.name), "%s", name ? name : "Audio In");
    bus.info.busType = busType;
    bus.info.flags = (busType == kMain) ? 1U : 0U;
    bus.arrangement = arr;
    bus.active = (busType == kMain);
    audioInputs_.push_back(bus);
}

void VST3ProcessorComponentBase::addAudioOutput(const char* name, SpeakerArrangement arr, BusTypes busType) {
    InternalBus bus;
    bus.info.mediaType = kAudio;
    bus.info.direction = kOutput;
    bus.info.channelCount = SpeakerArr::getChannelCount(arr);
    std::snprintf(bus.info.name, sizeof(bus.info.name), "%s", name ? name : "Audio Out");
    bus.info.busType = busType;
    bus.info.flags = 1U;
    bus.arrangement = arr;
    bus.active = true;
    audioOutputs_.push_back(bus);
}

void VST3ProcessorComponentBase::addEventInput(const char* name) {
    InternalBus bus;
    bus.info.mediaType = kEvent;
    bus.info.direction = kInput;
    bus.info.channelCount = 16;
    std::snprintf(bus.info.name, sizeof(bus.info.name), "%s", name ? name : "MIDI Event In");
    bus.info.busType = kMain;
    bus.active = true;
    eventInputs_.push_back(bus);
}

void VST3ProcessorComponentBase::addEventOutput(const char* name) {
    InternalBus bus;
    bus.info.mediaType = kEvent;
    bus.info.direction = kOutput;
    bus.info.channelCount = 16;
    std::snprintf(bus.info.name, sizeof(bus.info.name), "%s", name ? name : "MIDI Event Out");
    bus.info.busType = kMain;
    bus.active = true;
    eventOutputs_.push_back(bus);
}

// ============================================================================
// VST3EditControllerBase
// ============================================================================
tresult VST3EditControllerBase::queryInterface(const FUID& targetIid, void** obj) {
    if (obj == nullptr) {
        return kInvalidArgument;
    }
    if (targetIid == FUnknown::iid || targetIid == IEditController::iid) {
        addRef();
        *obj = static_cast<IEditController*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

uint32 VST3EditControllerBase::release() {
    const uint32 r = --refCount_;
    if (r == 0) {
        delete this;
    }
    return r;
}

tresult VST3EditControllerBase::initialize(FUnknown* /*context*/) {
    if (initialized_) {
        return kInvalidArgument;
    }
    initialized_ = true;
    return kResultOk;
}

tresult VST3EditControllerBase::terminate() {
    initialized_ = false;
    handler_ = nullptr;
    return kResultOk;
}

tresult VST3EditControllerBase::setComponentState(IBStream* /*state*/) { return kResultOk; }
tresult VST3EditControllerBase::setState(IBStream* /*state*/) { return kResultOk; }
tresult VST3EditControllerBase::getState(IBStream* /*state*/) { return kResultOk; }

int32 VST3EditControllerBase::getParameterCount() const {
    return static_cast<int32>(parameters_.size());
}

tresult VST3EditControllerBase::getParameterInfo(int32 paramIndex, ParameterInfo& info) const {
    if (paramIndex < 0 || static_cast<std::size_t>(paramIndex) >= parameters_.size()) {
        return kInvalidArgument;
    }
    info = parameters_[static_cast<std::size_t>(paramIndex)].info;
    return kResultOk;
}

tresult VST3EditControllerBase::getParamStringByValue(ParamID id, ParamValue valueNormalized,
                                                      char outString[128]) const {
    const ParamValue plain = normalizedParamToPlain(id, valueNormalized);
    std::snprintf(outString, 128, "%.2f", plain);
    return kResultOk;
}

tresult VST3EditControllerBase::getParamValueByString(ParamID id, const char* string,
                                                      ParamValue& valueNormalized) const {
    if (string == nullptr) {
        return kInvalidArgument;
    }
    const double plain = std::strtod(string, nullptr);
    valueNormalized = plainParamToNormalized(id, plain);
    return kResultOk;
}

ParamValue VST3EditControllerBase::normalizedParamToPlain(ParamID id, ParamValue valueNormalized) const {
    for (const auto& p : parameters_) {
        if (p.info.id == id) {
            return p.minPlain + std::clamp(valueNormalized, 0.0, 1.0) * (p.maxPlain - p.minPlain);
        }
    }
    return valueNormalized;
}

ParamValue VST3EditControllerBase::plainParamToNormalized(ParamID id, ParamValue plainValue) const {
    for (const auto& p : parameters_) {
        if (p.info.id == id) {
            const double span = p.maxPlain - p.minPlain;
            if (std::abs(span) < 1e-12) {
                return 0.0;
            }
            return std::clamp((plainValue - p.minPlain) / span, 0.0, 1.0);
        }
    }
    return std::clamp(plainValue, 0.0, 1.0);
}

ParamValue VST3EditControllerBase::getParamNormalized(ParamID id) const {
    for (const auto& p : parameters_) {
        if (p.info.id == id) {
            return p.currentNormalized;
        }
    }
    return 0.0;
}

tresult VST3EditControllerBase::setParamNormalized(ParamID id, ParamValue value) {
    for (auto& p : parameters_) {
        if (p.info.id == id) {
            p.currentNormalized = std::clamp(value, 0.0, 1.0);
            return kResultOk;
        }
    }
    return kInvalidArgument;
}

tresult VST3EditControllerBase::setComponentHandler(IComponentHandler* handler) {
    handler_ = handler;
    return kResultOk;
}

void VST3EditControllerBase::registerParameter(ParamID id, const char* title, const char* units,
                                               ParamValue minPlain, ParamValue maxPlain,
                                               ParamValue defaultPlain, int32 flags) {
    RegisteredParam rp;
    rp.info.id = id;
    std::snprintf(rp.info.title, sizeof(rp.info.title), "%s", title ? title : "Param");
    std::snprintf(rp.info.shortTitle, sizeof(rp.info.shortTitle), "%s", title ? title : "Prm");
    std::snprintf(rp.info.units, sizeof(rp.info.units), "%s", units ? units : "");
    rp.info.flags = flags;
    rp.minPlain = minPlain;
    rp.maxPlain = maxPlain;
    const double span = (std::abs(maxPlain - minPlain) > 1e-12) ? (maxPlain - minPlain) : 1.0;
    rp.info.defaultNormalizedValue = std::clamp((defaultPlain - minPlain) / span, 0.0, 1.0);
    rp.currentNormalized = rp.info.defaultNormalizedValue;
    parameters_.push_back(rp);
}

} // namespace AlphaStudio::SDK::VST3
