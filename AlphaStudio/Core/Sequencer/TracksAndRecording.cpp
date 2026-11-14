#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

const char* trackTypeName(TrackType type) noexcept {
    switch (type) {
        case TrackType::AudioTrack:        return "Audio";
        case TrackType::MIDITrack:         return "MIDI";
        case TrackType::InstrumentTrack:   return "Instrument";
        case TrackType::GroupChannelTrack: return "Group Channel";
        case TrackType::FXChannelTrack:    return "FX Channel";
        case TrackType::FolderTrack:       return "Folder";
        case TrackType::MarkerTrack:       return "Marker";
        case TrackType::RulerTrack:        return "Ruler";
        case TrackType::TempoTrack:        return "Tempo";
        case TrackType::SignatureTrack:    return "Signature";
        case TrackType::ArrangerTrack:     return "Arranger";
        case TrackType::TransposeTrack:    return "Transpose";
    }
    return "Audio";
}

float AudioClipEvent::evaluateVolumeEnvelope(double normPos01) const noexcept {
    if (volumeEnvelope.empty()) {
        return 1.0f;
    }
    const double x = std::clamp(normPos01, 0.0, 1.0);
    if (x <= volumeEnvelope.front().normalizedPosition01) {
        return volumeEnvelope.front().gainLinear;
    }
    for (std::size_t i = 0; i + 1 < volumeEnvelope.size(); ++i) {
        const auto& p0 = volumeEnvelope[i];
        const auto& p1 = volumeEnvelope[i + 1];
        if (x >= p0.normalizedPosition01 && x <= p1.normalizedPosition01) {
            const double span = p1.normalizedPosition01 - p0.normalizedPosition01;
            if (span < 1e-9) {
                return p1.gainLinear;
            }
            const float t = static_cast<float>((x - p0.normalizedPosition01) / span);
            return p0.gainLinear + t * (p1.gainLinear - p0.gainLinear);
        }
    }
    return volumeEnvelope.back().gainLinear;
}

// ============================================================================
// TrackBase & AudioTrack
// ============================================================================
TrackBase::TrackBase(std::uint32_t id, std::string name, TrackType type)
    : trackId_(id), name_(std::move(name)), type_(type) {}

AudioTrack::AudioTrack(std::uint32_t id, std::string name, ChannelStrip* strip)
    : TrackBase(id, std::move(name), TrackType::AudioTrack), strip_(strip) {}

AudioClipEvent& AudioTrack::commitRecordedAudioClip(AudioClipEvent clip,
                                                    RecordMode mode,
                                                    std::uint32_t cycleTakeCounter) {
    clip.eventId = nextClipId_++;
    const std::int64_t newStart = clip.startSample;
    const std::int64_t newEnd = clip.startSample + static_cast<std::int64_t>(clip.lengthFrames);

    if (mode == RecordMode::Overwrite) {
        clips_.erase(
            std::remove_if(clips_.begin(), clips_.end(), [&](const AudioClipEvent& existing) {
                const std::int64_t exEnd =
                    existing.startSample + static_cast<std::int64_t>(existing.lengthFrames);
                return existing.startSample < newEnd && exEnd > newStart;
            }),
            clips_.end());
    } else if (mode == RecordMode::Stacked) {
        lanesExpanded_ = true;
        clip.laneIndex = cycleTakeCounter;
        // Mute earlier overlapping takes on lower lanes so latest take plays on top (comp-ready)
        for (auto& existing : clips_) {
            const std::int64_t exEnd =
                existing.startSample + static_cast<std::int64_t>(existing.lengthFrames);
            if (existing.startSample < newEnd && exEnd > newStart) {
                existing.muted = true;
            }
        }
    }

    clips_.push_back(std::move(clip));
    return clips_.back();
}

// ============================================================================
// MIDITrack & InstrumentTrack
// ============================================================================
MIDITrack::MIDITrack(std::uint32_t id, std::string name, TrackType concreteType)
    : TrackBase(id, std::move(name), concreteType) {}

void MIDITrack::setMidiInsert(std::size_t slot0To3, std::shared_ptr<IMidiInsertProcessor> proc) {
    if (slot0To3 < kNumMidiInserts) {
        midiInserts_[slot0To3] = std::move(proc);
    }
}

IMidiInsertProcessor* MIDITrack::midiInsert(std::size_t slot0To3) const noexcept {
    return (slot0To3 < kNumMidiInserts) ? midiInserts_[slot0To3].get() : nullptr;
}

void MIDITrack::configureMidiSend(std::size_t slot0To3, bool enabled, bool preInserts,
                                  std::uint8_t outPort, std::uint8_t ch) noexcept {
    if (slot0To3 < kNumMidiSends) {
        midiSends_[slot0To3].enabled = enabled;
        midiSends_[slot0To3].preInsertsAndExpression = preInserts;
        midiSends_[slot0To3].outputPortIndex = outPort;
        midiSends_[slot0To3].midiChannel = ch & 0x0FU;
    }
}

MidiPartContainer& MIDITrack::commitRecordedMidiPart(MidiPartContainer incoming,
                                                     RecordMode mode,
                                                     std::uint32_t cycleTakeCounter) {
    const std::int64_t inStart = incoming.startTick;
    const std::int64_t inEnd = incoming.startTick + incoming.lengthTicks;

    if ((mode == RecordMode::Mix || mode == RecordMode::Merge) && !parts_.empty()) {
        for (auto& p : parts_) {
            const std::int64_t pEnd = p.startTick + p.lengthTicks;
            if (p.startTick <= inEnd && pEnd >= inStart) {
                p.startTick = std::min(p.startTick, inStart);
                p.lengthTicks = std::max(pEnd, inEnd) - p.startTick;
                p.events.insert(p.events.end(), incoming.events.begin(), incoming.events.end());
                std::sort(p.events.begin(), p.events.end(),
                          [](const MidiSequenceEvent& a, const MidiSequenceEvent& b) {
                              return a.startTick < b.startTick;
                          });
                return p;
            }
        }
    } else if (mode == RecordMode::Overwrite) {
        for (auto& p : parts_) {
            p.events.erase(
                std::remove_if(p.events.begin(), p.events.end(), [&](const MidiSequenceEvent& e) {
                    return e.startTick >= inStart && e.startTick < inEnd;
                }),
                p.events.end());
        }
    } else if (mode == RecordMode::Stacked) {
        incoming.laneIndex = cycleTakeCounter;
        for (auto& p : parts_) {
            const std::int64_t pEnd = p.startTick + p.lengthTicks;
            if (p.startTick < inEnd && pEnd > inStart) {
                p.muted = true;
            }
        }
    }

    incoming.partId = nextPartId_++;
    parts_.push_back(std::move(incoming));
    return parts_.back();
}

InstrumentTrack::InstrumentTrack(std::uint32_t id, std::string name,
                                 ChannelStrip* audioStrip,
                                 std::string instrumentPluginName)
    : MIDITrack(id, std::move(name), TrackType::InstrumentTrack),
      audioStrip_(audioStrip),
      instrumentName_(std::move(instrumentPluginName)) {}

// ============================================================================
// FolderTrack, MarkerTrack, ArrangerTrack, TransposeTrack
// ============================================================================
void FolderTrack::addChildTrack(TrackBase* child) {
    if (child != nullptr && child != this) {
        children_.push_back(child);
    }
}

void FolderTrack::setGroupMute(bool mute) {
    setMuted(mute);
    for (auto* c : children_) {
        c->setMuted(mute);
    }
}

void FolderTrack::setGroupSolo(bool solo) {
    setSoloed(solo);
    for (auto* c : children_) {
        c->setSoloed(solo);
    }
}

void FolderTrack::setGroupRecordArm(bool arm) {
    setRecordArmed(arm);
    for (auto* c : children_) {
        c->setRecordArmed(arm);
    }
}

std::uint32_t MarkerTrack::addPositionMarker(std::int64_t tick, const std::string& desc) {
    const std::uint32_t id = nextMarkerId_++;
    markers_.push_back(MarkerEvent{id, desc, false, tick, tick});
    return id;
}

std::uint32_t MarkerTrack::addCycleMarker(std::int64_t startTick, std::int64_t endTick,
                                          const std::string& desc) {
    const std::uint32_t id = nextMarkerId_++;
    markers_.push_back(MarkerEvent{id, desc, true, std::min(startTick, endTick), std::max(startTick, endTick)});
    return id;
}

std::uint32_t ArrangerTrack::addRegion(const std::string& label, std::int64_t startTick, std::int64_t endTick) {
    const std::uint32_t id = nextRegionId_++;
    regions_.push_back(ArrangerRegion{id, label, std::min(startTick, endTick), std::max(startTick, endTick)});
    return id;
}

void ArrangerTrack::appendChainStep(std::uint32_t regionId, std::uint32_t repeatCount) {
    chain_.push_back(ArrangerChainStep{regionId, std::max(1U, repeatCount)});
}

std::vector<std::pair<std::int64_t, std::int64_t>> ArrangerTrack::buildFlattenedSchedule() const {
    std::vector<std::pair<std::int64_t, std::int64_t>> schedule;
    for (const auto& step : chain_) {
        for (const auto& reg : regions_) {
            if (reg.regionId == step.regionId) {
                for (std::uint32_t r = 0; r < step.repeatCount; ++r) {
                    schedule.emplace_back(reg.startTick, reg.endTick);
                }
                break;
            }
        }
    }
    return schedule;
}

void TransposeTrack::addTransposeChange(std::int64_t startTick, std::int32_t semitones) {
    events_.push_back(GlobalTransposeEvent{
        std::max<std::int64_t>(0, startTick), std::clamp(semitones, -24, 24)
    });
    std::sort(events_.begin(), events_.end(),
              [](const GlobalTransposeEvent& a, const GlobalTransposeEvent& b) {
                  return a.startTick < b.startTick;
              });
}

std::int32_t TransposeTrack::transposeAtTick(std::int64_t tick) const noexcept {
    std::int32_t cur = 0;
    for (const auto& e : events_) {
        if (tick >= e.startTick) {
            cur = e.semitones;
        } else {
            break;
        }
    }
    return cur;
}

// ============================================================================
// RetrospectiveRecordBuffer
// ============================================================================
bool RetrospectiveRecordBuffer::captureMidiEventRT(const MidiSequenceEvent& ev) noexcept {
    if (ring_.full()) {
        MidiSequenceEvent discarded{};
        (void)ring_.tryPop(discarded); // Circular overwrite of oldest event
    }
    return ring_.tryPush(ev);
}

MidiPartContainer RetrospectiveRecordBuffer::materializeCapturedPart(const std::string& partName) {
    MidiPartContainer part;
    part.name = partName;
    MidiSequenceEvent ev{};
    std::int64_t minTick = 0;
    std::int64_t maxTick = 1920;
    bool first = true;

    while (ring_.tryPop(ev)) {
        if (first) {
            minTick = ev.startTick;
            maxTick = ev.startTick + ev.lengthTicks;
            first = false;
        } else {
            minTick = std::min(minTick, ev.startTick);
            maxTick = std::max(maxTick, ev.startTick + ev.lengthTicks);
        }
        part.events.push_back(ev);
    }

    part.startTick = minTick;
    part.lengthTicks = std::max<std::int64_t>(480, maxTick - minTick);
    return part;
}

} // namespace AlphaStudio::Core
