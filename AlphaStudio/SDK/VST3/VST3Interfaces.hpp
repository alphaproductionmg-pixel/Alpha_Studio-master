#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::SDK::VST3 {

// ============================================================================
// Clean-Room VST 3.0.0 - 3.8 Binary-Decoupled Interfaces & Types
// ============================================================================
using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
using tresult = std::int32_t;
using ParamID = std::uint32_t;
using ParamValue = double;
using Sample32 = float;
using Sample64 = double;
using TQuarterNotes = double;
using TSamples = std::int64_t;
using SpeakerArrangement = std::uint64_t;

inline constexpr tresult kResultOk          = 0;
inline constexpr tresult kResultTrue        = 0;
inline constexpr tresult kResultFalse       = 1;
inline constexpr tresult kNoInterface       = -1;
inline constexpr tresult kInvalidArgument   = -2;
inline constexpr tresult kNotImplemented    = -3;
inline constexpr tresult kInternalError     = -4;
inline constexpr tresult kNotInitialized    = -5;

struct FUID {
    std::array<std::uint8_t, 16> bytes{};

    constexpr FUID() noexcept = default;
    constexpr FUID(uint32 l1, uint32 l2, uint32 l3, uint32 l4) noexcept {
        for (int i = 0; i < 4; ++i) {
            bytes[static_cast<std::size_t>(i)]      = static_cast<std::uint8_t>((l1 >> (24 - i * 8)) & 0xFFU);
            bytes[static_cast<std::size_t>(4 + i)]  = static_cast<std::uint8_t>((l2 >> (24 - i * 8)) & 0xFFU);
            bytes[static_cast<std::size_t>(8 + i)]  = static_cast<std::uint8_t>((l3 >> (24 - i * 8)) & 0xFFU);
            bytes[static_cast<std::size_t>(12 + i)] = static_cast<std::uint8_t>((l4 >> (24 - i * 8)) & 0xFFU);
        }
    }
    constexpr bool operator==(const FUID& other) const noexcept = default;
};

// Speaker Arrangements (Mono, Stereo, Quad, 5.0, Surround 5.1)
namespace SpeakerArr {
inline constexpr SpeakerArrangement kEmpty  = 0ULL;
inline constexpr SpeakerArrangement kMono   = (1ULL << 2);                               // C
inline constexpr SpeakerArrangement kStereo = (1ULL << 0) | (1ULL << 1);                 // L | R
inline constexpr SpeakerArrangement kLCR    = kStereo | (1ULL << 2);                     // L | R | C
inline constexpr SpeakerArrangement kQuad   = kStereo | (1ULL << 4) | (1ULL << 5);       // L | R | Ls | Rs
inline constexpr SpeakerArrangement k50     = kLCR | (1ULL << 4) | (1ULL << 5);          // L | R | C | Ls | Rs
inline constexpr SpeakerArrangement k51     = k50 | (1ULL << 3);                         // L | R | C | LFE | Ls | Rs

[[nodiscard]] constexpr int32 getChannelCount(SpeakerArrangement arr) noexcept {
    int32 count = 0;
    while (arr != 0ULL) {
        count += static_cast<int32>(arr & 1ULL);
        arr >>= 1ULL;
    }
    return count;
}
} // namespace SpeakerArr

enum MediaTypes : int32 {
    kAudio = 0,
    kEvent = 1,
    kNumMediaTypes = 2
};

enum BusDirections : int32 {
    kInput  = 0,
    kOutput = 1
};

enum BusTypes : int32 {
    kMain = 0,
    kAux  = 1 // Used for VST 3 Sidechain input
};

enum IoModes : int32 {
    kSimple            = 0,
    kAdvanced          = 1,
    kOfflineProcessing = 2
};

enum SymbolicSampleSizes : int32 {
    kSample32 = 0,
    kSample64 = 1
};

enum ProcessModes : int32 {
    kRealtime = 0,
    kPrefetch = 1,
    kOffline  = 2
};

enum RestartFlags : int32 {
    kReloadComponent          = 1 << 0,
    kIoChanged                = 1 << 1,
    kParamValuesChanged       = 1 << 2,
    kLatencyChanged           = 1 << 3,
    kParamTitlesChanged       = 1 << 4,
    kMidiCCAssignmentChanged  = 1 << 5,
    kNoteExpressionChanged    = 1 << 6
};

// Strict VST 3 Component Lifecycle State Machine
enum class VST3LifecycleState : std::uint8_t {
    Uninitialized = 0,
    Initialized   = 1,
    SetupDone     = 2,
    Activated     = 3,
    Processing    = 4
};

struct BusInfo {
    MediaTypes mediaType{kAudio};
    BusDirections direction{kInput};
    int32 channelCount{2};
    char name[128]{"Main"};
    BusTypes busType{kMain};
    uint32 flags{1}; // kDefaultActive = 1
};

struct RoutingInfo {
    MediaTypes mediaType{kAudio};
    int32 busIndex{0};
    int32 channel{-1};
};

// ============================================================================
// Base COM-like Interface & Binary State Stream
// ============================================================================
class FUnknown {
public:
    static constexpr FUID iid{0x00000000U, 0x00000000U, 0xC0000000U, 0x00000046U};
    virtual ~FUnknown() = default;
    virtual tresult queryInterface(const FUID& targetIid, void** obj) = 0;
    virtual uint32 addRef() = 0;
    virtual uint32 release() = 0;
};

class IBStream : public FUnknown {
public:
    static constexpr FUID iid{0xC3BF6EA2U, 0x30994752U, 0x9B6BF990U, 0x1EE33E9BU};
    virtual tresult read(void* buffer, int32 numBytes, int32* numBytesRead) = 0;
    virtual tresult write(const void* buffer, int32 numBytes, int32* numBytesWritten) = 0;
    virtual tresult seek(int64 pos, int32 mode, int64* result) = 0;
    virtual tresult tell(int64* pos) = 0;
};

class MemoryBStream final : public IBStream {
public:
    tresult queryInterface(const FUID& targetIid, void** obj) override;
    uint32 addRef() override { return ++refCount_; }
    uint32 release() override;

    tresult read(void* buffer, int32 numBytes, int32* numBytesRead) override;
    tresult write(const void* buffer, int32 numBytes, int32* numBytesWritten) override;
    tresult seek(int64 pos, int32 mode, int64* result) override;
    tresult tell(int64* pos) override;

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    void setData(std::span<const std::uint8_t> bytes);

private:
    std::atomic<uint32> refCount_{1};
    std::vector<std::uint8_t> data_;
    std::size_t cursor_{0};
};

// ============================================================================
// Real-Time Parameter Changes, Events, ProcessContext & ProcessData
// ============================================================================
class IParamValueQueue {
public:
    virtual ~IParamValueQueue() = default;
    [[nodiscard]] virtual ParamID getParameterId() const noexcept = 0;
    [[nodiscard]] virtual int32 getPointCount() const noexcept = 0;
    virtual tresult getPoint(int32 index, int32& sampleOffset, ParamValue& value) const noexcept = 0;
    virtual tresult addPoint(int32 sampleOffset, ParamValue value, int32& index) noexcept = 0;
};

class IParameterChanges {
public:
    virtual ~IParameterChanges() = default;
    [[nodiscard]] virtual int32 getParameterCount() const noexcept = 0;
    virtual IParamValueQueue* getParameterData(int32 index) noexcept = 0;
    virtual IParamValueQueue* addParameterData(const ParamID& id, int32& index) noexcept = 0;
};

struct Event {
    enum EventTypes : uint16_t {
        kNoteOnEvent              = 0,
        kNoteOffEvent             = 1,
        kDataEvent                = 2,
        kPolyPressureEvent        = 3,
        kNoteExpressionValueEvent = 4
    };

    int32 busIndex{0};
    int32 sampleOffset{0};
    TQuarterNotes ppqPosition{0.0};
    uint16_t flags{0};
    uint16_t type{kNoteOnEvent};

    struct NoteOnData {
        int16_t channel{0};
        int16_t pitch{60};
        float tuning{0.0f}; // cents
        float velocity{0.8f};
        int32 length{0};
        int32 noteId{-1};
    };
    struct NoteOffData {
        int16_t channel{0};
        int16_t pitch{60};
        float velocity{0.0f};
        int32 noteId{-1};
        float tuning{0.0f};
    };
    struct NoteExpressionData {
        uint32 typeId{0};
        int32 noteId{-1};
        double value{0.5};
    };

    union {
        NoteOnData noteOn{};
        NoteOffData noteOff;
        NoteExpressionData noteExpression;
    };
};

class IEventList {
public:
    virtual ~IEventList() = default;
    [[nodiscard]] virtual int32 getEventCount() const noexcept = 0;
    virtual tresult getEvent(int32 index, Event& e) const noexcept = 0;
    virtual tresult addEvent(const Event& e) noexcept = 0;
};

// Pre-allocated Real-Time Safe Implementations of ParameterChanges & EventList
class PreallocatedParamValueQueue final : public IParamValueQueue {
public:
    static constexpr int32 kMaxPoints = 64;
    void reset(ParamID id) noexcept { paramId_ = id; count_ = 0; }
    [[nodiscard]] ParamID getParameterId() const noexcept override { return paramId_; }
    [[nodiscard]] int32 getPointCount() const noexcept override { return count_; }
    tresult getPoint(int32 index, int32& sampleOffset, ParamValue& value) const noexcept override;
    tresult addPoint(int32 sampleOffset, ParamValue value, int32& index) noexcept override;

private:
    struct Point { int32 offset{0}; ParamValue val{0.0}; };
    ParamID paramId_{0};
    int32 count_{0};
    std::array<Point, kMaxPoints> points_{};
};

class PreallocatedParameterChanges final : public IParameterChanges {
public:
    static constexpr int32 kMaxQueues = 128;
    void clear() noexcept { count_ = 0; }
    [[nodiscard]] int32 getParameterCount() const noexcept override { return count_; }
    IParamValueQueue* getParameterData(int32 index) noexcept override;
    IParamValueQueue* addParameterData(const ParamID& id, int32& index) noexcept override;

private:
    int32 count_{0};
    std::array<PreallocatedParamValueQueue, kMaxQueues> queues_{};
};

class PreallocatedEventList final : public IEventList {
public:
    static constexpr int32 kMaxEvents = 512;
    void clear() noexcept { count_ = 0; }
    [[nodiscard]] int32 getEventCount() const noexcept override { return count_; }
    tresult getEvent(int32 index, Event& e) const noexcept override;
    tresult addEvent(const Event& e) noexcept override;

private:
    int32 count_{0};
    std::array<Event, kMaxEvents> events_{};
};

struct ProcessSetup {
    int32 processMode{kRealtime};
    int32 symbolicSampleSize{kSample32};
    int32 maxSamplesPerBlock{512};
    double sampleRate{48000.0};
};

struct AudioBusBuffers {
    int32 numChannels{0};
    uint64 silenceFlags{0};
    union {
        Sample32** channelBuffers32{nullptr};
        Sample64** channelBuffers64;
    };
};

struct ProcessContext {
    enum StatesAndFlags : uint32 {
        kPlaying          = 1U << 1,
        kCycleActive      = 1U << 2,
        kRecording        = 1U << 3,
        kSystemTimeValid  = 1U << 8,
        kProjectTimeValid = 1U << 9,
        kTempoValid       = 1U << 10,
        kBarPositionValid = 1U << 11,
        kCycleValid       = 1U << 12,
        kTimeSigValid     = 1U << 13
    };

    uint32 state{kTempoValid | kTimeSigValid | kProjectTimeValid};
    double sampleRate{48000.0};
    TSamples projectTimeSamples{0};
    int64 systemTime{0};
    TSamples continousTimeSamples{0};
    TQuarterNotes projectTimeMusic{0.0};
    TQuarterNotes barPositionMusic{0.0};
    TQuarterNotes cycleStartMusic{0.0};
    TQuarterNotes cycleEndMusic{4.0};
    double tempo{120.0};
    int32 timeSigNumerator{4};
    int32 timeSigDenominator{4};
};

struct ProcessData {
    int32 processMode{kRealtime};
    int32 symbolicSampleSize{kSample32};
    int32 numSamples{0}; // numSamples == 0 triggers parameter flush without audio!
    int32 numInputs{0};
    int32 numOutputs{0};
    AudioBusBuffers* inputs{nullptr};
    AudioBusBuffers* outputs{nullptr};
    IParameterChanges* inputParameterChanges{nullptr};
    IParameterChanges* outputParameterChanges{nullptr};
    IEventList* inputEvents{nullptr};
    IEventList* outputEvents{nullptr};
    ProcessContext* processContext{nullptr};
};

// ============================================================================
// Decoupled Processor & EditController Interfaces
// ============================================================================
class IComponentHandler : public FUnknown {
public:
    static constexpr FUID iid{0x93A0BEA3U, 0x0BD045DBU, 0x8E890B0CU, 0xC1E46AC6U};
    virtual tresult beginEdit(ParamID id) = 0;
    virtual tresult performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult endEdit(ParamID id) = 0;
    virtual tresult restartComponent(int32 flags) = 0;
};

struct ParameterInfo {
    enum ParameterFlags : int32 {
        kNoFlags      = 0,
        kCanAutomate  = 1 << 0,
        kIsReadOnly   = 1 << 1,
        kIsWrapAround = 1 << 2,
        kIsList       = 1 << 3,
        kIsBypass     = 1 << 16
    };

    ParamID id{0};
    char title[128]{};
    char shortTitle[128]{};
    char units[128]{};
    int32 stepCount{0};
    ParamValue defaultNormalizedValue{0.0};
    int32 unitId{0};
    int32 flags{kCanAutomate};
};

class IEditController : public FUnknown {
public:
    static constexpr FUID iid{0xDCD7BBE3U, 0x7742448DU, 0xA874ACC9U, 0x79C759EBU};
    virtual tresult initialize(FUnknown* context) = 0;
    virtual tresult terminate() = 0;
    virtual tresult setComponentState(IBStream* state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
    [[nodiscard]] virtual int32 getParameterCount() const = 0;
    virtual tresult getParameterInfo(int32 paramIndex, ParameterInfo& info) const = 0;
    virtual tresult getParamStringByValue(ParamID id, ParamValue valueNormalized, char outString[128]) const = 0;
    virtual tresult getParamValueByString(ParamID id, const char* string, ParamValue& valueNormalized) const = 0;
    [[nodiscard]] virtual ParamValue normalizedParamToPlain(ParamID id, ParamValue valueNormalized) const = 0;
    [[nodiscard]] virtual ParamValue plainParamToNormalized(ParamID id, ParamValue plainValue) const = 0;
    [[nodiscard]] virtual ParamValue getParamNormalized(ParamID id) const = 0;
    virtual tresult setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult setComponentHandler(IComponentHandler* handler) = 0;
};

class IComponent : public FUnknown {
public:
    static constexpr FUID iid{0xE831FF31U, 0xF2D54301U, 0x928EBBEEU, 0x25697802U};
    virtual tresult initialize(FUnknown* context) = 0;
    virtual tresult terminate() = 0;
    virtual tresult getControllerClassId(FUID& classId) const = 0;
    virtual tresult setIoMode(int32 mode) = 0;
    [[nodiscard]] virtual int32 getBusCount(MediaTypes type, BusDirections dir) const = 0;
    virtual tresult getBusInfo(MediaTypes type, BusDirections dir, int32 index, BusInfo& bus) const = 0;
    virtual tresult getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) const = 0;
    virtual tresult activateBus(MediaTypes type, BusDirections dir, int32 index, bool state) = 0;
    virtual tresult setActive(bool state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
};

class IAudioProcessor : public FUnknown {
public:
    static constexpr FUID iid{0x42043F99U, 0xB7DA453CU, 0xA569E79DU, 0x9AAEC33DU};
    virtual tresult setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                       SpeakerArrangement* outputs, int32 numOuts) = 0;
    virtual tresult getBusArrangement(BusDirections dir, int32 index, SpeakerArrangement& arr) const = 0;
    virtual tresult canProcessSampleSize(int32 symbolicSampleSize) const = 0;
    [[nodiscard]] virtual uint32 getLatencySamples() const = 0;
    virtual tresult setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult setProcessing(bool state) = 0;
    virtual tresult process(ProcessData& data) = 0;
    [[nodiscard]] virtual uint32 getTailSamples() const = 0;
};

// ============================================================================
// Clean-Room Base Implementations Enforcing Strict State Machine & Bus Safety
// ============================================================================
class VST3ProcessorComponentBase : public IComponent, public IAudioProcessor {
public:
    explicit VST3ProcessorComponentBase(FUID controllerId);
    ~VST3ProcessorComponentBase() override = default;

    tresult queryInterface(const FUID& targetIid, void** obj) override;
    uint32 addRef() override { return ++refCount_; }
    uint32 release() override;

    // Strict State Machine Transitions:
    // Uninitialized -> Initialized -> SetupDone -> Activated -> Processing
    tresult initialize(FUnknown* context) override;
    tresult terminate() override;
    tresult getControllerClassId(FUID& classId) const override;
    tresult setIoMode(int32 mode) override;
    [[nodiscard]] int32 getBusCount(MediaTypes type, BusDirections dir) const override;
    tresult getBusInfo(MediaTypes type, BusDirections dir, int32 index, BusInfo& bus) const override;
    tresult getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) const override;
    tresult activateBus(MediaTypes type, BusDirections dir, int32 index, bool state) override;
    tresult setActive(bool state) override;
    tresult setState(IBStream* state) override;
    tresult getState(IBStream* state) override;

    tresult setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                               SpeakerArrangement* outputs, int32 numOuts) override;
    tresult getBusArrangement(BusDirections dir, int32 index, SpeakerArrangement& arr) const override;
    tresult canProcessSampleSize(int32 symbolicSampleSize) const override;
    [[nodiscard]] uint32 getLatencySamples() const override { return latencySamples_; }
    tresult setupProcessing(ProcessSetup& setup) override;
    tresult setProcessing(bool state) override;
    tresult process(ProcessData& data) override;
    [[nodiscard]] uint32 getTailSamples() const override { return tailSamples_; }

    [[nodiscard]] VST3LifecycleState lifecycleState() const noexcept { return state_; }

    void addAudioInput(const char* name, SpeakerArrangement arr, BusTypes busType = kMain);
    void addAudioOutput(const char* name, SpeakerArrangement arr, BusTypes busType = kMain);
    void addEventInput(const char* name);
    void addEventOutput(const char* name);

protected:
    virtual void onSetupProcessing(const ProcessSetup& /*setup*/) {}
    virtual void onFlushParameterChange(ParamID /*id*/, ParamValue /*normalized*/) noexcept {}
    virtual void onProcessAudio32(ProcessData& data) noexcept;
    virtual void onProcessAudio64(ProcessData& data) noexcept;

    struct InternalBus {
        BusInfo info{};
        SpeakerArrangement arrangement{SpeakerArr::kStereo};
        bool active{true};
    };

    std::atomic<uint32> refCount_{1};
    FUID controllerClassId_{};
    VST3LifecycleState state_{VST3LifecycleState::Uninitialized};
    int32 ioMode_{kSimple};
    ProcessSetup currentSetup_{};
    uint32 latencySamples_{0};
    uint32 tailSamples_{0};
    std::vector<InternalBus> audioInputs_;
    std::vector<InternalBus> audioOutputs_;
    std::vector<InternalBus> eventInputs_;
    std::vector<InternalBus> eventOutputs_;
};

class VST3EditControllerBase : public IEditController {
public:
    VST3EditControllerBase() = default;
    ~VST3EditControllerBase() override = default;

    tresult queryInterface(const FUID& targetIid, void** obj) override;
    uint32 addRef() override { return ++refCount_; }
    uint32 release() override;

    tresult initialize(FUnknown* context) override;
    tresult terminate() override;
    tresult setComponentState(IBStream* state) override;
    tresult setState(IBStream* state) override;
    tresult getState(IBStream* state) override;
    [[nodiscard]] int32 getParameterCount() const override;
    tresult getParameterInfo(int32 paramIndex, ParameterInfo& info) const override;
    tresult getParamStringByValue(ParamID id, ParamValue valueNormalized, char outString[128]) const override;
    tresult getParamValueByString(ParamID id, const char* string, ParamValue& valueNormalized) const override;
    [[nodiscard]] ParamValue normalizedParamToPlain(ParamID id, ParamValue valueNormalized) const override;
    [[nodiscard]] ParamValue plainParamToNormalized(ParamID id, ParamValue plainValue) const override;
    [[nodiscard]] ParamValue getParamNormalized(ParamID id) const override;
    tresult setParamNormalized(ParamID id, ParamValue value) override;
    tresult setComponentHandler(IComponentHandler* handler) override;

    void registerParameter(ParamID id, const char* title, const char* units,
                           ParamValue minPlain, ParamValue maxPlain,
                           ParamValue defaultPlain, int32 flags = ParameterInfo::kCanAutomate);

protected:
    struct RegisteredParam {
        ParameterInfo info{};
        ParamValue minPlain{0.0};
        ParamValue maxPlain{1.0};
        ParamValue currentNormalized{0.0};
    };

    std::atomic<uint32> refCount_{1};
    bool initialized_{false};
    IComponentHandler* handler_{nullptr};
    std::vector<RegisteredParam> parameters_;
};

} // namespace AlphaStudio::SDK::VST3
