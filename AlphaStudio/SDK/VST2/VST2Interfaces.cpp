#include "AlphaStudio/SDK/VST2/VST2Interfaces.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace AlphaStudio::SDK::VST2 {

// ============================================================================
// AudioEffect (VST 2.4 Base)
// ============================================================================
AudioEffect::AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
    : audioMaster_(audioMaster),
      normalizedParams_(static_cast<std::size_t>(std::max<VstInt32>(0, numParams)), 0.0f) {
    cEffect_.magic = kEffectMagic;
    cEffect_.dispatcher = &AudioEffect::dispatchEffectClass;
    cEffect_.process = &AudioEffect::processClassReplacing;
    cEffect_.setParameter = &AudioEffect::setParameterClass;
    cEffect_.getParameter = &AudioEffect::getParameterClass;
    cEffect_.numPrograms = std::max<VstInt32>(1, numPrograms);
    cEffect_.numParams = std::max<VstInt32>(0, numParams);
    cEffect_.numInputs = 2;
    cEffect_.numOutputs = 2;
    cEffect_.flags = effFlagsCanReplacing;
    cEffect_.ioRatio = 1.0f;
    cEffect_.object = this;
    cEffect_.uniqueID = 0x41535632;
    cEffect_.version = 2400;
    cEffect_.processReplacing = &AudioEffect::processClassReplacing;
    cEffect_.processDoubleReplacing = &AudioEffect::processClassDoubleReplacing;
}

VstIntPtr AudioEffect::dispatchEffectClass(AEffect* e, VstInt32 opcode, VstInt32 index,
                                           VstIntPtr value, void* ptr, float opt) {
    if (e == nullptr || e->object == nullptr) {
        return 0;
    }
    auto* ae = static_cast<AudioEffect*>(e->object);
    if (opcode == effClose) {
        ae->dispatcher(opcode, index, value, ptr, opt);
        e->object = nullptr;
        delete ae; // Explicitly release heap allocation on effClose
        return 1;
    }
    return ae->dispatcher(opcode, index, value, ptr, opt);
}

void AudioEffect::setParameterClass(AEffect* e, VstInt32 index, float value) {
    if (e != nullptr && e->object != nullptr) {
        static_cast<AudioEffect*>(e->object)->setParameter(index, std::clamp(value, 0.0f, 1.0f));
    }
}

float AudioEffect::getParameterClass(AEffect* e, VstInt32 index) {
    if (e != nullptr && e->object != nullptr) {
        return std::clamp(static_cast<AudioEffect*>(e->object)->getParameter(index), 0.0f, 1.0f);
    }
    return 0.0f;
}

void AudioEffect::processClassReplacing(AEffect* e, float** inputs, float** outputs, VstInt32 sampleFrames) {
    if (e != nullptr && e->object != nullptr && sampleFrames >= 0) {
        static_cast<AudioEffect*>(e->object)->processReplacing(inputs, outputs, sampleFrames);
    }
}

void AudioEffect::processClassDoubleReplacing(AEffect* e, double** inputs, double** outputs, VstInt32 sampleFrames) {
    if (e != nullptr && e->object != nullptr && sampleFrames >= 0) {
        static_cast<AudioEffect*>(e->object)->processDoubleReplacing(inputs, outputs, sampleFrames);
    }
}

VstIntPtr AudioEffect::dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
    switch (opcode) {
        case effOpen:
            open();
            return 0;
        case effClose:
            close();
            return 1;
        case effSetProgram:
            if (value >= 0 && value < cEffect_.numPrograms) {
                setProgram(static_cast<VstInt32>(value));
            }
            return 0;
        case effGetProgram:
            return getProgram();
        case effSetProgramName:
            if (ptr != nullptr) {
                setProgramName(static_cast<const char*>(ptr));
            }
            return 0;
        case effGetProgramName:
            if (ptr != nullptr) {
                getProgramName(static_cast<char*>(ptr));
            }
            return 0;
        case effGetParamLabel:
            if (ptr != nullptr) {
                getParameterLabel(index, static_cast<char*>(ptr));
            }
            return 0;
        case effGetParamDisplay:
            if (ptr != nullptr) {
                getParameterDisplay(index, static_cast<char*>(ptr));
            }
            return 0;
        case effGetParamName:
            if (ptr != nullptr) {
                getParameterName(index, static_cast<char*>(ptr));
            }
            return 0;
        case effSetSampleRate:
            setSampleRate(opt);
            return 0;
        case effSetBlockSize:
            setBlockSize(static_cast<VstInt32>(value));
            return 0;
        case effMainsChanged:
            if (value == 0) {
                suspend();
            } else {
                resume();
            }
            return 0;
        default:
            return 0;
    }
}

void AudioEffect::setParameter(VstInt32 index, float value) {
    if (index >= 0 && static_cast<std::size_t>(index) < normalizedParams_.size()) {
        normalizedParams_[static_cast<std::size_t>(index)] = std::clamp(value, 0.0f, 1.0f);
    }
}

float AudioEffect::getParameter(VstInt32 index) const {
    if (index >= 0 && static_cast<std::size_t>(index) < normalizedParams_.size()) {
        return normalizedParams_[static_cast<std::size_t>(index)];
    }
    return 0.0f;
}

void AudioEffect::setParameterAutomated(VstInt32 index, float value) {
    setParameter(index, value);
    if (audioMaster_ != nullptr) {
        audioMaster_(&cEffect_, audioMasterAutomate, index, 0, nullptr, std::clamp(value, 0.0f, 1.0f));
    }
}

void AudioEffect::getParameterName(VstInt32 index, char* label) const {
    if (label != nullptr) {
        std::snprintf(label, 32, "Param %d", index + 1);
    }
}

void AudioEffect::getParameterDisplay(VstInt32 index, char* text) const {
    if (text != nullptr) {
        std::snprintf(text, 32, "%.3f", getParameter(index));
    }
}

void AudioEffect::getParameterLabel(VstInt32 /*index*/, char* label) const {
    if (label != nullptr) {
        std::snprintf(label, 32, "%s", "");
    }
}

void AudioEffect::setProgramName(const char* name) {
    if (name != nullptr) {
        programName_ = name;
    }
}

void AudioEffect::getProgramName(char* name) const {
    if (name != nullptr) {
        std::snprintf(name, 24, "%s", programName_.c_str());
    }
}

void AudioEffect::processDoubleReplacing(double** inputs, double** outputs, VstInt32 sampleFrames) {
    if (outputs == nullptr || sampleFrames <= 0) {
        return;
    }
    for (VstInt32 c = 0; c < cEffect_.numOutputs; ++c) {
        if (outputs[c] == nullptr) {
            continue;
        }
        if (inputs != nullptr && c < cEffect_.numInputs && inputs[c] != nullptr) {
            if (inputs[c] != outputs[c]) {
                std::memmove(outputs[c], inputs[c], static_cast<std::size_t>(sampleFrames) * sizeof(double));
            }
        } else {
            std::memset(outputs[c], 0, static_cast<std::size_t>(sampleFrames) * sizeof(double));
        }
    }
}

void AudioEffect::canProcessReplacing(bool state) noexcept {
    if (state) {
        cEffect_.flags |= effFlagsCanReplacing;
    } else {
        cEffect_.flags &= ~effFlagsCanReplacing;
    }
}

void AudioEffect::canDoubleReplacing(bool state) noexcept {
    if (state) {
        cEffect_.flags |= effFlagsCanDoubleReplacing;
    } else {
        cEffect_.flags &= ~effFlagsCanDoubleReplacing;
    }
}

// ============================================================================
// AudioEffectX (VST 2.4 Extended)
// ============================================================================
AudioEffectX::AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
    : AudioEffect(audioMaster, numPrograms, numParams) {}

VstIntPtr AudioEffectX::dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
    switch (opcode) {
        case effProcessEvents:
            return (ptr != nullptr) ? processEvents(static_cast<VstEvents*>(ptr)) : 0;
        case effCanBeAutomated:
            return (index >= 0 && index < cEffect_.numParams) ? 1 : 0;
        case effGetEffectName:
            return (ptr != nullptr && getEffectName(static_cast<char*>(ptr))) ? 1 : 0;
        case effGetVendorString:
            return (ptr != nullptr && getVendorString(static_cast<char*>(ptr))) ? 1 : 0;
        case effGetProductString:
            return (ptr != nullptr && getProductString(static_cast<char*>(ptr))) ? 1 : 0;
        case effGetVendorVersion:
            return 2400;
        case effCanDo:
            return (ptr != nullptr) ? canDo(static_cast<const char*>(ptr)) : 0;
        case effStartProcess:
            return startProcess();
        case effStopProcess:
            return stopProcess();
        default:
            return AudioEffect::dispatcher(opcode, index, value, ptr, opt);
    }
}

VstInt32 AudioEffectX::processEvents(VstEvents* /*events*/) {
    return 1;
}

bool AudioEffectX::sendVstEventsToHost(VstEvents* events) {
    if (audioMaster_ == nullptr || events == nullptr) {
        return false;
    }
    return audioMaster_(&cEffect_, audioMasterProcessEvents, 0, 0, events, 0.0f) == 1;
}

void AudioEffectX::isSynth(bool state) noexcept {
    if (state) {
        cEffect_.flags |= effFlagsIsSynth;
    } else {
        cEffect_.flags &= ~effFlagsIsSynth;
    }
}

VstInt32 AudioEffectX::canDo(const char* text) const {
    if (text == nullptr) {
        return 0;
    }
    if (std::strcmp(text, "receiveVstEvents") == 0 ||
        std::strcmp(text, "receiveVstMidiEvent") == 0 ||
        std::strcmp(text, "sendVstEvents") == 0 ||
        std::strcmp(text, "sendVstMidiEvent") == 0 ||
        std::strcmp(text, "receiveVstTimeInfo") == 0) {
        return 1;
    }
    return -1;
}

bool AudioEffectX::getEffectName(char* name) const {
    if (name == nullptr) return false;
    std::snprintf(name, 32, "%s", "AlphaStudio VST2.4 Plug-in");
    return true;
}

bool AudioEffectX::getVendorString(char* text) const {
    if (text == nullptr) return false;
    std::snprintf(text, 64, "%s", "AlphaStudio Clean-Room Audio");
    return true;
}

bool AudioEffectX::getProductString(char* text) const {
    if (text == nullptr) return false;
    std::snprintf(text, 64, "%s", "AlphaStudio DSP");
    return true;
}

// ============================================================================
// VSTXmlDescriptor (.vstxml)
// ============================================================================
std::string VSTXmlDescriptor::serializeToXml(const std::string& pluginName, VstInt32 uniqueId,
                                             const std::vector<VSTXmlParamNode>& params) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<VSTParametersStructure>\n";
    oss << "  <Plugin name=\"" << pluginName << "\" uniqueID=\"" << uniqueId << "\">\n";
    oss << "    <Params>\n";
    for (const auto& p : params) {
        oss << "      <Param id=\"" << p.id
            << "\" name=\"" << p.name
            << "\" shortName=\"" << p.shortName
            << "\" label=\"" << p.label
            << "\" group=\"" << p.groupName << "\"/>\n";
    }
    oss << "    </Params>\n";
    oss << "  </Plugin>\n";
    oss << "</VSTParametersStructure>\n";
    return oss.str();
}

std::vector<VSTXmlParamNode> VSTXmlDescriptor::parseFromXml(const std::string& xmlText) {
    std::vector<VSTXmlParamNode> out;
    auto extractAttr = [](const std::string& tag, const char* attrName) -> std::string {
        const std::string needle = std::string(attrName) + "=\"";
        const std::size_t p0 = tag.find(needle);
        if (p0 == std::string::npos) return {};
        const std::size_t start = p0 + needle.size();
        const std::size_t end = tag.find('"', start);
        return (end != std::string::npos) ? tag.substr(start, end - start) : std::string{};
    };

    std::size_t pos = 0;
    while (true) {
        const std::size_t tagStart = xmlText.find("<Param ", pos);
        if (tagStart == std::string::npos) break;
        const std::size_t tagEnd = xmlText.find("/>", tagStart);
        if (tagEnd == std::string::npos) break;
        const std::string tag = xmlText.substr(tagStart, tagEnd - tagStart);

        VSTXmlParamNode node;
        const std::string idStr = extractAttr(tag, "id");
        node.id = idStr.empty() ? 0 : static_cast<VstInt32>(std::strtol(idStr.c_str(), nullptr, 10));
        node.name = extractAttr(tag, "name");
        node.shortName = extractAttr(tag, "shortName");
        node.label = extractAttr(tag, "label");
        node.groupName = extractAttr(tag, "group");
        out.push_back(std::move(node));
        pos = tagEnd + 2;
    }
    return out;
}

// ============================================================================
// VST2HostSlot & Entry Point
// ============================================================================
VST2HostSlot::VST2HostSlot(AudioEffectX* ownedPluginInstance) {
    if (ownedPluginInstance != nullptr) {
        effect_ = ownedPluginInstance->getAeffect();
        if (effect_ != nullptr && effect_->dispatcher != nullptr) {
            effect_->dispatcher(effect_, effOpen, 0, 0, nullptr, 0.0f);
        }
    }
}

VST2HostSlot::~VST2HostSlot() {
    if (effect_ != nullptr && effect_->dispatcher != nullptr) {
        effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
        effect_->dispatcher(effect_, effClose, 0, 0, nullptr, 0.0f); // Frees heap instance
        effect_ = nullptr;
    }
}

void VST2HostSlot::prepare(float sampleRate, VstInt32 blockSize) {
    if (effect_ == nullptr || effect_->dispatcher == nullptr) {
        return;
    }
    effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
    effect_->dispatcher(effect_, effSetSampleRate, 0, 0, nullptr, sampleRate);
    effect_->dispatcher(effect_, effSetBlockSize, 0, blockSize, nullptr, 0.0f);
    effect_->dispatcher(effect_, effMainsChanged, 0, 1, nullptr, 0.0f);
    effect_->dispatcher(effect_, effStartProcess, 0, 0, nullptr, 0.0f);
}

void VST2HostSlot::processBlockRT(VstEvents* midiEvents, float** inputs,
                                  float** outputs, VstInt32 sampleFrames) noexcept {
    if (effect_ == nullptr) {
        return;
    }
    // 1. Always dispatch MIDI/VstEvents strictly BEFORE processReplacing
    if (midiEvents != nullptr && midiEvents->numEvents > 0 && effect_->dispatcher != nullptr) {
        effect_->dispatcher(effect_, effProcessEvents, 0, 0, midiEvents, 0.0f);
    }
    // 2. Process 32-bit float audio block
    if (effect_->processReplacing != nullptr && sampleFrames > 0) {
        effect_->processReplacing(effect_, inputs, outputs, sampleFrames);
    }
}

AEffect* VSTPluginMainEntry(audioMasterCallback audioMaster, CreateEffectInstanceFn factory) {
    if (audioMaster == nullptr || factory == nullptr) {
        return nullptr;
    }
    if (audioMaster(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) {
        return nullptr;
    }
    AudioEffect* effect = factory(audioMaster);
    return (effect != nullptr) ? effect->getAeffect() : nullptr;
}

} // namespace AlphaStudio::SDK::VST2
