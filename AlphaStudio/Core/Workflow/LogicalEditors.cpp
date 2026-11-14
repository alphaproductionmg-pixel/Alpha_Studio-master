#include "AlphaStudio/Core/Workflow/LogicalEditors.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

namespace {
[[nodiscard]] bool evalNumeric(std::int64_t lhs, LogicalComparison cmp,
                               std::int64_t a, std::int64_t b) noexcept {
    switch (cmp) {
        case LogicalComparison::Equal:        return lhs == a;
        case LogicalComparison::NotEqual:     return lhs != a;
        case LogicalComparison::Greater:      return lhs > a;
        case LogicalComparison::GreaterEqual: return lhs >= a;
        case LogicalComparison::Less:         return lhs < a;
        case LogicalComparison::LessEqual:    return lhs <= a;
        case LogicalComparison::InsideRange:  return lhs >= std::min(a, b) && lhs <= std::max(a, b);
        case LogicalComparison::ContainsText: return lhs == a;
    }
    return false;
}
} // namespace

// ============================================================================
// LogicalEditor (MIDI, Display-PPQ Resolution Dependent)
// ============================================================================
bool LogicalEditor::matchesAllClauses(const MidiSequenceEvent& ev,
                                      std::span<const MidiLogicalFilterClause> clauses,
                                      const MusicalClock& clock) noexcept {
    if (clauses.empty()) {
        return true;
    }
    const std::int64_t internalPpq = clock.internalPPQ();
    const std::int64_t dispPpq = clock.midiDisplayResolutionPPQ();

    bool acc = true;
    for (std::size_t i = 0; i < clauses.size(); ++i) {
        const auto& c = clauses[i];
        std::int64_t value = 0;
        switch (c.target) {
            case MidiFilterTarget::EventType:
                value = static_cast<std::int64_t>(ev.kind);
                break;
            case MidiFilterTarget::Value1_PitchOrCC:
                value = ev.pitchOrCC;
                break;
            case MidiFilterTarget::Value2_VelocityOrVal:
                value = ev.velocityOrVal;
                break;
            case MidiFilterTarget::Channel:
                value = ev.channel;
                break;
            case MidiFilterTarget::LengthInDisplayTicks:
                // Dependent on active MIDI Display PPQ resolution!
                value = (ev.lengthTicks * dispPpq) / std::max<std::int64_t>(1, internalPpq);
                break;
            case MidiFilterTarget::PositionInBarTicks: {
                const TimeSignatureNode sig = clock.signatureAtTick(ev.startTick);
                const std::int64_t barInternalTicks =
                    (static_cast<std::int64_t>(sig.numerator) * internalPpq * 4LL) /
                    std::max(1, sig.denominator);
                const std::int64_t inBarInternal =
                    (ev.startTick - sig.tickPosition) % std::max<std::int64_t>(1, barInternalTicks);
                value = (inBarInternal * dispPpq) / std::max<std::int64_t>(1, internalPpq);
                break;
            }
        }

        const bool clauseMatch = evalNumeric(value, c.comparison, c.paramA, c.paramB);
        if (i == 0) {
            acc = clauseMatch;
        } else if (c.joinWithPrevious == LogicalBoolOp::And) {
            acc = acc && clauseMatch;
        } else {
            acc = acc || clauseMatch;
        }
    }
    return acc;
}

void LogicalEditor::applyTransform(MidiSequenceEvent& ev,
                                   const MidiLogicalTransformAction& act,
                                   const MusicalClock& clock) noexcept {
    auto applyMath = [&](double current) -> double {
        switch (act.mathOp) {
            case TransformMathOp::SetFixedValue: return act.operand;
            case TransformMathOp::Add:           return current + act.operand;
            case TransformMathOp::Subtract:      return current - act.operand;
            case TransformMathOp::MultiplyRatio: return current * act.operand;
        }
        return current;
    };

    const double scaleFromDispToInternal =
        static_cast<double>(clock.internalPPQ()) /
        static_cast<double>(std::max(1, clock.midiDisplayResolutionPPQ()));

    switch (act.target) {
        case MidiTransformTarget::Value1_PitchOrCC:
            ev.pitchOrCC = static_cast<std::uint8_t>(
                std::clamp<int>(static_cast<int>(std::llround(applyMath(ev.pitchOrCC))), 0, 127));
            break;
        case MidiTransformTarget::Value2_VelocityOrVal:
            ev.velocityOrVal = static_cast<std::uint8_t>(
                std::clamp<int>(static_cast<int>(std::llround(applyMath(ev.velocityOrVal))), 0, 127));
            break;
        case MidiTransformTarget::Channel:
            ev.channel = static_cast<std::uint8_t>(
                std::clamp<int>(static_cast<int>(std::llround(applyMath(ev.channel))), 0, 15));
            break;
        case MidiTransformTarget::LengthTicks: {
            if (act.mathOp == TransformMathOp::MultiplyRatio) {
                ev.lengthTicks = std::max<std::int64_t>(
                    1, static_cast<std::int64_t>(std::llround(ev.lengthTicks * act.operand)));
            } else {
                const double curDisp = static_cast<double>(ev.lengthTicks) / scaleFromDispToInternal;
                ev.lengthTicks = std::max<std::int64_t>(
                    1, static_cast<std::int64_t>(std::llround(applyMath(curDisp) * scaleFromDispToInternal)));
            }
            break;
        }
        case MidiTransformTarget::PositionTicks: {
            if (act.mathOp == TransformMathOp::MultiplyRatio) {
                ev.startTick = std::max<std::int64_t>(
                    0, static_cast<std::int64_t>(std::llround(ev.startTick * act.operand)));
            } else {
                const double curDisp = static_cast<double>(ev.startTick) / scaleFromDispToInternal;
                ev.startTick = std::max<std::int64_t>(
                    0, static_cast<std::int64_t>(std::llround(applyMath(curDisp) * scaleFromDispToInternal)));
            }
            break;
        }
    }
}

std::vector<MidiSequenceEvent> LogicalEditor::applyPreset(
    const MidiLogicalPreset& preset,
    std::vector<MidiSequenceEvent>& targetEvents,
    const MusicalClock& clock) {
    std::vector<MidiSequenceEvent> extracted;

    if (preset.mode == LogicalExecutionMode::Delete || preset.mode == LogicalExecutionMode::Extract) {
        std::vector<MidiSequenceEvent> remaining;
        remaining.reserve(targetEvents.size());
        for (const auto& ev : targetEvents) {
            if (matchesAllClauses(ev, preset.filters, clock)) {
                if (preset.mode == LogicalExecutionMode::Extract) {
                    extracted.push_back(ev);
                }
            } else {
                remaining.push_back(ev);
            }
        }
        targetEvents = std::move(remaining);
        return extracted;
    }

    for (auto& ev : targetEvents) {
        const bool matched = matchesAllClauses(ev, preset.filters, clock);
        if (preset.mode == LogicalExecutionMode::Select) {
            ev.selected = matched;
        } else if (matched && preset.mode == LogicalExecutionMode::Transform) {
            for (const auto& act : preset.actions) {
                applyTransform(ev, act, clock);
            }
        }
    }
    return extracted;
}

// ============================================================================
// ProjectLogicalEditor (PLE)
// ============================================================================
bool ProjectLogicalEditor::matchesTrack(const TrackBase& track,
                                        std::span<const PLEFilterClause> clauses) {
    if (clauses.empty()) {
        return true;
    }
    bool acc = true;
    for (std::size_t i = 0; i < clauses.size(); ++i) {
        const auto& c = clauses[i];
        bool match = false;
        switch (c.target) {
            case PLEMediaTarget::TrackName:
                if (c.comparison == LogicalComparison::ContainsText) {
                    match = (track.name().find(c.textParam) != std::string::npos);
                } else if (c.comparison == LogicalComparison::Equal) {
                    match = (track.name() == c.textParam);
                } else {
                    match = (track.name() != c.textParam);
                }
                break;
            case PLEMediaTarget::TrackMediaType:
                match = evalNumeric(static_cast<std::int64_t>(track.type()),
                                    c.comparison, c.numericParam, c.numericParam);
                break;
            case PLEMediaTarget::PropertyIsMuted:
                match = (static_cast<std::int64_t>(track.isMuted() ? 1 : 0) == c.numericParam);
                break;
            case PLEMediaTarget::PropertyIsSoloed:
                match = (static_cast<std::int64_t>(track.isSoloed() ? 1 : 0) == c.numericParam);
                break;
            case PLEMediaTarget::PropertyIsArmed:
                match = (static_cast<std::int64_t>(track.isRecordArmed() ? 1 : 0) == c.numericParam);
                break;
            case PLEMediaTarget::ContainerCount: {
                std::int64_t count = 0;
                if (const auto* at = dynamic_cast<const AudioTrack*>(&track)) {
                    count = static_cast<std::int64_t>(at->clips().size());
                } else if (const auto* mt = dynamic_cast<const MIDITrack*>(&track)) {
                    count = static_cast<std::int64_t>(mt->parts().size());
                }
                match = evalNumeric(count, c.comparison, c.numericParam, c.numericParam);
                break;
            }
        }
        if (i == 0) {
            acc = match;
        } else if (c.joinWithPrevious == LogicalBoolOp::And) {
            acc = acc && match;
        } else {
            acc = acc || match;
        }
    }
    return acc;
}

std::vector<std::uint32_t> ProjectLogicalEditor::executeOnProjectTracks(
    const ProjectLogicalPreset& preset,
    std::vector<std::unique_ptr<TrackBase>>& tracks) {
    std::vector<std::uint32_t> matchedIds;

    if (preset.mode == LogicalExecutionMode::Delete) {
        tracks.erase(
            std::remove_if(tracks.begin(), tracks.end(), [&](const std::unique_ptr<TrackBase>& t) {
                if (matchesTrack(*t, preset.filters)) {
                    matchedIds.push_back(t->trackId());
                    return true;
                }
                return false;
            }),
            tracks.end());
        return matchedIds;
    }

    for (auto& t : tracks) {
        if (!matchesTrack(*t, preset.filters)) {
            continue;
        }
        matchedIds.push_back(t->trackId());
        if (preset.mode == LogicalExecutionMode::Transform) {
            for (const auto& act : preset.actions) {
                switch (act.target) {
                    case PLEActionTarget::AppendTrackName:
                        t->setName(t->name() + act.textOperand);
                        break;
                    case PLEActionTarget::PrependTrackName:
                        t->setName(act.textOperand + t->name());
                        break;
                    case PLEActionTarget::ReplaceTrackName:
                        t->setName(act.textOperand);
                        break;
                    case PLEActionTarget::SetTrackMute:
                        t->setMuted(act.numericOperand != 0);
                        break;
                    case PLEActionTarget::SetTrackSolo:
                        t->setSoloed(act.numericOperand != 0);
                        break;
                    case PLEActionTarget::SetTrackColor:
                        t->setColorRgba(act.numericOperand);
                        break;
                }
            }
        }
    }
    return matchedIds;
}

} // namespace AlphaStudio::Core
