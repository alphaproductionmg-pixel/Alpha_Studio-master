#include "AlphaStudio/Core/Workflow/AutomationAndQuickControls.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

// ============================================================================
// AutomationLane
// ============================================================================
AutomationLane::AutomationLane(std::uint32_t paramId, std::string paramName,
                               AutomationCategory category, float initialValue)
    : paramId_(paramId),
      paramName_(std::move(paramName)),
      category_(category),
      staticFallbackValue_(std::clamp(initialValue, 0.0f, 1.0f)) {}

void AutomationLane::insertOrReplacePoint(std::int64_t samplePos, float normVal) {
    const std::int64_t s = std::max<std::int64_t>(0, samplePos);
    const float v = std::clamp(normVal, 0.0f, 1.0f);
    for (auto& pt : points_) {
        if (pt.samplePosition == s) {
            pt.normalizedValue = v;
            return;
        }
    }
    points_.push_back(AutomationPoint{s, v});
    std::sort(points_.begin(), points_.end(),
              [](const AutomationPoint& a, const AutomationPoint& b) {
                  return a.samplePosition < b.samplePosition;
              });
}

float AutomationLane::evaluateAtSample(std::int64_t samplePos) const noexcept {
    if (points_.empty()) {
        return staticFallbackValue_;
    }
    if (samplePos <= points_.front().samplePosition) {
        return points_.front().normalizedValue;
    }
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        const auto& p0 = points_[i];
        const auto& p1 = points_[i + 1];
        if (samplePos >= p0.samplePosition && samplePos <= p1.samplePosition) {
            const std::int64_t span = p1.samplePosition - p0.samplePosition;
            if (span <= 0) {
                return p1.normalizedValue;
            }
            const float t = static_cast<float>(samplePos - p0.samplePosition) /
                            static_cast<float>(span);
            return p0.normalizedValue + t * (p1.normalizedValue - p0.normalizedValue);
        }
    }
    return points_.back().normalizedValue;
}

float AutomationLane::processAutomationPass(
    std::int64_t currentSample,
    std::int64_t projectLengthSamples,
    std::int64_t loopStartSample,
    std::int64_t loopEndSample,
    bool controlTouched,
    float liveNormalizedInput,
    AutomationWriteMode mode,
    std::uint8_t fillMask,
    bool suspendRead,
    bool suspendWrite) {
    const float clampedLive = std::clamp(liveNormalizedInput, 0.0f, 1.0f);
    const float curveVal = evaluateAtSample(currentSample);

    if (writeEnabled_ && !suspendWrite) {
        switch (mode) {
            case AutomationWriteMode::Touch:
                if (controlTouched) {
                    if (!latched_) {
                        latched_ = true;
                        punchInSample_ = currentSample;
                    }
                    insertOrReplacePoint(currentSample, clampedLive);
                } else {
                    latched_ = false;
                }
                break;

            case AutomationWriteMode::AutoLatch:
                if (controlTouched && !latched_) {
                    latched_ = true;
                    punchInSample_ = currentSample;
                }
                if (latched_) {
                    insertOrReplacePoint(currentSample, clampedLive);
                }
                break;

            case AutomationWriteMode::CrossOver:
                if (controlTouched && !latched_) {
                    latched_ = true;
                    punchInSample_ = currentSample;
                    crossOverInitialSide_ = (clampedLive >= curveVal) ? 1.0f : -1.0f;
                }
                if (latched_) {
                    const float diff = clampedLive - curveVal;
                    if (!controlTouched && ((crossOverInitialSide_ > 0.0f && diff <= 0.0f) ||
                                            (crossOverInitialSide_ < 0.0f && diff >= 0.0f))) {
                        latched_ = false; // Crossed existing automation curve -> punch out cleanly!
                    } else {
                        insertOrReplacePoint(currentSample, clampedLive);
                    }
                }
                break;

            case AutomationWriteMode::Trim:
                // Trim mode applies relative delta and retains the last Trim offset even after release!
                if (controlTouched) {
                    if (!trimPassActive_) {
                        trimPassActive_ = true;
                        punchInSample_ = currentSample;
                    }
                    activeTrimDelta_ = clampedLive - 0.5f; // Center 0.5 == 0 dB / zero trim delta
                }
                if (trimPassActive_) {
                    return std::clamp(curveVal + activeTrimDelta_, 0.0f, 1.0f);
                }
                break;
        }

        // Apply any active Fill Modes (ToPunch, ToStart, ToEnd, Loop)
        if ((latched_ || controlTouched) && fillMask != 0U) {
            if ((fillMask & static_cast<std::uint8_t>(AutomationFillFlags::ToStart)) != 0U) {
                insertOrReplacePoint(0, clampedLive);
            }
            if ((fillMask & static_cast<std::uint8_t>(AutomationFillFlags::ToPunch)) != 0U) {
                insertOrReplacePoint(punchInSample_, clampedLive);
            }
            if ((fillMask & static_cast<std::uint8_t>(AutomationFillFlags::ToEnd)) != 0U) {
                insertOrReplacePoint(std::max(currentSample, projectLengthSamples), clampedLive);
            }
            if ((fillMask & static_cast<std::uint8_t>(AutomationFillFlags::Loop)) != 0U &&
                loopEndSample > loopStartSample) {
                insertOrReplacePoint(loopStartSample, clampedLive);
                insertOrReplacePoint(loopEndSample, clampedLive);
            }
        }
    }

    if (readEnabled_ && !suspendRead) {
        return evaluateAtSample(currentSample);
    }
    return staticFallbackValue_;
}

void AutomationLane::commitTrimPass(std::int64_t passEndSample) {
    if (!trimPassActive_) {
        return;
    }
    if (points_.empty()) {
        insertOrReplacePoint(punchInSample_, std::clamp(staticFallbackValue_ + activeTrimDelta_, 0.0f, 1.0f));
        insertOrReplacePoint(passEndSample, std::clamp(staticFallbackValue_ + activeTrimDelta_, 0.0f, 1.0f));
    } else {
        for (auto& pt : points_) {
            if (pt.samplePosition >= punchInSample_ && pt.samplePosition <= passEndSample) {
                pt.normalizedValue = std::clamp(pt.normalizedValue + activeTrimDelta_, 0.0f, 1.0f);
            }
        }
    }
    trimPassActive_ = false;
    activeTrimDelta_ = 0.0f;
}

// ============================================================================
// RuleBasedConsolidation
// ============================================================================
float RuleBasedConsolidation::evaluateConsolidatedValue(
    AutomationLane& trackLane,
    MidiPartContainer& midiPart,
    std::uint8_t targetCCNumber,
    std::int64_t currentTick,
    std::int64_t currentSample,
    const MusicalClock& clock,
    MidiAutomationMergeRule rule) {
    if (rule == MidiAutomationMergeRule::ExtractCCToCurve) {
        midiPart.events.erase(
            std::remove_if(midiPart.events.begin(), midiPart.events.end(),
                           [&](const MidiSequenceEvent& e) {
                               if (e.kind == SequencerMidiKind::ControlChange &&
                                   e.pitchOrCC == targetCCNumber) {
                                   const std::int64_t absTick = midiPart.startTick + e.startTick;
                                   trackLane.insertOrReplacePoint(
                                       clock.ticksToSamples(absTick),
                                       static_cast<float>(e.velocityOrVal) / 127.0f);
                                   return true;
                               }
                               return false;
                           }),
            midiPart.events.end());
        return trackLane.evaluateAtSample(currentSample);
    }

    const float autoVal = trackLane.evaluateAtSample(currentSample);
    if (rule == MidiAutomationMergeRule::AutomationOnly) {
        return autoVal;
    }

    bool foundCC = false;
    float latestCCNorm = 0.5f;
    for (const auto& e : midiPart.events) {
        if (e.kind == SequencerMidiKind::ControlChange && e.pitchOrCC == targetCCNumber) {
            const std::int64_t absTick = midiPart.startTick + e.startTick;
            if (absTick <= currentTick) {
                foundCC = true;
                latestCCNorm = static_cast<float>(e.velocityOrVal) / 127.0f;
            }
        }
    }

    if (!foundCC) {
        return autoVal;
    }

    switch (rule) {
        case MidiAutomationMergeRule::MidiPartPriority:
            return latestCCNorm;
        case MidiAutomationMergeRule::AverageBoth:
            return 0.5f * (autoVal + latestCCNorm);
        case MidiAutomationMergeRule::RelativeOffsetSum:
            return std::clamp(autoVal + (latestCCNorm - 0.5f), 0.0f, 1.0f);
        case MidiAutomationMergeRule::AutomationOnly:
        case MidiAutomationMergeRule::ExtractCCToCurve:
            break;
    }
    return autoVal;
}

// ============================================================================
// TrackQuickControls (Extended across Audio, Instrument, MIDI, Group, FX, In, Out)
// ============================================================================
TrackQuickControls::TrackQuickControls(MixerChannelKind hostKind)
    : hostKind_(hostKind) {}

bool TrackQuickControls::assignSlot(std::size_t slot0To7, const std::string& label,
                                    std::uint32_t paramId, float initialNormalized,
                                    std::function<void(float)> setter) {
    if (slot0To7 >= kNumQuickControls) {
        return false;
    }
    slots_[slot0To7] = QuickControlAssignment{
        .assigned = true,
        .label = label,
        .targetParameterId = paramId,
        .normalizedValue = std::clamp(initialNormalized, 0.0f, 1.0f),
        .setterCallback = std::move(setter)
    };
    return true;
}

void TrackQuickControls::setQuickControlValue(std::size_t slot0To7, float normalized01) {
    if (!enabled_ || slot0To7 >= kNumQuickControls || !slots_[slot0To7].assigned) {
        return;
    }
    const float v = std::clamp(normalized01, 0.0f, 1.0f);
    slots_[slot0To7].normalizedValue = v;
    if (slots_[slot0To7].setterCallback) {
        slots_[slot0To7].setterCallback(v);
    }
}

float TrackQuickControls::getQuickControlValue(std::size_t slot0To7) const noexcept {
    return (slot0To7 < kNumQuickControls) ? slots_[slot0To7].normalizedValue : 0.0f;
}

void TrackQuickControls::bindDefaultChannelStripTargets(ChannelStrip& strip) {
    hostKind_ = strip.kind();
    assignSlot(0, "Volume", 100, 0.75f, [&strip](float n) { strip.setFaderDb((n * 66.0f) - 60.0f); });
    assignSlot(1, "Pan", 101, 0.50f, [&strip](float n) { strip.panner().setPan(n * 2.0f - 1.0f); });
    assignSlot(2, "Input Trim", 102, 0.50f, [&strip](float n) { strip.setInputGainDb((n - 0.5f) * 48.0f); });
    assignSlot(3, "EQ Low Gain", 103, 0.50f, [&strip](float n) {
        strip.channelEQ().configureBand(0, true, ChannelEQBandType::LowShelf, 100.0f, (n - 0.5f) * 48.0f, 0.707f);
    });
    assignSlot(4, "EQ LoMid Gain", 104, 0.50f, [&strip](float n) {
        strip.channelEQ().configureBand(1, true, ChannelEQBandType::ParametricPeak, 800.0f, (n - 0.5f) * 48.0f, 1.0f);
    });
    assignSlot(5, "EQ HiMid Gain", 105, 0.50f, [&strip](float n) {
        strip.channelEQ().configureBand(2, true, ChannelEQBandType::ParametricPeak, 3200.0f, (n - 0.5f) * 48.0f, 1.0f);
    });
    assignSlot(6, "EQ High Gain", 106, 0.50f, [&strip](float n) {
        strip.channelEQ().configureBand(3, true, ChannelEQBandType::HighShelf, 10000.0f, (n - 0.5f) * 48.0f, 0.707f);
    });
    assignSlot(7, "FX Send 1", 107, 0.0f, [&strip](float n) {
        strip.configureAuxSend(0, n > 0.01f, false, strip.auxSend(0).targetBusChannelId, (n * 66.0f) - 60.0f);
    });
}

} // namespace AlphaStudio::Core
