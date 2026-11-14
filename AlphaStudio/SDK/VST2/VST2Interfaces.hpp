#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AlphaStudio::SDK::VST2 {

// ============================================================================
// Clean-Room VST 2.4 Backward-Compatibility C ABI & C++ Hierarchy
// ============================================================================
using VstInt32  = std::int32_t;
using VstIntPtr = std::intptr_t;

inline constexpr VstInt32 kEffectMagic = 0x56737450; // 'VstP'

struct AEffect;

using audioMasterCallback = VstIntPtr (*)(AEffect* effect, VstInt32 opcode,
                                          VstInt32 index, VstIntPtr value,
                                          void* ptr, float opt);
using AEffectDispatcherProc = VstIntPtr (*)(AEffect* effect, VstInt32 opcode,
                                            VstInt32 index, VstIntPtr value,
                                            void* ptr, float opt);
using AEffectProcessProc = void (*)(AEffect* effect, float** inputs,
                                    float** outputs, VstInt32 sampleFrames);
using AEffectProcessDoubleProc = void (*)(AEffect* effect, double** inputs,
                                          double** outputs, VstInt32 sampleFrames);
using AEffectSetParameterProc = void (*)(AEffect* effect, VstInt32 index, float parameter);
using AEffectGetParameterProc = float (*)(AEffect* effect, VstInt32 index);

enum AEffectOpcodes : VstInt32 {
    effOpen              = 0,
    effClose             = 1, // Deletes AudioEffect object, freeing heap
    effSetProgram        = 2,
    effGetProgram        = 3,
    effSetProgramName    = 4,
    effGetProgramName    = 5,
    effGetParamLabel     = 6,
    effGetParamDisplay   = 7,
    effGetParamName      = 8,
    effSetSampleRate     = 10,
    effSetBlockSize      = 11,
    effMainsChanged      = 12,
    effGetChunk          = 23,
    effSetChunk          = 24,
    effProcessEvents     = 25,
    effCanBeAutomated    = 26,
    effGetPlugCategory   = 35,
    effGetEffectName     = 45,
    effGetVendorString   = 47,
    effGetProductString  = 48,
    effGetVendorVersion  = 49,
    effCanDo             = 51,
    effStartProcess      = 71,
    effStopProcess       = 72
};

enum AudioMasterOpcodes : VstInt32 {
    audioMasterAutomate        = 0,
    audioMasterVersion         = 1,
    audioMasterCurrentId       = 2,
    audioMasterIdle            = 3,
    audioMasterGetTime         = 7,
    audioMasterProcessEvents   = 8, // Used by sendVstEventsToHost
    audioMasterIOChanged       = 13,
    audioMasterSizeWindow      = 15,
    audioMasterGetSampleRate   = 16,
    audioMasterGetBlockSize    = 17
};

enum VstAEffectFlags : VstInt32 {
    effFlagsHasEditor          = 1 << 0,
    effFlagsCanReplacing       = 1 << 4,
    effFlagsProgramChunks      = 1 << 5,
    effFlagsIsSynth            = 1 << 8,
    effFlagsCanDoubleReplacing = 1 << 12
};

enum VstEventTypes : VstInt32 {
    kVstMidiType  = 1,
    kVstSysExType = 6
};

struct VstEvent {
    VstInt32 type{kVstMidiType};
    VstInt32 byteSize{0};
    VstInt32 deltaFrames{0};
    VstInt32 flags{0};
    char data[16]{};
};

struct VstMidiEvent {
    VstInt32 type{kVstMidiType};
    VstInt32 byteSize{sizeof(VstMidiEvent)};
    VstInt32 deltaFrames{0};
    VstInt32 flags{0};
    VstInt32 noteLength{0};
    VstInt32 noteOffset{0};
    char midiData[4]{};
    char detune{0};
    char noteOffVelocity{0};
    char reserved1{0};
    char reserved2{0};
};

struct VstEvents {
    VstInt32 numEvents{0};
    VstIntPtr reserved{0};
    VstEvent* events[256]{};
};

struct AEffect {
    VstInt32 magic{kEffectMagic};
    AEffectDispatcherProc dispatcher{nullptr};
    AEffectProcessProc process{nullptr};
    AEffectSetParameterProc setParameter{nullptr};
    AEffectGetParameterProc getParameter{nullptr};
    VstInt32 numPrograms{1};
    VstInt32 numParams{0};
    VstInt32 numInputs{2};
    VstInt32 numOutputs{2};
    VstInt32 flags{effFlagsCanReplacing};
    VstIntPtr resvd1{0};
    VstIntPtr resvd2{0};
    VstInt32 initialDelay{0};
    VstInt32 realQualities{0};
    VstInt32 offQualities{0};
    float ioRatio{1.0f};
    void* object{nullptr};
    void* user{nullptr};
    VstInt32 uniqueID{0x41535632};
    VstInt32 version{2400};
    AEffectProcessProc processReplacing{nullptr};
    AEffectProcessDoubleProc processDoubleReplacing{nullptr};
    char future[56]{};
};

// ============================================================================
// C++ Hierarchy: AudioEffect -> AudioEffectX
// ============================================================================
class AudioEffect {
public:
    AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams);
    virtual ~AudioEffect() = default;

    [[nodiscard]] AEffect* getAeffect() noexcept { return &cEffect_; }
    [[nodiscard]] const AEffect* getAeffect() const noexcept { return &cEffect_; }

    virtual VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);
    virtual void open() {}
    virtual void close() {}
    virtual void suspend() {}
    virtual void resume() {}

    virtual void setSampleRate(float sampleRate) { sampleRate_ = sampleRate; }
    virtual void setBlockSize(VstInt32 blockSize) { blockSize_ = blockSize; }

    // Normalized 32-bit parameters in [0.0f, 1.0f]
    virtual void setParameter(VstInt32 index, float value);
    [[nodiscard]] virtual float getParameter(VstInt32 index) const;
    virtual void setParameterAutomated(VstInt32 index, float value);

    virtual void getParameterName(VstInt32 index, char* label) const;
    virtual void getParameterDisplay(VstInt32 index, char* text) const;
    virtual void getParameterLabel(VstInt32 index, char* label) const;

    virtual void setProgram(VstInt32 program) { curProgram_ = program; }
    [[nodiscard]] virtual VstInt32 getProgram() const { return curProgram_; }
    virtual void setProgramName(const char* name);
    virtual void getProgramName(char* name) const;

    virtual void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames) = 0;
    virtual void processDoubleReplacing(double** inputs, double** outputs, VstInt32 sampleFrames);

    void setNumInputs(VstInt32 inputs) noexcept { cEffect_.numInputs = inputs; }
    void setNumOutputs(VstInt32 outputs) noexcept { cEffect_.numOutputs = outputs; }
    void setUniqueID(VstInt32 id) noexcept { cEffect_.uniqueID = id; }
    void setInitialDelay(VstInt32 delaySamples) noexcept { cEffect_.initialDelay = delaySamples; }
    void canProcessReplacing(bool state = true) noexcept;
    void canDoubleReplacing(bool state = true) noexcept;

protected:
    static VstIntPtr dispatchEffectClass(AEffect* e, VstInt32 opcode, VstInt32 index,
                                         VstIntPtr value, void* ptr, float opt);
    static void setParameterClass(AEffect* e, VstInt32 index, float value);
    static float getParameterClass(AEffect* e, VstInt32 index);
    static void processClassReplacing(AEffect* e, float** inputs, float** outputs, VstInt32 sampleFrames);
    static void processClassDoubleReplacing(AEffect* e, double** inputs, double** outputs, VstInt32 sampleFrames);

    audioMasterCallback audioMaster_{nullptr};
    AEffect cEffect_{};
    float sampleRate_{48000.0f};
    VstInt32 blockSize_{256};
    VstInt32 curProgram_{0};
    std::string programName_{"Init"};
    std::vector<float> normalizedParams_;
};

class AudioEffectX : public AudioEffect {
public:
    AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams);
    ~AudioEffectX() override = default;

    VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) override;

    // Executed strictly BEFORE processReplacing in every audio block
    virtual VstInt32 processEvents(VstEvents* events);
    bool sendVstEventsToHost(VstEvents* events);

    void isSynth(bool state = true) noexcept;
    virtual VstInt32 canDo(const char* text) const;
    virtual bool getEffectName(char* name) const;
    virtual bool getVendorString(char* text) const;
    virtual bool getProductString(char* text) const;
    [[nodiscard]] virtual VstInt32 startProcess() { return 1; }
    [[nodiscard]] virtual VstInt32 stopProcess() { return 1; }
};

// ============================================================================
// VST 2.4 Host Slot Executor & .vstxml Descriptor Support
// ============================================================================
struct VSTXmlParamNode {
    VstInt32 id{0};
    std::string name;
    std::string shortName;
    std::string label;
    std::string groupName;
};

class VSTXmlDescriptor {
public:
    static std::string serializeToXml(const std::string& pluginName, VstInt32 uniqueId,
                                      const std::vector<VSTXmlParamNode>& params);
    static std::vector<VSTXmlParamNode> parseFromXml(const std::string& xmlText);
};

class VST2HostSlot {
public:
    explicit VST2HostSlot(AudioEffectX* ownedPluginInstance);
    ~VST2HostSlot();

    VST2HostSlot(const VST2HostSlot&) = delete;
    VST2HostSlot& operator=(const VST2HostSlot&) = delete;

    void prepare(float sampleRate, VstInt32 blockSize);
    // Enforces VST 2.4 contract: processEvents(events) strictly before processReplacing(...)
    void processBlockRT(VstEvents* midiEvents, float** inputs, float** outputs, VstInt32 sampleFrames) noexcept;

    [[nodiscard]] AEffect* aeffect() noexcept { return effect_; }

private:
    AEffect* effect_{nullptr};
};

// Standard VST 2.4 Factory & Entry Point Helper
using CreateEffectInstanceFn = AudioEffect* (*)(audioMasterCallback audioMaster);
AEffect* VSTPluginMainEntry(audioMasterCallback audioMaster, CreateEffectInstanceFn factory);

} // namespace AlphaStudio::SDK::VST2
