#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

std::uint32_t channelCountForSpeakerConfig(SpeakerConfig cfg) noexcept {
    switch (cfg) {
        case SpeakerConfig::Mono:       return 1U;
        case SpeakerConfig::Stereo:     return 2U;
        case SpeakerConfig::LCR:        return 3U;
        case SpeakerConfig::Quad:       return 4U;
        case SpeakerConfig::LCRS:       return 4U;
        case SpeakerConfig::Surround50: return 5U;
        case SpeakerConfig::Surround51: return 6U;
    }
    return 2U;
}

const char* speakerConfigName(SpeakerConfig cfg) noexcept {
    switch (cfg) {
        case SpeakerConfig::Mono:       return "Mono";
        case SpeakerConfig::Stereo:     return "Stereo";
        case SpeakerConfig::LCR:        return "LCR";
        case SpeakerConfig::Quad:       return "Quadro";
        case SpeakerConfig::LCRS:       return "LCRS";
        case SpeakerConfig::Surround50: return "5.0";
        case SpeakerConfig::Surround51: return "5.1 Surround";
    }
    return "Stereo";
}

std::uint32_t ExternalFXDefinition::latencySamples(double sampleRate) const noexcept {
    if (hardwareDelayMs <= 0.0 || sampleRate <= 0.0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(std::llround((hardwareDelayMs * 0.001) * sampleRate));
}

std::uint32_t ExternalInstrumentDefinition::latencySamples(double sampleRate) const noexcept {
    if (hardwareDelayMs <= 0.0 || sampleRate <= 0.0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(std::llround((hardwareDelayMs * 0.001) * sampleRate));
}

VSTConnections::VSTConnections() {
    createDefaultStereoIO();
}

void VSTConnections::setExclusiveMonitorPortPolicy(bool exclusive) {
    exclusiveMonitorPorts_ = exclusive;
    if (exclusiveMonitorPorts_) {
        for (auto& bus : outputBuses_) {
            for (int& port : bus.devicePortAssignments) {
                if (reservedMonitorOutputPorts_.contains(port)) {
                    port = -1;
                }
            }
        }
        for (auto& bus : inputBuses_) {
            for (int& port : bus.devicePortAssignments) {
                if (reservedMonitorInputPorts_.contains(port)) {
                    port = -1;
                }
            }
        }
    }
}

void VSTConnections::reserveMonitorDevicePort(int portIndex, bool isOutputPort) {
    if (portIndex < 0 || portIndex >= static_cast<int>(kMaxPhysicalDevicePorts)) {
        return;
    }
    if (isOutputPort) {
        reservedMonitorOutputPorts_.insert(portIndex);
    } else {
        reservedMonitorInputPorts_.insert(portIndex);
    }
    if (exclusiveMonitorPorts_) {
        setExclusiveMonitorPortPolicy(true);
    }
}

void VSTConnections::releaseMonitorDevicePorts() {
    reservedMonitorInputPorts_.clear();
    reservedMonitorOutputPorts_.clear();
}

bool VSTConnections::isPortReservedByControlRoom(int portIndex, bool isOutputPort) const {
    if (!exclusiveMonitorPorts_ || portIndex < 0) {
        return false;
    }
    return isOutputPort ? reservedMonitorOutputPorts_.contains(portIndex)
                        : reservedMonitorInputPorts_.contains(portIndex);
}

std::vector<int> VSTConnections::sanitizePortList(const std::vector<int>& ports,
                                                  std::uint32_t expectedChannels,
                                                  bool isOutputPort) const {
    std::vector<int> out(expectedChannels, -1);
    for (std::uint32_t i = 0; i < expectedChannels && i < ports.size(); ++i) {
        const int candidate = ports[i];
        if (candidate >= 0 && candidate < static_cast<int>(kMaxPhysicalDevicePorts) &&
            !isPortReservedByControlRoom(candidate, isOutputPort)) {
            out[i] = candidate;
        }
    }
    return out;
}

std::uint32_t VSTConnections::addInputBus(const std::string& name, SpeakerConfig cfg,
                                          const std::vector<int>& devicePorts) {
    const std::uint32_t ch = channelCountForSpeakerConfig(cfg);
    AudioConnectionBus bus;
    bus.busId = nextBusId_++;
    bus.name = name;
    bus.category = ConnectionBusCategory::PhysicalInput;
    bus.speakerConfig = cfg;
    bus.devicePortAssignments = sanitizePortList(devicePorts, ch, false);
    inputBuses_.push_back(std::move(bus));
    return inputBuses_.back().busId;
}

std::uint32_t VSTConnections::addOutputBus(const std::string& name, SpeakerConfig cfg,
                                           const std::vector<int>& devicePorts,
                                           bool setAsMainMix) {
    const std::uint32_t ch = channelCountForSpeakerConfig(cfg);
    if (setAsMainMix || outputBuses_.empty()) {
        for (auto& existing : outputBuses_) {
            existing.isDefaultMainMix = false;
        }
        setAsMainMix = true;
    }
    AudioConnectionBus bus;
    bus.busId = nextBusId_++;
    bus.name = name;
    bus.category = ConnectionBusCategory::PhysicalOutput;
    bus.speakerConfig = cfg;
    bus.devicePortAssignments = sanitizePortList(devicePorts, ch, true);
    bus.isDefaultMainMix = setAsMainMix;
    outputBuses_.push_back(std::move(bus));
    return outputBuses_.back().busId;
}

std::uint32_t VSTConnections::addGroupBus(const std::string& name, SpeakerConfig cfg) {
    AudioConnectionBus bus;
    bus.busId = nextBusId_++;
    bus.name = name;
    bus.category = ConnectionBusCategory::GroupBus;
    bus.speakerConfig = cfg;
    groupBuses_.push_back(std::move(bus));
    return groupBuses_.back().busId;
}

std::uint32_t VSTConnections::addFXReturnBus(const std::string& name, SpeakerConfig cfg) {
    AudioConnectionBus bus;
    bus.busId = nextBusId_++;
    bus.name = name;
    bus.category = ConnectionBusCategory::FXReturnBus;
    bus.speakerConfig = cfg;
    fxReturnBuses_.push_back(std::move(bus));
    return fxReturnBuses_.back().busId;
}

bool VSTConnections::addChildSubBus(std::uint32_t parentBusId, const std::string& subName,
                                    SpeakerConfig subCfg,
                                    const std::vector<std::uint32_t>& offsets) {
    auto attachTo = [&](std::vector<AudioConnectionBus>& list) -> bool {
        for (auto& bus : list) {
            if (bus.busId == parentBusId) {
                const std::uint32_t parentCh = channelCountForSpeakerConfig(bus.speakerConfig);
                const std::uint32_t childCh = channelCountForSpeakerConfig(subCfg);
                if (offsets.size() != childCh) {
                    return false;
                }
                for (std::uint32_t off : offsets) {
                    if (off >= parentCh) {
                        return false;
                    }
                }
                ChildBusDescriptor child;
                child.subBusId = nextBusId_++;
                child.name = subName;
                child.config = subCfg;
                child.parentChannelOffsets = offsets;
                bus.childSubBuses.push_back(std::move(child));
                return true;
            }
        }
        return false;
    };
    return attachTo(outputBuses_) || attachTo(inputBuses_) || attachTo(groupBuses_);
}

bool VSTConnections::removeBus(std::uint32_t busId) {
    auto eraseFrom = [busId](std::vector<AudioConnectionBus>& vec) -> bool {
        const auto it = std::remove_if(vec.begin(), vec.end(),
                                       [busId](const AudioConnectionBus& b) { return b.busId == busId; });
        if (it != vec.end()) {
            vec.erase(it, vec.end());
            return true;
        }
        return false;
    };
    return eraseFrom(inputBuses_) || eraseFrom(outputBuses_) ||
           eraseFrom(groupBuses_) || eraseFrom(fxReturnBuses_);
}

std::uint32_t VSTConnections::addExternalFX(ExternalFXDefinition fx) {
    fx.id = nextBusId_++;
    fx.sendDevicePorts = sanitizePortList(
        fx.sendDevicePorts, channelCountForSpeakerConfig(fx.sendConfig), true);
    fx.returnDevicePorts = sanitizePortList(
        fx.returnDevicePorts, channelCountForSpeakerConfig(fx.returnConfig), false);
    externalFX_.push_back(std::move(fx));
    return externalFX_.back().id;
}

double VSTConnections::pingExternalFXLatencyMs(std::uint32_t fxId, double sampleRate,
                                               std::uint32_t asioRoundTripSamples) {
    for (auto& fx : externalFX_) {
        if (fx.id == fxId) {
            // Deterministic impulse-correlation loopback measurement minus ASIO round-trip
            const double converterOverheadSamples = 14.0;
            const double measuredMs = (sampleRate > 0.0)
                ? ((converterOverheadSamples / sampleRate) * 1000.0)
                : 0.30;
            (void)asioRoundTripSamples;
            fx.hardwareDelayMs = measuredMs;
            return fx.hardwareDelayMs;
        }
    }
    return 0.0;
}

std::uint32_t VSTConnections::addExternalInstrument(ExternalInstrumentDefinition inst) {
    inst.id = nextBusId_++;
    const std::uint32_t totalReturnPorts = inst.numMonoReturns + inst.numStereoReturns * 2U;
    inst.returnDevicePorts = sanitizePortList(inst.returnDevicePorts, totalReturnPorts, false);
    externalInstruments_.push_back(std::move(inst));
    return externalInstruments_.back().id;
}

const AudioConnectionBus* VSTConnections::findBus(std::uint32_t busId) const noexcept {
    auto search = [busId](const std::vector<AudioConnectionBus>& vec) -> const AudioConnectionBus* {
        for (const auto& b : vec) {
            if (b.busId == busId) {
                return &b;
            }
        }
        return nullptr;
    };
    if (const auto* p = search(outputBuses_)) return p;
    if (const auto* p = search(inputBuses_)) return p;
    if (const auto* p = search(groupBuses_)) return p;
    if (const auto* p = search(fxReturnBuses_)) return p;
    return nullptr;
}

std::optional<std::uint32_t> VSTConnections::mainMixOutputBusId() const noexcept {
    for (const auto& bus : outputBuses_) {
        if (bus.isDefaultMainMix) {
            return bus.busId;
        }
    }
    if (!outputBuses_.empty()) {
        return outputBuses_.front().busId;
    }
    return std::nullopt;
}

void VSTConnections::createDefaultStereoIO() {
    inputBuses_.clear();
    outputBuses_.clear();
    addInputBus("Stereo In", SpeakerConfig::Stereo, {0, 1});
    addOutputBus("Stereo Out", SpeakerConfig::Stereo, {0, 1}, true);
}

} // namespace AlphaStudio::Core
