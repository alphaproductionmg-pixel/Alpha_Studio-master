#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace AlphaStudio::Core {

// ============================================================================
// MusicalClock
// ============================================================================
MusicalClock::MusicalClock() {
    tempoMap_.push_back(TempoEventNode{0, 120.0, false});
    signatureMap_.push_back(TimeSignatureNode{1, 0, 4, 4});
}

void MusicalClock::setMidiDisplayResolutionPPQ(std::int32_t displayPpq) noexcept {
    displayPpq_ = std::clamp(displayPpq, 24, 960);
}

void MusicalClock::setBaseTempo(double bpm) {
    const double clamped = std::clamp(bpm, 20.0, 360.0);
    if (tempoMap_.empty()) {
        tempoMap_.push_back(TempoEventNode{0, clamped, false});
    } else {
        tempoMap_.front().bpm = clamped;
    }
}

void MusicalClock::addOrUpdateTempoNode(std::int64_t tickPos, double bpm, bool ramp) {
    const std::int64_t t = std::max<std::int64_t>(0, tickPos);
    const double b = std::clamp(bpm, 20.0, 360.0);
    for (auto& node : tempoMap_) {
        if (node.tickPosition == t) {
            node.bpm = b;
            node.rampToNext = ramp;
            return;
        }
    }
    tempoMap_.push_back(TempoEventNode{t, b, ramp});
    std::sort(tempoMap_.begin(), tempoMap_.end(),
              [](const TempoEventNode& a, const TempoEventNode& bNode) {
                  return a.tickPosition < bNode.tickPosition;
              });
}

double MusicalClock::tempoAtTick(std::int64_t tickPos) const noexcept {
    if (tempoMap_.empty()) {
        return 120.0;
    }
    if (tickPos <= tempoMap_.front().tickPosition) {
        return tempoMap_.front().bpm;
    }
    for (std::size_t i = 0; i + 1 < tempoMap_.size(); ++i) {
        const auto& cur = tempoMap_[i];
        const auto& nxt = tempoMap_[i + 1];
        if (tickPos >= cur.tickPosition && tickPos < nxt.tickPosition) {
            if (!cur.rampToNext) {
                return cur.bpm;
            }
            const double alpha = static_cast<double>(tickPos - cur.tickPosition) /
                                 static_cast<double>(nxt.tickPosition - cur.tickPosition);
            return cur.bpm + alpha * (nxt.bpm - cur.bpm);
        }
    }
    return tempoMap_.back().bpm;
}

bool MusicalClock::applyTimeWarpAtTick(std::int64_t musicalTick, std::int64_t targetSampleFrame) {
    if (musicalTick <= 0 || targetSampleFrame <= 0 || sampleRate_ <= 0.0 || tempoMap_.empty()) {
        return false;
    }
    // Find the tempo node strictly preceding musicalTick
    std::size_t prevIdx = 0;
    for (std::size_t i = 0; i < tempoMap_.size(); ++i) {
        if (tempoMap_[i].tickPosition < musicalTick) {
            prevIdx = i;
        }
    }
    const std::int64_t prevTick = tempoMap_[prevIdx].tickPosition;
    const std::int64_t prevSamples = ticksToSamples(prevTick);
    const std::int64_t deltaTicks = musicalTick - prevTick;
    const std::int64_t deltaSamples = targetSampleFrame - prevSamples;
    if (deltaTicks <= 0 || deltaSamples <= 0) {
        return false;
    }

    const double deltaSeconds = static_cast<double>(deltaSamples) / sampleRate_;
    const double quarterNotes = static_cast<double>(deltaTicks) / static_cast<double>(kDefaultInternalPPQ);
    const double warpedBpm = (quarterNotes * 60.0) / deltaSeconds;
    if (warpedBpm < 20.0 || warpedBpm > 360.0) {
        return false;
    }

    const double downstreamBpm = tempoAtTick(musicalTick);
    tempoMap_[prevIdx].bpm = warpedBpm;
    tempoMap_[prevIdx].rampToNext = false;
    addOrUpdateTempoNode(musicalTick, downstreamBpm, false);
    return true;
}

void MusicalClock::addOrUpdateSignatureNode(std::int32_t barNumber,
                                            std::int32_t numerator,
                                            std::int32_t denominator) {
    const std::int32_t bar = std::max(1, barNumber);
    const std::int32_t num = std::clamp(numerator, 1, 32);
    const std::int32_t den = std::clamp(denominator, 1, 32);
    for (auto& sig : signatureMap_) {
        if (sig.barNumber == bar) {
            sig.numerator = num;
            sig.denominator = den;
            recalculateSignatureTicks();
            return;
        }
    }
    signatureMap_.push_back(TimeSignatureNode{bar, 0, num, den});
    std::sort(signatureMap_.begin(), signatureMap_.end(),
              [](const TimeSignatureNode& a, const TimeSignatureNode& b) {
                  return a.barNumber < b.barNumber;
              });
    recalculateSignatureTicks();
}

void MusicalClock::recalculateSignatureTicks() noexcept {
    if (signatureMap_.empty()) {
        return;
    }
    signatureMap_.front().barNumber = 1;
    signatureMap_.front().tickPosition = 0;
    for (std::size_t i = 1; i < signatureMap_.size(); ++i) {
        const auto& prev = signatureMap_[i - 1];
        const std::int64_t barsSpan = signatureMap_[i].barNumber - prev.barNumber;
        const std::int64_t ticksPerBar =
            (static_cast<std::int64_t>(prev.numerator) * kDefaultInternalPPQ * 4LL) /
            std::max<std::int32_t>(1, prev.denominator);
        signatureMap_[i].tickPosition = prev.tickPosition + barsSpan * ticksPerBar;
    }
}

TimeSignatureNode MusicalClock::signatureAtTick(std::int64_t tickPos) const noexcept {
    if (signatureMap_.empty()) {
        return TimeSignatureNode{1, 0, 4, 4};
    }
    TimeSignatureNode chosen = signatureMap_.front();
    for (const auto& s : signatureMap_) {
        if (tickPos >= s.tickPosition) {
            chosen = s;
        } else {
            break;
        }
    }
    return chosen;
}

std::int64_t MusicalClock::ticksToSamples(std::int64_t tickPos) const noexcept {
    if (tickPos <= 0 || tempoMap_.empty()) {
        return 0;
    }
    double totalSeconds = 0.0;
    for (std::size_t i = 0; i < tempoMap_.size(); ++i) {
        const std::int64_t segStart = tempoMap_[i].tickPosition;
        if (tickPos <= segStart) {
            break;
        }
        const bool hasNext = (i + 1 < tempoMap_.size());
        const std::int64_t segEnd = hasNext ? std::min(tickPos, tempoMap_[i + 1].tickPosition) : tickPos;
        const double segTicks = static_cast<double>(segEnd - segStart);
        const double avgBpm = (hasNext && tempoMap_[i].rampToNext)
            ? 0.5 * (tempoMap_[i].bpm + tempoAtTick(segEnd))
            : tempoMap_[i].bpm;
        totalSeconds += (segTicks / static_cast<double>(kDefaultInternalPPQ)) * (60.0 / std::max(1.0, avgBpm));
    }
    return static_cast<std::int64_t>(std::llround(totalSeconds * sampleRate_));
}

std::int64_t MusicalClock::samplesToTicks(std::int64_t samplePos) const noexcept {
    if (samplePos <= 0 || sampleRate_ <= 0.0) {
        return 0;
    }
    // Binary search inversion over monotonic ticksToSamples
    std::int64_t lo = 0;
    std::int64_t hi = std::max<std::int64_t>(kDefaultInternalPPQ * 16LL,
        static_cast<std::int64_t>((static_cast<double>(samplePos) / sampleRate_) * 6.0 * kDefaultInternalPPQ));
    while (ticksToSamples(hi) < samplePos) {
        hi *= 2;
    }
    while (lo < hi) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        if (ticksToSamples(mid) < samplePos) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

BarBeatSubbeatTick MusicalClock::tickToBBST(std::int64_t internalTick) const noexcept {
    const std::int64_t clampedTick = std::max<std::int64_t>(0, internalTick);
    const TimeSignatureNode sig = signatureAtTick(clampedTick);
    const std::int64_t ticksPerBeat =
        (static_cast<std::int64_t>(kDefaultInternalPPQ) * 4LL) / std::max(1, sig.denominator);
    const std::int64_t ticksPerBar = ticksPerBeat * sig.numerator;
    const std::int64_t rel = clampedTick - sig.tickPosition;

    const std::int64_t barOffset = rel / std::max<std::int64_t>(1, ticksPerBar);
    const std::int64_t inBar = rel % std::max<std::int64_t>(1, ticksPerBar);
    const std::int64_t beatIdx = inBar / std::max<std::int64_t>(1, ticksPerBeat);
    const std::int64_t inBeat = inBar % std::max<std::int64_t>(1, ticksPerBeat);
    const std::int64_t subIdx = inBeat / kSixteenthNoteTicks;
    const std::int64_t remInternalTicks = inBeat % kSixteenthNoteTicks;

    // Scale remaining ticks into active MIDI Display Resolution [24..960 PPQ]
    const std::int32_t displayTick = static_cast<std::int32_t>(
        (remInternalTicks * displayPpq_) / kDefaultInternalPPQ);

    return BarBeatSubbeatTick{
        .bars = static_cast<std::int32_t>(sig.barNumber + barOffset),
        .beats = static_cast<std::int32_t>(beatIdx + 1),
        .subbeats = static_cast<std::int32_t>(subIdx + 1),
        .ticks = displayTick
    };
}

std::int64_t MusicalClock::bbstToTick(const BarBeatSubbeatTick& bbst) const noexcept {
    TimeSignatureNode sig{1, 0, 4, 4};
    for (const auto& s : signatureMap_) {
        if (bbst.bars >= s.barNumber) {
            sig = s;
        }
    }
    const std::int64_t ticksPerBeat =
        (static_cast<std::int64_t>(kDefaultInternalPPQ) * 4LL) / std::max(1, sig.denominator);
    const std::int64_t ticksPerBar = ticksPerBeat * sig.numerator;
    const std::int64_t internalRemTicks =
        (static_cast<std::int64_t>(bbst.ticks) * kDefaultInternalPPQ) / std::max(1, displayPpq_);

    return sig.tickPosition +
           static_cast<std::int64_t>(std::max(0, bbst.bars - sig.barNumber)) * ticksPerBar +
           static_cast<std::int64_t>(std::max(0, bbst.beats - 1)) * ticksPerBeat +
           static_cast<std::int64_t>(std::max(0, bbst.subbeats - 1)) * kSixteenthNoteTicks +
           internalRemTicks;
}

std::string MusicalClock::formatBBST(std::int64_t internalTick) const {
    const auto b = tickToBBST(internalTick);
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%03d", b.bars, b.beats, b.subbeats, b.ticks);
    return std::string(buf);
}

std::int64_t MusicalClock::parseBBST(const std::string& formatted) const noexcept {
    BarBeatSubbeatTick b{};
    if (std::sscanf(formatted.c_str(), "%d.%d.%d.%d", &b.bars, &b.beats, &b.subbeats, &b.ticks) == 4) {
        return bbstToTick(b);
    }
    return 0;
}

// ============================================================================
// VirtualPCKeyboard
// ============================================================================
void VirtualPCKeyboard::setBaseOctave(int octave0To8) noexcept {
    baseOctave_ = std::clamp(octave0To8, 0, 8);
}

void VirtualPCKeyboard::setVelocity(std::uint8_t vel1To127) noexcept {
    velocity_ = static_cast<std::uint8_t>(std::clamp<int>(vel1To127, 1, 127));
}

bool VirtualPCKeyboard::translateKeyToMidi(char keyAscii, bool keyDown,
                                           std::int64_t currentTick,
                                           MidiSequenceEvent& outEvent) const noexcept {
    if (!enabled_) {
        return false;
    }
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(keyAscii)));
    constexpr std::string_view kKeyLayout = "zsxdcvgbhnjmq2w3er5t6y7u";
    const auto pos = kKeyLayout.find(lower);
    if (pos == std::string_view::npos) {
        return false;
    }
    const int midiNote = std::clamp((baseOctave_ + 2) * 12 + static_cast<int>(pos), 0, 127);
    outEvent.kind = SequencerMidiKind::Note;
    outEvent.startTick = currentTick;
    outEvent.lengthTicks = 120;
    outEvent.channel = 0;
    outEvent.pitchOrCC = static_cast<std::uint8_t>(midiNote);
    outEvent.velocityOrVal = keyDown ? velocity_ : 0;
    return true;
}

} // namespace AlphaStudio::Core
