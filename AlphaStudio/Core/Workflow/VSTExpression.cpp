#include "AlphaStudio/Core/Workflow/VSTExpression.hpp"

#include <algorithm>

namespace AlphaStudio::Core {

ExpressionMap::ExpressionMap(std::string mapName)
    : name_(std::move(mapName)) {}

std::uint16_t ExpressionMap::addSoundSlot(ExpressionSoundSlot slot) {
    slot.slotId = nextSlotId_++;
    slots_.push_back(std::move(slot));
    return slots_.back().slotId;
}

const ExpressionSoundSlot* ExpressionMap::findSlot(std::uint16_t slotId) const noexcept {
    for (const auto& s : slots_) {
        if (s.slotId == slotId) {
            return &s;
        }
    }
    return nullptr;
}

const ExpressionSoundSlot* ExpressionMap::findSlotByRemoteKeySwitch(std::uint8_t midiNote) const noexcept {
    for (const auto& s : slots_) {
        if (s.remoteTriggerKeySwitch.has_value() && *s.remoteTriggerKeySwitch == midiNote) {
            return &s;
        }
    }
    return nullptr;
}

std::uint32_t ExpressionMap::colorForArticulationSlot(std::uint16_t slotId) const noexcept {
    if (const auto* s = findSlot(slotId)) {
        return s->noteColorRgba;
    }
    return 0x4AA3FFFFU;
}

std::vector<MidiSequenceEvent> ExpressionMap::evaluateTrackMidiWithExpression(
    MIDITrack& track,
    std::span<const MidiSequenceEvent> rawEvents,
    std::int64_t blockStartTick,
    std::int64_t blockEndTick,
    const MusicalClock& clock,
    std::array<std::vector<MidiSequenceEvent>, MIDITrack::kNumMidiSends>* routedMidiSendsOut) const {
    std::vector<MidiSequenceEvent> working(rawEvents.begin(), rawEvents.end());

    // 1. Tap Pre-Inserts/Pre-Expression MIDI Sends (Pre/Post button == Pre)
    if (routedMidiSendsOut != nullptr) {
        for (std::size_t i = 0; i < MIDITrack::kNumMidiSends; ++i) {
            (*routedMidiSendsOut)[i].clear();
            const auto& send = track.midiSend(i);
            if (send.enabled && send.preInsertsAndExpression) {
                (*routedMidiSendsOut)[i] = working;
                if (send.sendEffect) {
                    send.sendEffect->processMidiEvents(
                        (*routedMidiSendsOut)[i], blockStartTick, blockEndTick, clock);
                }
            }
        }
    }

    // 2. Run Track's 4 MIDI Insert Slots
    for (std::size_t ins = 0; ins < MIDITrack::kNumMidiInserts; ++ins) {
        if (auto* proc = track.midiInsert(ins)) {
            proc->processMidiEvents(working, blockStartTick, blockEndTick, clock);
        }
    }

    // 3. Expand VST Expression SoundSlots (Key Switches, Program Changes, up to 2 CCs)
    std::vector<MidiSequenceEvent> expanded;
    expanded.reserve(working.size() * 3U);
    std::uint16_t activeDirectionSlot = 0;

    for (const auto& ev : working) {
        if (ev.muted) {
            continue;
        }
        if (ev.kind == SequencerMidiKind::Note) {
            // Check if incoming note is a live remote key-switch
            if (const auto* remoteSlot = findSlotByRemoteKeySwitch(ev.pitchOrCC)) {
                activeDirectionSlot = remoteSlot->slotId;
                continue;
            }

            const std::uint16_t targetSlotId =
                (ev.articulationSlotId != 0) ? ev.articulationSlotId : activeDirectionSlot;
            const auto* slot = findSlot(targetSlotId);
            MidiSequenceEvent noteOut = ev;

            if (slot != nullptr) {
                if (slot->behavior == ArticulationBehavior::DirectionPersistent) {
                    activeDirectionSlot = slot->slotId;
                }
                if (slot->outputChannelOverride.has_value()) {
                    noteOut.channel = *slot->outputChannelOverride & 0x0FU;
                }
                // Emit Program Change if configured
                if (slot->outputProgramChange.has_value()) {
                    expanded.push_back(MidiSequenceEvent{
                        .kind = SequencerMidiKind::ProgramChange,
                        .startTick = std::max<std::int64_t>(0, ev.startTick - 2),
                        .lengthTicks = 0,
                        .channel = noteOut.channel,
                        .pitchOrCC = *slot->outputProgramChange,
                        .velocityOrVal = 0
                    });
                }
                // Emit up to 2 MIDI CC controllers per SoundSlot
                for (const auto& cc : slot->outputControllers) {
                    if (cc.enabled) {
                        expanded.push_back(MidiSequenceEvent{
                            .kind = SequencerMidiKind::ControlChange,
                            .startTick = std::max<std::int64_t>(0, ev.startTick - 1),
                            .lengthTicks = 0,
                            .channel = noteOut.channel,
                            .pitchOrCC = cc.ccNumber,
                            .velocityOrVal = cc.ccValue
                        });
                    }
                }
                // Emit Key Switch note slightly ahead/coincident with articulated note
                if (slot->outputKeySwitchNote.has_value()) {
                    expanded.push_back(MidiSequenceEvent{
                        .kind = SequencerMidiKind::Note,
                        .startTick = std::max<std::int64_t>(0, ev.startTick - 1),
                        .lengthTicks = std::max<std::int64_t>(10, ev.lengthTicks),
                        .channel = noteOut.channel,
                        .pitchOrCC = *slot->outputKeySwitchNote,
                        .velocityOrVal = slot->outputKeySwitchVelocity
                    });
                }

                noteOut.velocityOrVal = static_cast<std::uint8_t>(std::clamp<int>(
                    (static_cast<int>(noteOut.velocityOrVal) * slot->velocityScalePercent) / 100, 1, 127));
                noteOut.lengthTicks = std::max<std::int64_t>(
                    1, (noteOut.lengthTicks * slot->lengthScalePercent) / 100);
            }

            expanded.push_back(noteOut);
        } else {
            expanded.push_back(ev);
        }
    }

    // 4. Tap Post-Inserts/Post-Expression MIDI Sends (Pre/Post button == Post)
    if (routedMidiSendsOut != nullptr) {
        for (std::size_t i = 0; i < MIDITrack::kNumMidiSends; ++i) {
            const auto& send = track.midiSend(i);
            if (send.enabled && !send.preInsertsAndExpression) {
                (*routedMidiSendsOut)[i] = expanded;
                if (send.sendEffect) {
                    send.sendEffect->processMidiEvents(
                        (*routedMidiSendsOut)[i], blockStartTick, blockEndTick, clock);
                }
            }
        }
    }

    return expanded;
}

} // namespace AlphaStudio::Core
