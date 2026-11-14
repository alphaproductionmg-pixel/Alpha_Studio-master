#include "AlphaStudio/Core/Sync/RemoteAndSync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <sstream>

namespace AlphaStudio::Core::Sync {

// ============================================================================
// 1. GenericRemoteEngine
// ============================================================================
void GenericRemoteEngine::addMappingPair(GenericRemoteUpperRow upper, GenericRemoteLowerRow lower) {
    upperGrid_.push_back(std::move(upper));
    lowerGrid_.push_back(std::move(lower));
}

void GenericRemoteEngine::clearMappings() {
    upperGrid_.clear();
    lowerGrid_.clear();
}

std::string GenericRemoteEngine::serializeToXml() const {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<remotedescription version=\"1.1\">\n";
    oss << "  <ctrltable name=\"Standard MIDI\">\n";
    for (const auto& u : upperGrid_) {
        oss << "    <ctrl name=\"" << u.controlName
            << "\" stat=\"" << static_cast<int>(u.midiStatus)
            << "\" chan=\"" << static_cast<int>(u.midiChannel)
            << "\" addr=\"" << static_cast<int>(u.ccAddress)
            << "\" max=\"" << u.maxValue
            << "\" flags=\"" << static_cast<int>(u.flags) << "\"/>\n";
    }
    oss << "  </ctrltable>\n";
    oss << "  <bank name=\"Main Bank\">\n";
    for (const auto& l : lowerGrid_) {
        oss << "    <entry ctrl=\"" << l.controlName
            << "\" device=\"" << l.deviceCategory
            << "\" chan=\"" << l.channelOrTrackIndex
            << "\" action=\"" << static_cast<int>(l.action)
            << "\" toggle=\"" << (l.toggleFlag ? 1 : 0) << "\"/>\n";
    }
    oss << "  </bank>\n";
    oss << "</remotedescription>\n";
    return oss.str();
}

bool GenericRemoteEngine::loadFromXml(const std::string& xmlContent) {
    if (xmlContent.find("<remotedescription") == std::string::npos) {
        return false;
    }
    clearMappings();

    auto attr = [](const std::string& line, const char* key) -> std::string {
        const std::string needle = std::string(key) + "=\"";
        const auto p0 = line.find(needle);
        if (p0 == std::string::npos) return {};
        const auto s = p0 + needle.size();
        const auto e = line.find('"', s);
        return (e != std::string::npos) ? line.substr(s, e - s) : std::string{};
    };

    std::istringstream iss(xmlContent);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("<ctrl ") != std::string::npos) {
            GenericRemoteUpperRow u;
            u.controlName = attr(line, "name");
            u.midiStatus = static_cast<std::uint8_t>(std::strtol(attr(line, "stat").c_str(), nullptr, 10));
            u.midiChannel = static_cast<std::uint8_t>(std::strtol(attr(line, "chan").c_str(), nullptr, 10));
            u.ccAddress = static_cast<std::uint8_t>(std::strtol(attr(line, "addr").c_str(), nullptr, 10));
            u.maxValue = static_cast<std::uint16_t>(std::max(1L, std::strtol(attr(line, "max").c_str(), nullptr, 10)));
            u.flags = static_cast<std::uint8_t>(std::strtol(attr(line, "flags").c_str(), nullptr, 10));
            upperGrid_.push_back(std::move(u));
        } else if (line.find("<entry ") != std::string::npos) {
            GenericRemoteLowerRow l;
            l.controlName = attr(line, "ctrl");
            l.deviceCategory = attr(line, "device");
            l.channelOrTrackIndex = static_cast<std::uint32_t>(std::strtoul(attr(line, "chan").c_str(), nullptr, 10));
            l.action = static_cast<GenericRemoteTargetAction>(std::strtol(attr(line, "action").c_str(), nullptr, 10));
            l.toggleFlag = (std::strtol(attr(line, "toggle").c_str(), nullptr, 10) != 0);
            lowerGrid_.push_back(std::move(l));
        }
    }
    return !upperGrid_.empty() && upperGrid_.size() == lowerGrid_.size();
}

bool GenericRemoteEngine::processIncomingMidiMessage(
    std::uint8_t status, std::uint8_t data1, std::uint8_t data2,
    StudioTransportState& transport,
    std::vector<std::unique_ptr<TrackBase>>& tracks,
    VSTMixer& mixer) {
    const std::uint8_t statusHi = status & 0xF0U;
    const std::uint8_t ch = status & 0x0FU;
    bool handled = false;
    const std::size_t count = std::min(upperGrid_.size(), lowerGrid_.size());

    for (std::size_t i = 0; i < count; ++i) {
        const auto& u = upperGrid_[i];
        if ((u.flags & kGRFlagReceive) == 0U) continue;
        if ((u.midiStatus & 0xF0U) != statusHi || u.midiChannel != ch || u.ccAddress != data1) {
            continue;
        }

        const auto& l = lowerGrid_[i];
        const float norm = std::clamp(
            static_cast<float>(data2) / static_cast<float>(std::max<std::uint16_t>(1U, u.maxValue)),
            0.0f, 1.0f);

        switch (l.action) {
            case GenericRemoteTargetAction::TransportStart:
                if (data2 > 0) transport.isPlaying = true;
                break;
            case GenericRemoteTargetAction::TransportStop:
                if (data2 > 0) {
                    transport.isPlaying = false;
                    transport.isRecording = false;
                }
                break;
            case GenericRemoteTargetAction::TransportRecord:
                if (data2 > 0) {
                    transport.isRecording = l.toggleFlag ? !transport.isRecording : true;
                    if (transport.isRecording) transport.isPlaying = true;
                }
                break;
            case GenericRemoteTargetAction::SelectTrack:
                if (!tracks.empty()) {
                    transport.selectedTrackIndex = std::min<std::size_t>(
                        l.channelOrTrackIndex, tracks.size() - 1);
                }
                break;
            case GenericRemoteTargetAction::ToggleTrackMonitor:
                if (l.channelOrTrackIndex < tracks.size() && data2 > 0) {
                    auto& trk = *tracks[l.channelOrTrackIndex];
                    trk.setMonitorEnabled(l.toggleFlag ? !trk.isMonitorEnabled() : (norm >= 0.5f));
                }
                break;
            case GenericRemoteTargetAction::ToggleTrackRecArm:
                if (l.channelOrTrackIndex < tracks.size() && data2 > 0) {
                    auto& trk = *tracks[l.channelOrTrackIndex];
                    trk.setRecordArmed(l.toggleFlag ? !trk.isRecordArmed() : (norm >= 0.5f));
                }
                break;
            case GenericRemoteTargetAction::ConsoleChannelFader:
                if (l.channelOrTrackIndex < mixer.channels().size()) {
                    mixer.channels()[l.channelOrTrackIndex]->setFaderDb((norm * 66.0f) - 60.0f);
                }
                break;
            case GenericRemoteTargetAction::ConsoleChannelPan:
                if (l.channelOrTrackIndex < mixer.channels().size()) {
                    mixer.channels()[l.channelOrTrackIndex]->panner().setPan(norm * 2.0f - 1.0f);
                }
                break;
        }
        handled = true;
    }
    return handled;
}

// ============================================================================
// 2. MMCAndMidiClockEngine
// ============================================================================
std::vector<std::uint8_t> MMCAndMidiClockEngine::buildMMCStop() const {
    return {0xF0, 0x7F, setup_.mmcDeviceId, 0x06, 0x01, 0xF7};
}

std::vector<std::uint8_t> MMCAndMidiClockEngine::buildMMCDeferredPlay() const {
    return {0xF0, 0x7F, setup_.mmcDeviceId, 0x06, 0x03, 0xF7};
}

std::vector<std::uint8_t> MMCAndMidiClockEngine::buildMMCRecordStrobe() const {
    return {0xF0, 0x7F, setup_.mmcDeviceId, 0x06, 0x06, 0xF7};
}

std::vector<std::uint8_t> MMCAndMidiClockEngine::buildMMCLocateSamples(
    std::int64_t samplePos, double sampleRate) const {
    const double totalSec = (sampleRate > 0.0) ? (static_cast<double>(std::max<std::int64_t>(0, samplePos)) / sampleRate) : 0.0;
    const auto hours = static_cast<std::uint8_t>(static_cast<int>(totalSec / 3600.0) & 0x1F);
    const auto mins  = static_cast<std::uint8_t>((static_cast<int>(totalSec) / 60) % 60);
    const auto secs  = static_cast<std::uint8_t>(static_cast<int>(totalSec) % 60);
    const auto frames = static_cast<std::uint8_t>(
        std::fmod(totalSec * std::max(1.0, setup_.frameRateFps), std::max(1.0, setup_.frameRateFps)));
    return {0xF0, 0x7F, setup_.mmcDeviceId, 0x06, 0x44, 0x06, 0x01, hours, mins, secs, frames, 0x00, 0xF7};
}

bool MMCAndMidiClockEngine::handleIncomingMMCSysEx(
    std::span<const std::uint8_t> sysexBytes,
    double sampleRate,
    StudioTransportState& transport) const noexcept {
    if (!setup_.mmcSlaveActive || sysexBytes.size() < 6) {
        return false;
    }
    if (sysexBytes[0] != 0xF0 || sysexBytes[1] != 0x7F || sysexBytes[3] != 0x06) {
        return false;
    }
    const std::uint8_t dev = sysexBytes[2];
    if (dev != 0x7F && setup_.mmcDeviceId != 0x7F && dev != setup_.mmcDeviceId) {
        return false;
    }

    const std::uint8_t cmd = sysexBytes[4];
    switch (cmd) {
        case 0x01: // MMC Stop
            transport.isPlaying = false;
            transport.isRecording = false;
            return true;
        case 0x02: // MMC Play
        case 0x03: // MMC Deferred Play
            transport.isPlaying = true;
            return true;
        case 0x06: // MMC Record Strobe
            transport.isPlaying = true;
            transport.isRecording = true;
            return true;
        case 0x44: // MMC Locate (F0 7F dev 06 44 06 01 hr mn sc fr sf F7)
            if (sysexBytes.size() >= 13 && sysexBytes[6] == 0x01) {
                const double hr = static_cast<double>(sysexBytes[7] & 0x1F);
                const double mn = static_cast<double>(sysexBytes[8]);
                const double sc = static_cast<double>(sysexBytes[9]);
                const double fr = static_cast<double>(sysexBytes[10]);
                const double totalSec = hr * 3600.0 + mn * 60.0 + sc + (fr / std::max(1.0, setup_.frameRateFps));
                transport.currentSamplePosition = static_cast<std::int64_t>(std::llround(totalSec * sampleRate));
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

std::vector<std::uint8_t> MMCAndMidiClockEngine::generateMidiClockBytesForTickSpan(
    std::int64_t startTick480, std::int64_t endTick480, bool transportJustStarted) const {
    std::vector<std::uint8_t> out;
    if (!setup_.sendMidiClock) {
        return out;
    }
    if (transportJustStarted) {
        out.push_back(startTick480 == 0 ? 0xFA : 0xFB); // Start or Continue
    }
    // 24 MIDI Clocks per quarter note => 1 clock every (480 / 24) = 20 internal ticks
    constexpr std::int64_t kTicksPerMidiClock = 20;
    for (std::int64_t t = ((startTick480 + kTicksPerMidiClock - 1) / kTicksPerMidiClock) * kTicksPerMidiClock;
         t < endTick480; t += kTicksPerMidiClock) {
        out.push_back(0xF8);
    }
    return out;
}

// ============================================================================
// 3. VSTSystemLinkProcessor (LSB Bit-0 Digital Audio Sync Protocol)
// ============================================================================
std::uint8_t VSTSystemLinkProcessor::computeCrc4(std::uint64_t payload60Bits) noexcept {
    std::uint8_t crc = 0x0FU;
    for (int i = 0; i < 60; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>((payload60Bits >> i) & 1ULL);
        crc = static_cast<std::uint8_t>(((crc << 1U) ^ (bit ? 0x03U : 0x00U)) & 0x0FU);
    }
    return crc;
}

void VSTSystemLinkProcessor::encodePacketIntoAudioLSB(
    std::span<float> digitalAudioChannel,
    const SystemLinkPacket& packet) noexcept {
    if (!online_ || digitalAudioChannel.size() < kFrameBits) {
        return;
    }

    // Pack 64-bit System Link frame:
    // [11:0]  = 12-bit Sync Header (0xA5B)
    // [15:12] = 4-bit SystemLinkTransportCmd (Play, Stop, Record, Locate, FF, Rewind, JogShuttle)
    // [23:16] = 8-bit Signed Jog/Shuttle speed (-128..+127 => -8.0x .. +8.0x)
    // [31:24] = 8-bit MIDI Tunnel Byte
    // [59:32] = 28-bit Sample Position Counter
    // [63:60] = 4-bit CRC4 Checksum
    const auto jogQuant = static_cast<std::int8_t>(
        std::clamp(std::lround(packet.jogShuttleVelocity * 15.0f), -127L, 127L));
    std::uint64_t word60 = static_cast<std::uint64_t>(kSyncHeader12) |
                           (static_cast<std::uint64_t>(packet.command) & 0x0FULL) << 12U |
                           (static_cast<std::uint64_t>(static_cast<std::uint8_t>(jogQuant)) << 16U) |
                           (static_cast<std::uint64_t>(packet.midiTunnelByte) << 24U) |
                           ((packet.samplePosition & 0x0FFFFFFFULL) << 32U);
    const std::uint64_t fullFrame64 = word60 | (static_cast<std::uint64_t>(computeCrc4(word60)) << 60U);

    // Embed into Bit 0 (LSB) of 24-bit signed PCM carried exactly in IEEE-754 32-bit float mantissa!
    constexpr float kScale23 = 8388608.0f; // 2^23 (exact in 24-bit float significand)
    constexpr float kInvScale23 = 1.0f / kScale23;

    for (std::size_t b = 0; b < kFrameBits; ++b) {
        const std::int32_t bit = static_cast<std::int32_t>((fullFrame64 >> b) & 1ULL);
        auto pcm24 = static_cast<std::int32_t>(
            std::lround(std::clamp(digitalAudioChannel[b], -0.9999f, 0.9999f) * kScale23));
        pcm24 = (pcm24 & ~1) | bit; // Replace LSB (bit 0) with System Link sync bit
        digitalAudioChannel[b] = static_cast<float>(pcm24) * kInvScale23;
    }
}

bool VSTSystemLinkProcessor::decodePacketFromAudioLSB(
    std::span<const float> digitalAudioChannel,
    SystemLinkPacket& outPacket,
    StudioTransportState* applyToTransport) noexcept {
    if (!online_ || digitalAudioChannel.size() < kFrameBits) {
        return false;
    }

    constexpr float kScale23 = 8388608.0f;
    std::uint64_t fullFrame64 = 0;
    for (std::size_t b = 0; b < kFrameBits; ++b) {
        const auto pcm24 = static_cast<std::int32_t>(std::lround(digitalAudioChannel[b] * kScale23));
        if ((pcm24 & 1) != 0) {
            fullFrame64 |= (1ULL << b);
        }
    }

    const std::uint16_t sync = static_cast<std::uint16_t>(fullFrame64 & 0xFFFULL);
    if (sync != kSyncHeader12) {
        return false;
    }
    const std::uint64_t word60 = fullFrame64 & 0x0FFFFFFFFFFFFFFFULL;
    const std::uint8_t rxCrc = static_cast<std::uint8_t>((fullFrame64 >> 60U) & 0x0FULL);
    if (computeCrc4(word60) != rxCrc) {
        return false;
    }

    outPacket.command = static_cast<SystemLinkTransportCmd>((word60 >> 12U) & 0x0FULL);
    const auto jogQuant = static_cast<std::int8_t>(static_cast<std::uint8_t>((word60 >> 16U) & 0xFFULL));
    outPacket.jogShuttleVelocity = static_cast<float>(jogQuant) / 15.0f;
    outPacket.midiTunnelByte = static_cast<std::uint8_t>((word60 >> 24U) & 0xFFULL);
    outPacket.samplePosition = (word60 >> 32U) & 0x0FFFFFFFULL;

    if (applyToTransport != nullptr) {
        switch (outPacket.command) {
            case SystemLinkTransportCmd::Play:
                applyToTransport->isPlaying = true;
                applyToTransport->fastForwarding = false;
                applyToTransport->rewinding = false;
                break;
            case SystemLinkTransportCmd::Stop:
                applyToTransport->isPlaying = false;
                applyToTransport->isRecording = false;
                applyToTransport->fastForwarding = false;
                applyToTransport->rewinding = false;
                applyToTransport->jogShuttleSpeed = 0.0f;
                break;
            case SystemLinkTransportCmd::Record:
                applyToTransport->isPlaying = true;
                applyToTransport->isRecording = true;
                break;
            case SystemLinkTransportCmd::Locate:
                applyToTransport->currentSamplePosition = static_cast<std::int64_t>(outPacket.samplePosition);
                break;
            case SystemLinkTransportCmd::FastForward:
                applyToTransport->fastForwarding = true;
                applyToTransport->rewinding = false;
                break;
            case SystemLinkTransportCmd::Rewind:
                applyToTransport->rewinding = true;
                applyToTransport->fastForwarding = false;
                break;
            case SystemLinkTransportCmd::JogShuttle:
                applyToTransport->jogShuttleSpeed = outPacket.jogShuttleVelocity;
                break;
            case SystemLinkTransportCmd::NoChange:
                break;
        }
    }
    return true;
}

// ============================================================================
// 4. ReWireSubsystem
// ============================================================================
ReWireSubsystem::ReWireSubsystem(double sampleRate, std::uint32_t maxBlockFrames)
    : sampleRate_(sampleRate), maxFrames_(maxBlockFrames) {
    buses_.reserve(8);
    for (std::uint32_t i = 0; i < 8U; ++i) {
        buses_.push_back(ReWireDeviceChannelBus{
            i, "ReWire Mix " + std::to_string(i + 1), (i == 0),
            AudioBlock32(SpeakerConfig::Stereo, maxBlockFrames)
        });
    }
}

bool ReWireSubsystem::openDeviceSession(const std::string& slaveAppName, bool actAsHost) {
    if (slaveAppName.empty()) return false;
    connectedPeerName_ = slaveAppName;
    isHostMode_ = actAsHost;
    sessionActive_ = true;
    return true;
}

void ReWireSubsystem::closeDeviceSession() noexcept {
    sessionActive_ = false;
    connectedPeerName_.clear();
}

bool ReWireSubsystem::setBusActive(std::size_t busIdx, bool active) {
    if (busIdx >= buses_.size()) return false;
    buses_[busIdx].active = active;
    return true;
}

void ReWireSubsystem::renderLinkedDeviceRT(const StudioTransportState& transport,
                                           double tempoBpm,
                                           AudioBlock32& mixAccumulator,
                                           std::uint32_t numFrames) noexcept {
    if (!sessionActive_ || !transport.isPlaying) {
        return;
    }
    const double syncFreq = 110.0 * (tempoBpm / 120.0);
    for (auto& bus : buses_) {
        if (!bus.active) continue;
        bus.audioBuffer.clearRT(numFrames);
        float* l = bus.audioBuffer.channel(0);
        float* r = bus.audioBuffer.channel(1);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            testTonePhase_ += (2.0 * std::numbers::pi * syncFreq) / sampleRate_;
            if (testTonePhase_ >= 2.0 * std::numbers::pi) testTonePhase_ -= 2.0 * std::numbers::pi;
            const float s = static_cast<float>(std::sin(testTonePhase_)) * 0.05f;
            l[f] = s;
            if (r) r[f] = s;
        }
        mixAccumulator.accumulateFromRT(bus.audioBuffer, numFrames, 1.0f);
    }
}

} // namespace AlphaStudio::Core::Sync
