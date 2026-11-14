#include "AlphaStudio/Core/DSP/MidiPluginsSuite.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AlphaStudio::Core::DSP {

SuiteMidiPlugin::SuiteMidiPlugin(std::string exactName, MidiPluginAlgorithm algo)
    : name_(std::move(exactName)), algo_(algo) {
    // Initialize default 64-step Beat Designer electronic kit pattern
    constexpr std::array<std::uint8_t, kNumDrumLanes> kDefaultPitches = {36, 38, 42, 46, 39, 45, 48, 49};
    for (std::size_t l = 0; l < kNumDrumLanes; ++l) {
        drumLanes_[l].drumNotePitch = kDefaultPitches[l];
        for (std::size_t s = 0; s < kMaxBeatDesignerSteps; ++s) {
            if (l == 0 && (s % 4 == 0)) drumLanes_[l].stepVelocities[s] = 115;       // Four-on-the-floor Kick
            else if (l == 1 && (s % 8 == 4)) drumLanes_[l].stepVelocities[s] = 110;  // Backbeat Snare
            else if (l == 2 && (s % 2 == 0)) drumLanes_[l].stepVelocities[s] = 85;   // 8th/16th Closed Hat
        }
    }

    // Initialize StepDesigner melodic sequence
    constexpr std::array<std::int8_t, 8> kArpOffsets = {0, 3, 7, 12, 7, 3, 10, 7};
    for (std::size_t s = 0; s < 64; ++s) {
        stepDesignerPitches_[s] = static_cast<std::int8_t>(60 + kArpOffsets[s & 7U]);
        stepDesignerVelocities_[s] = (s % 2 == 0) ? 100 : 75;
    }

    // Default real-time Transformer rule: boost soft notes (< 64) by +12 velocity
    transformerPreset_.presetName = "Live Velocity Lifter";
    transformerPreset_.mode = LogicalExecutionMode::Transform;
    transformerPreset_.filters.push_back(MidiLogicalFilterClause{
        LogicalBoolOp::And, MidiFilterTarget::Value2_VelocityOrVal, LogicalComparison::Less, 64, 0});
    transformerPreset_.actions.push_back(MidiLogicalTransformAction{
        MidiTransformTarget::Value2_VelocityOrVal, TransformMathOp::Add, 12.0});
}

void SuiteMidiPlugin::reset() noexcept {
    monitorLog_.clear();
}

void SuiteMidiPlugin::setParameterNormalized(std::uint32_t paramIdx, float value) noexcept {
    if (paramIdx < params_.size()) {
        params_[paramIdx] = std::clamp(value, 0.0f, 1.0f);
    }
}

float SuiteMidiPlugin::getParameterNormalized(std::uint32_t paramIdx) const noexcept {
    return (paramIdx < params_.size()) ? params_[paramIdx] : 0.0f;
}

void SuiteMidiPlugin::setBeatDesignerStepCount(std::uint32_t steps1To64) noexcept {
    beatDesignerSteps_ = std::clamp(steps1To64, 1U, static_cast<std::uint32_t>(kMaxBeatDesignerSteps));
}

void SuiteMidiPlugin::setBeatDesignerStep(std::size_t lane0To7, std::size_t step0To63,
                                          std::uint8_t velocity, bool flam) noexcept {
    if (lane0To7 < kNumDrumLanes && step0To63 < kMaxBeatDesignerSteps) {
        drumLanes_[lane0To7].stepVelocities[step0To63] = std::min<std::uint8_t>(127U, velocity);
        drumLanes_[lane0To7].stepFlam[step0To63] = flam;
    }
}

void SuiteMidiPlugin::setMicroTuningCents(std::size_t pitchClass0To11, float cents) noexcept {
    if (pitchClass0To11 < 12U) {
        microTuneCents_[pitchClass0To11] = std::clamp(cents, -100.0f, 100.0f);
    }
}

void SuiteMidiPlugin::processMidiEvents(std::vector<MidiSequenceEvent>& events,
                                        std::int64_t blockStartTick,
                                        std::int64_t blockEndTick,
                                        const MusicalClock& clock) {
    constexpr std::int64_t kStepTicks = MusicalClock::kSixteenthNoteTicks; // 120 ticks @ 480 PPQ

    switch (algo_) {
        case MidiPluginAlgorithm::Arpache5:
        case MidiPluginAlgorithm::ArpacheSX: {
            std::vector<MidiSequenceEvent> heldNotes;
            std::vector<MidiSequenceEvent> nonNotes;
            for (const auto& e : events) {
                if (e.kind == SequencerMidiKind::Note && e.velocityOrVal > 0) heldNotes.push_back(e);
                else nonNotes.push_back(e);
            }
            if (!heldNotes.empty()) {
                std::sort(heldNotes.begin(), heldNotes.end(),
                          [](const MidiSequenceEvent& a, const MidiSequenceEvent& b) {
                              return a.pitchOrCC < b.pitchOrCC;
                          });
                const int octaves = (algo_ == MidiPluginAlgorithm::ArpacheSX)
                    ? (1 + static_cast<int>(params_[0] * 3.0f))
                    : (1 + static_cast<int>(params_[0] * 2.0f));
                const std::int64_t gateTicks = std::max<std::int64_t>(20, static_cast<std::int64_t>(kStepTicks * (0.25f + 0.75f * params_[1])));
                const std::int64_t firstStep = (blockStartTick / kStepTicks) * kStepTicks;
                std::size_t idx = static_cast<std::size_t>(firstStep / kStepTicks);

                events = std::move(nonNotes);
                for (std::int64_t t = firstStep; t < blockEndTick; t += kStepTicks, ++idx) {
                    if (t < blockStartTick) continue;
                    const std::size_t noteIdx = idx % heldNotes.size();
                    const int oct = static_cast<int>((idx / heldNotes.size()) % static_cast<std::size_t>(octaves));
                    MidiSequenceEvent arp = heldNotes[noteIdx];
                    arp.startTick = t;
                    arp.lengthTicks = gateTicks;
                    arp.pitchOrCC = static_cast<std::uint8_t>(std::clamp(static_cast<int>(arp.pitchOrCC) + oct * 12, 0, 127));
                    events.push_back(arp);
                }
            }
            break;
        }

        case MidiPluginAlgorithm::Autopan: {
            for (std::int64_t t = (blockStartTick / kStepTicks) * kStepTicks; t < blockEndTick; t += kStepTicks) {
                if (t < blockStartTick) continue;
                const double phase = (static_cast<double>(t) / 1920.0) * 2.0 * std::numbers::pi;
                const auto ccVal = static_cast<std::uint8_t>(std::clamp(std::lround(64.0 + 63.0 * params_[0] * std::sin(phase)), 0L, 127L));
                events.push_back(MidiSequenceEvent{SequencerMidiKind::ControlChange, t, 0, 0, 10, ccVal});
            }
            break;
        }

        case MidiPluginAlgorithm::BeatDesigner: {
            // 64-Step Drum Pattern Sequencer with Swing & Flam
            const std::int64_t firstStepIdx = blockStartTick / kStepTicks;
            const std::int64_t lastStepIdx = (blockEndTick + kStepTicks - 1) / kStepTicks;
            const std::int64_t swingTicks = static_cast<std::int64_t>((params_[0] - 0.5f) * 40.0f);

            for (std::int64_t sIdx = firstStepIdx; sIdx < lastStepIdx; ++sIdx) {
                if (sIdx < 0) continue;
                const std::size_t patStep = static_cast<std::size_t>(sIdx) % beatDesignerSteps_;
                const std::int64_t tickPos = sIdx * kStepTicks + ((patStep & 1U) ? swingTicks : 0);
                if (tickPos < blockStartTick || tickPos >= blockEndTick) continue;

                for (const auto& lane : drumLanes_) {
                    const std::uint8_t vel = lane.stepVelocities[patStep];
                    if (vel > 0) {
                        if (lane.stepFlam[patStep]) {
                            events.push_back(MidiSequenceEvent{
                                SequencerMidiKind::Note, std::max<std::int64_t>(0, tickPos - 25), 20,
                                9, lane.drumNotePitch, static_cast<std::uint8_t>(vel / 2)});
                        }
                        events.push_back(MidiSequenceEvent{
                            SequencerMidiKind::Note, tickPos, 90, 9, lane.drumNotePitch, vel});
                    }
                }
            }
            break;
        }

        case MidiPluginAlgorithm::Chorder: {
            std::vector<MidiSequenceEvent> chordOut;
            chordOut.reserve(events.size() * 3U);
            constexpr std::array<int, 3> kTriad = {0, 4, 7}; // Major triad intervals
            for (const auto& e : events) {
                if (e.kind == SequencerMidiKind::Note && e.velocityOrVal > 0) {
                    for (int semi : kTriad) {
                        MidiSequenceEvent c = e;
                        c.pitchOrCC = static_cast<std::uint8_t>(std::clamp(static_cast<int>(e.pitchOrCC) + semi, 0, 127));
                        chordOut.push_back(c);
                    }
                } else {
                    chordOut.push_back(e);
                }
            }
            events = std::move(chordOut);
            break;
        }

        case MidiPluginAlgorithm::Compress: {
            const float threshold = 1.0f + params_[0] * 126.0f;
            const float ratio = 0.25f + params_[1] * 1.75f;
            const float gainOffset = (params_[2] - 0.5f) * 40.0f;
            for (auto& e : events) {
                if (e.kind == SequencerMidiKind::Note && e.velocityOrVal > 0) {
                    const float comp = threshold + (static_cast<float>(e.velocityOrVal) - threshold) * ratio + gainOffset;
                    e.velocityOrVal = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(comp)), 1, 127));
                }
            }
            break;
        }

        case MidiPluginAlgorithm::ContextGate: {
            const int minVel = static_cast<int>(params_[0] * 60.0f);
            const int maxVel = 64 + static_cast<int>(params_[1] * 63.0f);
            events.erase(
                std::remove_if(events.begin(), events.end(), [&](const MidiSequenceEvent& e) {
                    return e.kind == SequencerMidiKind::Note &&
                           (e.velocityOrVal < minVel || e.velocityOrVal > maxVel);
                }),
                events.end());
            break;
        }

        case MidiPluginAlgorithm::Density: {
            // Density factor: < 0.5 thins notes probabilistically; > 0.5 subdivides notes
            if (params_[0] < 0.48f) {
                events.erase(
                    std::remove_if(events.begin(), events.end(), [&](const MidiSequenceEvent& e) {
                        if (e.kind != SequencerMidiKind::Note) return false;
                        rng_ = rng_ * 1664525U + 1013904223U;
                        const float r = static_cast<float>(rng_ >> 8U) / 16777216.0f;
                        return r > (params_[0] * 2.0f);
                    }),
                    events.end());
            } else if (params_[0] > 0.52f) {
                std::vector<MidiSequenceEvent> doubled;
                for (const auto& e : events) {
                    doubled.push_back(e);
                    if (e.kind == SequencerMidiKind::Note && e.lengthTicks >= 60) {
                        MidiSequenceEvent sub = e;
                        sub.startTick += e.lengthTicks / 2;
                        sub.lengthTicks /= 2;
                        doubled.push_back(sub);
                    }
                }
                events = std::move(doubled);
            }
            break;
        }

        case MidiPluginAlgorithm::MicroTuner: {
            std::vector<MidiSequenceEvent> tuned;
            tuned.reserve(events.size() * 2U);
            for (const auto& e : events) {
                if (e.kind == SequencerMidiKind::Note && e.velocityOrVal > 0) {
                    const float cents = microTuneCents_[e.pitchOrCC % 12U];
                    if (std::abs(cents) > 0.1f) {
                        const auto pb = static_cast<std::int16_t>(std::clamp(std::lround((cents / 200.0f) * 8191.0f), -8192L, 8191L));
                        MidiSequenceEvent bend{};
                        bend.kind = SequencerMidiKind::PitchBend;
                        bend.startTick = e.startTick;
                        bend.channel = e.channel;
                        bend.pitchBend14Bit = pb;
                        tuned.push_back(bend);
                    }
                }
                tuned.push_back(e);
            }
            events = std::move(tuned);
            break;
        }

        case MidiPluginAlgorithm::MidiControl:
        case MidiPluginAlgorithm::TrackControl:
        case MidiPluginAlgorithm::TrackFX: {
            const std::uint8_t baseCC = (algo_ == MidiPluginAlgorithm::TrackFX) ? 91 : 74;
            events.push_back(MidiSequenceEvent{
                SequencerMidiKind::ControlChange, blockStartTick, 0, 0, baseCC,
                static_cast<std::uint8_t>(std::clamp(static_cast<int>(params_[0] * 127.0f), 0, 127))});
            break;
        }

        case MidiPluginAlgorithm::MidiEcho: {
            const int repeats = 1 + static_cast<int>(params_[0] * 4.0f);
            const std::int64_t delayTicks = kStepTicks * 2;
            const float velDecay = 0.35f + 0.55f * params_[1];
            const int semiTranspose = static_cast<int>((params_[2] - 0.5f) * 12.0f);

            std::vector<MidiSequenceEvent> echoed = events;
            for (const auto& e : events) {
                if (e.kind == SequencerMidiKind::Note && e.velocityOrVal > 0) {
                    float v = static_cast<float>(e.velocityOrVal);
                    for (int r = 1; r <= repeats; ++r) {
                        v *= velDecay;
                        if (v < 4.0f) break;
                        MidiSequenceEvent copy = e;
                        copy.startTick += delayTicks * r;
                        copy.pitchOrCC = static_cast<std::uint8_t>(
                            std::clamp(static_cast<int>(e.pitchOrCC) + semiTranspose * r, 0, 127));
                        copy.velocityOrVal = static_cast<std::uint8_t>(std::clamp(static_cast<int>(v), 1, 127));
                        echoed.push_back(copy);
                    }
                }
            }
            events = std::move(echoed);
            break;
        }

        case MidiPluginAlgorithm::MIDIMonitor: {
            for (const auto& e : events) {
                if (monitorLog_.size() >= 512U) {
                    monitorLog_.erase(monitorLog_.begin());
                }
                monitorLog_.push_back(e);
            }
            break;
        }

        case MidiPluginAlgorithm::Note2CC: {
            const auto targetCC = static_cast<std::uint8_t>(std::clamp(static_cast<int>(params_[0] * 127.0f), 0, 127));
            for (auto& e : events) {
                if (e.kind == SequencerMidiKind::Note) {
                    const std::uint8_t noteNum = e.pitchOrCC;
                    e.kind = SequencerMidiKind::ControlChange;
                    e.pitchOrCC = targetCC;
                    e.velocityOrVal = noteNum;
                }
            }
            break;
        }

        case MidiPluginAlgorithm::Quantizer: {
            const float strength = params_[0];
            for (auto& e : events) {
                if (e.kind == SequencerMidiKind::Note) {
                    const std::int64_t nearest =
                        ((e.startTick + kStepTicks / 2) / kStepTicks) * kStepTicks;
                    e.startTick = e.startTick + static_cast<std::int64_t>(
                        std::llround(static_cast<double>(nearest - e.startTick) * strength));
                }
            }
            break;
        }

        case MidiPluginAlgorithm::StepDesigner: {
            for (std::int64_t sIdx = blockStartTick / kStepTicks;
                 sIdx * kStepTicks < blockEndTick; ++sIdx) {
                const std::int64_t t = sIdx * kStepTicks;
                if (t < blockStartTick || sIdx < 0) continue;
                const std::size_t st = static_cast<std::size_t>(sIdx) & 31U;
                if (stepDesignerVelocities_[st] > 0) {
                    events.push_back(MidiSequenceEvent{
                        SequencerMidiKind::Note, t, 100, 0,
                        static_cast<std::uint8_t>(stepDesignerPitches_[st]),
                        stepDesignerVelocities_[st]});
                }
            }
            break;
        }

        case MidiPluginAlgorithm::Transformer: {
            LogicalEditor::applyPreset(transformerPreset_, events, clock);
            break;
        }
    }
}

// ============================================================================
// MidiPluginRegistry (All 18 Named MIDI Plug-ins)
// ============================================================================
MidiPluginRegistry::MidiPluginRegistry() {
    registerAll18MidiPlugins();
}

std::shared_ptr<IMidiInsertProcessor> MidiPluginRegistry::createByName(std::string_view exactName) const {
    for (const auto& e : entries_) {
        if (e.name == exactName) {
            return e.factory();
        }
    }
    return nullptr;
}

void MidiPluginRegistry::registerAll18MidiPlugins() {
    auto reg = [this](const char* exactName, MidiPluginAlgorithm algo) {
        entries_.push_back(MidiPluginRegistryEntry{
            .name = exactName,
            .algorithm = algo,
            .factory = [n = std::string(exactName), algo]() {
                return std::make_shared<SuiteMidiPlugin>(n, algo);
            }
        });
    };

    reg("Arpache 5",     MidiPluginAlgorithm::Arpache5);
    reg("Arpache SX",    MidiPluginAlgorithm::ArpacheSX);
    reg("Autopan",       MidiPluginAlgorithm::Autopan);
    reg("Beat Designer", MidiPluginAlgorithm::BeatDesigner);
    reg("Chorder",       MidiPluginAlgorithm::Chorder);
    reg("Compress",      MidiPluginAlgorithm::Compress);
    reg("Context Gate",  MidiPluginAlgorithm::ContextGate);
    reg("Density",       MidiPluginAlgorithm::Density);
    reg("Micro Tuner",   MidiPluginAlgorithm::MicroTuner);
    reg("MidiControl",   MidiPluginAlgorithm::MidiControl);
    reg("MidiEcho",      MidiPluginAlgorithm::MidiEcho);
    reg("MIDI Monitor",  MidiPluginAlgorithm::MIDIMonitor);
    reg("Note 2 CC",     MidiPluginAlgorithm::Note2CC);
    reg("Quantizer",     MidiPluginAlgorithm::Quantizer);
    reg("StepDesigner",  MidiPluginAlgorithm::StepDesigner);
    reg("Track Control", MidiPluginAlgorithm::TrackControl);
    reg("Track FX",      MidiPluginAlgorithm::TrackFX);
    reg("Transformer",   MidiPluginAlgorithm::Transformer);
}

} // namespace AlphaStudio::Core::DSP
