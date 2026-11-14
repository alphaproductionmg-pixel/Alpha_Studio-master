#include "AlphaStudio/Core/Instruments/VirtualInstrumentsSuite.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <sstream>

namespace AlphaStudio::Core::Instruments {

namespace {
[[nodiscard]] inline double midiNoteToHz(int note) noexcept {
    return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
}
} // namespace

// ============================================================================
// 1. HALionOneInstrument
// ============================================================================
HALionOneInstrument::HALionOneInstrument() {
    auto makeHarmonicWave = [](std::span<const float> amps) {
        constexpr std::size_t kLen = 2048;
        std::vector<float> wave(kLen, 0.0f);
        for (std::size_t n = 0; n < kLen; ++n) {
            const double ph = (2.0 * std::numbers::pi * static_cast<double>(n)) / static_cast<double>(kLen);
            double s = 0.0;
            for (std::size_t h = 0; h < amps.size(); ++h) {
                s += static_cast<double>(amps[h]) * std::sin(ph * static_cast<double>(h + 1));
            }
            wave[n] = static_cast<float>(s * 0.45);
        }
        return wave;
    };

    constexpr std::array<float, 6> kPianoHarm = {1.0f, 0.45f, 0.25f, 0.14f, 0.08f, 0.04f};
    constexpr std::array<float, 6> kStringsHarm = {1.0f, 0.50f, 0.33f, 0.25f, 0.20f, 0.16f};
    constexpr std::array<float, 6> kBassHarm = {1.0f, 0.70f, 0.30f, 0.10f, 0.02f, 0.01f};

    bank_.push_back({"ROM Bank A", "Acoustic Grand", 0.004f, 0.45f, 0.55f, 0.25f, 7500.0f, 1.0f, 4.5f, 0.02f, makeHarmonicWave(kPianoHarm)});
    bank_.push_back({"ROM Bank A", "Warm Strings",   0.120f, 0.30f, 0.85f, 0.45f, 5200.0f, 1.1f, 5.2f, 0.09f, makeHarmonicWave(kStringsHarm)});
    bank_.push_back({"ROM Bank A", "Studio Finger Bass", 0.005f, 0.25f, 0.65f, 0.15f, 2800.0f, 1.4f, 3.0f, 0.01f, makeHarmonicWave(kBassHarm)});
    channelProgramIdx_.fill(0);
}

void HALionOneInstrument::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void HALionOneInstrument::reset() noexcept {
    for (auto& v : voices_) {
        v = Voice{};
    }
}

void HALionOneInstrument::selectChannelProgram(std::uint8_t ch0To15, std::size_t bankProgramIdx) noexcept {
    if (ch0To15 < kMidiChannels && !bank_.empty()) {
        channelProgramIdx_[ch0To15] = bankProgramIdx % bank_.size();
    }
}

void HALionOneInstrument::noteOnRT(std::uint8_t midiChannel, std::uint8_t pitch, float velocity01) noexcept {
    const std::uint8_t ch = midiChannel & 0x0FU;
    Voice* chosen = &voices_[0];
    for (auto& v : voices_) {
        if (!v.active) {
            chosen = &v;
            break;
        }
    }
    *chosen = Voice{
        .active = true,
        .releasing = false,
        .channel = ch,
        .pitch = pitch,
        .velocity = std::clamp(velocity01, 0.01f, 1.0f),
        .phase = 0.0,
        .lfoPhase = 0.0,
        .env = 0.001f,
        .lpState = 0.0f
    };
}

void HALionOneInstrument::noteOffRT(std::uint8_t midiChannel, std::uint8_t pitch) noexcept {
    const std::uint8_t ch = midiChannel & 0x0FU;
    for (auto& v : voices_) {
        if (v.active && v.channel == ch && v.pitch == pitch) {
            v.releasing = true;
        }
    }
}

void HALionOneInstrument::renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept {
    outBuffer.clearRT(numFrames);
    if (outBuffer.numChannels() == 0 || bank_.empty()) {
        return;
    }
    float* outL = outBuffer.channel(0);
    float* outR = (outBuffer.numChannels() > 1) ? outBuffer.channel(1) : outL;

    for (auto& v : voices_) {
        if (!v.active) continue;
        const auto& prog = bank_[channelProgramIdx_[v.channel]];
        const std::size_t waveLen = prog.singleCycleOrPcmWave.size();
        if (waveLen == 0) continue;

        const double baseHz = midiNoteToHz(v.pitch);
        const float atkStep = 1.0f / std::max(1.0f, static_cast<float>(prog.attackSec * sampleRate_));
        const float relStep = 1.0f / std::max(1.0f, static_cast<float>(prog.releaseSec * masterReleaseScale_ * sampleRate_));
        const float lpAlpha = std::clamp(
            static_cast<float>((2.0 * std::numbers::pi * prog.cutoffHz * masterCutoffScale_) / sampleRate_),
            0.02f, 0.98f);

        for (std::uint32_t f = 0; f < numFrames; ++f) {
            if (!v.releasing) {
                v.env = std::min(1.0f, v.env + atkStep);
            } else {
                v.env -= relStep;
                if (v.env <= 0.0f) {
                    v.active = false;
                    break;
                }
            }
            v.lfoPhase += (2.0 * std::numbers::pi * prog.lfoRateHz) / sampleRate_;
            const double vibrato = 1.0 + std::sin(v.lfoPhase) * (prog.lfoPitchDepth * 0.01);
            v.phase += (baseHz * vibrato * static_cast<double>(waveLen)) / sampleRate_;
            while (v.phase >= static_cast<double>(waveLen)) {
                v.phase -= static_cast<double>(waveLen);
            }
            const auto i0 = static_cast<std::size_t>(v.phase);
            const std::size_t i1 = (i0 + 1U) % waveLen;
            const float frac = static_cast<float>(v.phase - static_cast<double>(i0));
            const float raw = prog.singleCycleOrPcmWave[i0] * (1.0f - frac) +
                              prog.singleCycleOrPcmWave[i1] * frac;
            v.lpState += lpAlpha * (raw - v.lpState);
            const float sample = v.lpState * v.env * v.velocity * 0.35f;
            outL[f] += sample;
            if (outR != outL) outR[f] += sample;
        }
    }
}

void HALionOneInstrument::setParameterNormalized(std::uint32_t paramId, float value01) noexcept {
    if (paramId == 0) masterCutoffScale_ = 0.2f + 1.8f * std::clamp(value01, 0.0f, 1.0f);
    else if (paramId == 1) masterReleaseScale_ = 0.2f + 2.5f * std::clamp(value01, 0.0f, 1.0f);
}

float HALionOneInstrument::getParameterNormalized(std::uint32_t paramId) const noexcept {
    if (paramId == 0) return std::clamp((masterCutoffScale_ - 0.2f) / 1.8f, 0.0f, 1.0f);
    if (paramId == 1) return std::clamp((masterReleaseScale_ - 0.2f) / 2.5f, 0.0f, 1.0f);
    return 0.5f;
}

// ============================================================================
// 2. GrooveAgentOneInstrument
// ============================================================================
GrooveAgentOneInstrument::GrooveAgentOneInstrument() {
    for (std::size_t p = 0; p < kNumPads; ++p) {
        pads_[p].triggerMidiNote = static_cast<std::uint8_t>(36U + p);
        pads_[p].padName = "Pad " + std::to_string(p + 1);
        // Synthesize punchy default studio drum samples (Kick, Snare, HiHat, Tom)
        constexpr std::size_t kLen = 4096;
        pads_[p].monoSamples.resize(kLen);
        std::uint32_t rng = static_cast<std::uint32_t>(0x1234567U + p * 997U);
        for (std::size_t n = 0; n < kLen; ++n) {
            const float t = static_cast<float>(n) / 48000.0f;
            rng = rng * 1664525U + 1013904223U;
            const float noise = (static_cast<float>(rng >> 8U) / 8388608.0f) - 1.0f;
            if (p % 3 == 0) {
                pads_[p].monoSamples[n] = std::sin(2.0f * std::numbers::pi_v<float> * (55.0f + 110.0f * std::exp(-t * 35.0f)) * t) * std::exp(-t * 14.0f);
            } else if (p % 3 == 1) {
                pads_[p].monoSamples[n] = (0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 185.0f * t) + 0.5f * noise) * std::exp(-t * 22.0f);
            } else {
                pads_[p].monoSamples[n] = noise * std::exp(-t * 55.0f) * 0.6f;
            }
        }
        pads_[p].sampleStartFrame = 0;
        pads_[p].sampleEndFrame = kLen;
    }
}

void GrooveAgentOneInstrument::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void GrooveAgentOneInstrument::reset() noexcept {
    for (auto& h : hits_) h = ActiveHit{};
}

void GrooveAgentOneInstrument::noteOnRT(std::uint8_t /*midiChannel*/, std::uint8_t pitch, float velocity01) noexcept {
    for (std::size_t p = 0; p < kNumPads; ++p) {
        if (pads_[p].triggerMidiNote == pitch && pads_[p].sampleEndFrame > pads_[p].sampleStartFrame) {
            ActiveHit* slot = &hits_[0];
            for (auto& h : hits_) {
                if (!h.active) { slot = &h; break; }
            }
            *slot = ActiveHit{
                .active = true,
                .padIndex = p,
                .cursor = static_cast<double>(pads_[p].sampleStartFrame),
                .step = std::pow(2.0, static_cast<double>(pads_[p].tuneSemitones) / 12.0),
                .velocity = std::clamp(velocity01, 0.01f, 1.0f)
            };
            break;
        }
    }
}

void GrooveAgentOneInstrument::noteOffRT(std::uint8_t /*midiChannel*/, std::uint8_t /*pitch*/) noexcept {}

void GrooveAgentOneInstrument::renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept {
    outBuffer.clearRT(numFrames);
    if (outBuffer.numChannels() == 0) return;
    float* outL = outBuffer.channel(0);
    float* outR = (outBuffer.numChannels() > 1) ? outBuffer.channel(1) : outL;

    for (auto& h : hits_) {
        if (!h.active) continue;
        const auto& pad = pads_[h.padIndex];
        const std::size_t endFrame = std::min(pad.sampleEndFrame, pad.monoSamples.size());
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const auto i0 = static_cast<std::size_t>(h.cursor);
            if (i0 >= endFrame) {
                h.active = false;
                break;
            }
            const float s = pad.monoSamples[i0] * h.velocity * pad.gainLinear * masterGain_;
            outL[f] += s;
            if (outR != outL) outR[f] += s;
            h.cursor += h.step;
        }
    }
}

bool GrooveAgentOneInstrument::importSampleToPad(std::size_t padIdx, const std::string& name,
                                                 std::span<const float> monoPcm) {
    if (padIdx >= kNumPads || monoPcm.empty()) return false;
    pads_[padIdx].padName = name;
    pads_[padIdx].monoSamples.assign(monoPcm.begin(), monoPcm.end());
    pads_[padIdx].sampleStartFrame = 0;
    pads_[padIdx].sampleEndFrame = pads_[padIdx].monoSamples.size();
    return true;
}

bool GrooveAgentOneInstrument::importAiffBytesToPad(std::size_t padIdx,
                                                    std::span<const std::uint8_t> aiffBytes) {
    // Clean-room big-endian FORM/AIFF 16-bit SSND parser
    if (padIdx >= kNumPads || aiffBytes.size() < 54) return false;
    if (std::memcmp(aiffBytes.data(), "FORM", 4) != 0 ||
        std::memcmp(aiffBytes.data() + 8, "AIFF", 4) != 0) {
        return false;
    }
    std::vector<float> samples;
    for (std::size_t i = 54; i + 1 < aiffBytes.size(); i += 2) {
        const auto hi = static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(aiffBytes[i]) << 8U) | aiffBytes[i + 1]);
        samples.push_back(static_cast<float>(hi) / 32768.0f);
    }
    return importSampleToPad(padIdx, "AIFF Imported Pad", samples);
}

std::size_t GrooveAgentOneInstrument::importMpcProgramDescriptor(const std::string& mpcProgramText) {
    std::istringstream iss(mpcProgramText);
    std::string line;
    std::size_t imported = 0;
    while (std::getline(iss, line) && imported < kNumPads) {
        if (line.empty()) continue;
        pads_[imported].padName = line;
        ++imported;
    }
    return imported;
}

std::size_t GrooveAgentOneInstrument::importSlicesAcrossPads(
    std::span<const DSP::AudioSliceRegion> slices, std::size_t firstPadIdx) {
    std::size_t mapped = 0;
    for (std::size_t i = 0; i < slices.size() && (firstPadIdx + i) < kNumPads; ++i) {
        importSampleToPad(firstPadIdx + i, "Slice " + std::to_string(i + 1),
                          slices[i].interleavedSliceSamples);
        ++mapped;
    }
    return mapped;
}

void GrooveAgentOneInstrument::setPadStartEndFrames(std::size_t padIdx,
                                                    std::size_t startFrame,
                                                    std::size_t endFrame) noexcept {
    if (padIdx < kNumPads) {
        const std::size_t maxF = pads_[padIdx].monoSamples.size();
        pads_[padIdx].sampleStartFrame = std::min(startFrame, maxF);
        pads_[padIdx].sampleEndFrame = std::clamp(endFrame, pads_[padIdx].sampleStartFrame, maxF);
    }
}

void GrooveAgentOneInstrument::replaceOrSwapPads(std::size_t padA, std::size_t padB) {
    if (padA < kNumPads && padB < kNumPads && padA != padB) {
        const std::uint8_t noteA = pads_[padA].triggerMidiNote;
        const std::uint8_t noteB = pads_[padB].triggerMidiNote;
        std::swap(pads_[padA], pads_[padB]);
        pads_[padA].triggerMidiNote = noteA;
        pads_[padB].triggerMidiNote = noteB;
    }
}

std::string GrooveAgentOneInstrument::serializeKitArchive() const {
    std::ostringstream oss;
    oss << "GAONE_KIT_ARCHIVE_V1\n";
    for (std::size_t i = 0; i < kNumPads; ++i) {
        oss << i << "|" << static_cast<int>(pads_[i].triggerMidiNote) << "|"
            << pads_[i].sampleStartFrame << "|" << pads_[i].sampleEndFrame << "|"
            << pads_[i].padName << "\n";
    }
    return oss.str();
}

bool GrooveAgentOneInstrument::loadKitArchive(const std::string& archiveText) {
    return archiveText.find("GAONE_KIT_ARCHIVE_V1") == 0;
}

void GrooveAgentOneInstrument::setParameterNormalized(std::uint32_t paramId, float value01) noexcept {
    if (paramId == 0) masterGain_ = std::clamp(value01, 0.0f, 1.0f) * 1.5f;
}

float GrooveAgentOneInstrument::getParameterNormalized(std::uint32_t paramId) const noexcept {
    return (paramId == 0) ? std::clamp(masterGain_ / 1.5f, 0.0f, 1.0f) : 0.5f;
}

// ============================================================================
// 3. LoopMashInstrument (8-Track Real-Time Similarity Slice Recombiner)
// ============================================================================
LoopMashInstrument::LoopMashInstrument() {
    // Populate 2 default rhythmic loops so LoopMash plays out of the box
    std::vector<float> loop0(48000, 0.0f);
    std::vector<float> loop1(48000, 0.0f);
    for (std::size_t n = 0; n < loop0.size(); ++n) {
        const float env = std::exp(-static_cast<float>(n % 6000) / 1400.0f);
        loop0[n] = std::sin(2.0f * std::numbers::pi_v<float> * 110.0f * (static_cast<float>(n) / 48000.0f)) * env * 0.5f;
        loop1[n] = std::sin(2.0f * std::numbers::pi_v<float> * 220.0f * (static_cast<float>(n) / 48000.0f)) * env * 0.4f;
    }
    loadLoopIntoTrack(0, loop0);
    loadLoopIntoTrack(1, loop1);
}

LoopMashInstrument::SliceFeature LoopMashInstrument::analyzeSlice(std::span<const float> audio) {
    SliceFeature feat;
    feat.audio.assign(audio.begin(), audio.end());
    if (audio.empty()) return feat;
    double sumSq = 0.0;
    std::size_t zc = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        sumSq += static_cast<double>(audio[i]) * static_cast<double>(audio[i]);
        if (i > 0 && ((audio[i - 1] <= 0.0f && audio[i] > 0.0f) ||
                      (audio[i - 1] >= 0.0f && audio[i] < 0.0f))) {
            ++zc;
        }
    }
    feat.rmsEnergy = static_cast<float>(std::sqrt(sumSq / static_cast<double>(audio.size())));
    feat.zeroCrossingRate = static_cast<float>(zc) / static_cast<float>(audio.size());
    return feat;
}

float LoopMashInstrument::computeSimilarity01(const SliceFeature& a, const SliceFeature& b) noexcept {
    const float dRms = std::abs(a.rmsEnergy - b.rmsEnergy);
    const float dZcr = std::abs(a.zeroCrossingRate - b.zeroCrossingRate) * 4.0f;
    return std::clamp(1.0f - (dRms + dZcr), 0.0f, 1.0f);
}

void LoopMashInstrument::loadLoopIntoTrack(std::size_t trackIdx0To7, std::span<const float> monoLoop) {
    if (trackIdx0To7 >= kMaxLoopTracks || monoLoop.size() < kSlicesPerLoop) return;
    const std::size_t sliceLen = monoLoop.size() / kSlicesPerLoop;
    for (std::size_t s = 0; s < kSlicesPerLoop; ++s) {
        slices_[trackIdx0To7][s] = analyzeSlice(monoLoop.subspan(s * sliceLen, sliceLen));
    }
    trackLoaded_[trackIdx0To7] = true;
}

void LoopMashInstrument::setTrackVolume(std::size_t trackIdx0To7, float vol01) noexcept {
    if (trackIdx0To7 < kMaxLoopTracks) {
        trackVolumes_[trackIdx0To7] = std::clamp(vol01, 0.0f, 1.0f);
    }
}

void LoopMashInstrument::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void LoopMashInstrument::reset() noexcept {
    currentSliceIndex_ = 0;
    frameWithinSlice_ = 0;
}

void LoopMashInstrument::noteOnRT(std::uint8_t /*midiChannel*/, std::uint8_t pitch, float /*velocity01*/) noexcept {
    transportPlaying_ = true;
    currentSliceIndex_ = pitch % kSlicesPerLoop;
    frameWithinSlice_ = 0;
}

void LoopMashInstrument::noteOffRT(std::uint8_t /*midiChannel*/, std::uint8_t /*pitch*/) noexcept {}

void LoopMashInstrument::renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept {
    outBuffer.clearRT(numFrames);
    if (!transportPlaying_ || !trackLoaded_[0] || outBuffer.numChannels() == 0) return;

    float* outL = outBuffer.channel(0);
    float* outR = (outBuffer.numChannels() > 1) ? outBuffer.channel(1) : outL;

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        const auto& masterSlice = slices_[0][currentSliceIndex_];
        if (masterSlice.audio.empty()) break;

        float mixedSample = masterSlice.audio[frameWithinSlice_ % masterSlice.audio.size()] * trackVolumes_[0];

        // Match & recombine slices from tracks 1..7 whose similarity >= SimilarityThreshold
        for (std::size_t t = 1; t < kMaxLoopTracks; ++t) {
            if (!trackLoaded_[t] || trackVolumes_[t] <= 0.001f) continue;
            for (std::size_t cand = 0; cand < kSlicesPerLoop; ++cand) {
                const auto& candSlice = slices_[t][cand];
                if (!candSlice.audio.empty() &&
                    computeSimilarity01(masterSlice, candSlice) >= similarityThreshold_) {
                    mixedSample += candSlice.audio[frameWithinSlice_ % candSlice.audio.size()] * trackVolumes_[t];
                    break;
                }
            }
        }

        outL[f] += mixedSample;
        if (outR != outL) outR[f] += mixedSample;

        if (++frameWithinSlice_ >= masterSlice.audio.size()) {
            frameWithinSlice_ = 0;
            currentSliceIndex_ = (currentSliceIndex_ + 1U) % kSlicesPerLoop;
        }
    }
}

void LoopMashInstrument::setParameterNormalized(std::uint32_t paramId, float value01) noexcept {
    const float v = std::clamp(value01, 0.0f, 1.0f);
    if (paramId == 0) {
        similarityThreshold_ = v;
    } else if (paramId >= 1 && paramId <= kMaxLoopTracks) {
        trackVolumes_[paramId - 1] = v;
    }
}

float LoopMashInstrument::getParameterNormalized(std::uint32_t paramId) const noexcept {
    if (paramId == 0) return similarityThreshold_;
    if (paramId >= 1 && paramId <= kMaxLoopTracks) return trackVolumes_[paramId - 1];
    return 0.0f;
}

// ============================================================================
// 4..8. UnifiedSynthesisInstrument (Prologue, Mystic, Spector, Embracer, Monologue)
// ============================================================================
UnifiedSynthesisInstrument::UnifiedSynthesisInstrument(std::string exactName, NativeSynthModel model)
    : name_(std::move(exactName)), model_(model) {
    // Default modulation matrix routing LFO -> Cutoff/Width
    modMatrix_[0] = ModMatrixSlot{true, 0, 0, 0.18f};
    modMatrix_[1] = ModMatrixSlot{true, 1, 0, 0.35f};
}

void UnifiedSynthesisInstrument::prepare(double sampleRate, std::uint32_t /*maxBlockFrames*/) {
    sampleRate_ = std::max(8000.0, sampleRate);
    reset();
}

void UnifiedSynthesisInstrument::reset() noexcept {
    for (auto& v : voices_) v = SynthVoice{};
    globalLfoPhase_ = 0.0;
}

void UnifiedSynthesisInstrument::configureModMatrixSlot(std::size_t slot0To3, std::uint8_t src,
                                                        std::uint8_t dst, float depth) noexcept {
    if (slot0To3 < modMatrix_.size()) {
        modMatrix_[slot0To3] = ModMatrixSlot{true, src, dst, std::clamp(depth, -1.0f, 1.0f)};
    }
}

void UnifiedSynthesisInstrument::noteOnRT(std::uint8_t /*midiChannel*/, std::uint8_t pitch, float velocity01) noexcept {
    const double targetHz = midiNoteToHz(pitch);

    if (model_ == NativeSynthModel::Monologue_MonoAnalogOverdrive) {
        // Monophonic voice with portamento glide!
        auto& mv = voices_[0];
        if (!mv.active) {
            mv.currentFreqHz = targetHz;
        }
        mv.active = true;
        mv.releasing = false;
        mv.pitch = pitch;
        mv.velocity = std::clamp(velocity01, 0.05f, 1.0f);
        mv.targetFreqHz = targetHz;
        mv.modEnv = 1.0f;
        return;
    }

    SynthVoice* v = &voices_[0];
    for (auto& cand : voices_) {
        if (!cand.active) { v = &cand; break; }
    }
    *v = SynthVoice{};
    v->active = true;
    v->pitch = pitch;
    v->velocity = std::clamp(velocity01, 0.05f, 1.0f);
    v->currentFreqHz = targetHz;
    v->targetFreqHz = targetHz;
    v->modEnv = 1.0f;

    // Mystic physical modeling: seed Karplus/waveguide comb lines with filtered excitation burst
    if (model_ == NativeSynthModel::Mystic_3CombPhysicalModel) {
        for (std::size_t i = 0; i < 256U; ++i) {
            noiseRng_ = noiseRng_ * 1664525U + 1013904223U;
            const float exc = ((static_cast<float>(noiseRng_ >> 8U) / 8388608.0f) - 1.0f) * v->velocity;
            v->combLine1[i] = exc;
            v->combLine2[i] = exc * 0.8f;
            v->combLine3[i] = -exc * 0.7f;
        }
    }
}

void UnifiedSynthesisInstrument::noteOffRT(std::uint8_t /*midiChannel*/, std::uint8_t pitch) noexcept {
    for (auto& v : voices_) {
        if (v.active && v.pitch == pitch) {
            v.releasing = true;
        }
    }
}

void UnifiedSynthesisInstrument::renderAudioRT(AudioBlock32& outBuffer, std::uint32_t numFrames) noexcept {
    outBuffer.clearRT(numFrames);
    const std::uint32_t chCount = outBuffer.numChannels();
    if (chCount == 0) return;

    float* outL = outBuffer.channel(0);
    float* outR = (chCount > 1) ? outBuffer.channel(1) : outL;
    float* outLs = (chCount >= 6) ? outBuffer.channel(4) : nullptr;
    float* outRs = (chCount >= 6) ? outBuffer.channel(5) : nullptr;

    const float baseCutoff = 120.0f + params_[0] * params_[0] * 11000.0f;
    const float qRes = 0.7f + params_[1] * 8.0f;

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        globalLfoPhase_ += (2.0 * std::numbers::pi * 3.2) / sampleRate_;
        if (globalLfoPhase_ >= 2.0 * std::numbers::pi) globalLfoPhase_ -= 2.0 * std::numbers::pi;
        const float lfo = std::sin(static_cast<float>(globalLfoPhase_));

        float frameL = 0.0f;
        float frameR = 0.0f;
        float frameSurround = 0.0f;

        const std::size_t voiceLimit =
            (model_ == NativeSynthModel::Monologue_MonoAnalogOverdrive) ? 1U : kMaxVoices;

        for (std::size_t vi = 0; vi < voiceLimit; ++vi) {
            auto& v = voices_[vi];
            if (!v.active) continue;

            const float atkRate = (model_ == NativeSynthModel::Embracer_SurroundPadSynth) ? 0.0004f : 0.004f;
            const float relRate = (model_ == NativeSynthModel::Embracer_SurroundPadSynth) ? 0.0003f : 0.0015f;
            if (!v.releasing) {
                v.ampEnv = std::min(1.0f, v.ampEnv + atkRate);
            } else {
                v.ampEnv -= relRate;
                if (v.ampEnv <= 0.0f) {
                    v.active = false;
                    continue;
                }
            }
            v.modEnv *= 0.9992f;

            // Glide for Monologue
            const double glideCoeff = (model_ == NativeSynthModel::Monologue_MonoAnalogOverdrive)
                ? (0.002 + (1.0 - params_[5]) * 0.05)
                : 1.0;
            v.currentFreqHz += (v.targetFreqHz - v.currentFreqHz) * glideCoeff;

            // Evaluate Modulation Matrix
            float cutoffMod = 0.0f;
            float widthMod = 0.0f;
            for (const auto& m : modMatrix_) {
                if (!m.enabled) continue;
                const float srcVal = (m.source == 0) ? lfo : ((m.source == 1) ? v.modEnv : v.velocity);
                if (m.destination == 0) cutoffMod += srcVal * m.depth * 2400.0f;
                else if (m.destination == 2) widthMod += srcVal * m.depth;
            }

            float rawSigL = 0.0f;
            float rawSigR = 0.0f;

            switch (model_) {
                case NativeSynthModel::Prologue_3OscPolySubtractive: {
                    // 3 Oscillators: Saw + Detuned Saw + Sub Pulse -> Multimode SVF
                    v.phase1 += v.currentFreqHz / sampleRate_;
                    v.phase2 += (v.currentFreqHz * 1.0055) / sampleRate_;
                    v.phase3 += (v.currentFreqHz * 0.5) / sampleRate_;
                    if (v.phase1 >= 1.0) v.phase1 -= 1.0;
                    if (v.phase2 >= 1.0) v.phase2 -= 1.0;
                    if (v.phase3 >= 1.0) v.phase3 -= 1.0;
                    const float osc1 = static_cast<float>(2.0 * v.phase1 - 1.0);
                    const float osc2 = static_cast<float>(2.0 * v.phase2 - 1.0);
                    const float osc3 = (v.phase3 < 0.5) ? 0.6f : -0.6f;
                    rawSigL = rawSigR = (osc1 + osc2 + osc3) * 0.30f;
                    break;
                }

                case NativeSynthModel::Mystic_3CombPhysicalModel: {
                    // 3 Parallel Damped Comb / Resonant Delay Lines
                    const auto d1 = std::clamp<std::size_t>(static_cast<std::size_t>(sampleRate_ / std::max(40.0, v.currentFreqHz)), 8U, kCombBufLen - 2U);
                    const auto d2 = std::clamp<std::size_t>(static_cast<std::size_t>(sampleRate_ / std::max(40.0, v.currentFreqHz * 1.498)), 8U, kCombBufLen - 2U);
                    const auto d3 = std::clamp<std::size_t>(static_cast<std::size_t>(sampleRate_ / std::max(40.0, v.currentFreqHz * 2.003)), 8U, kCombBufLen - 2U);
                    const float c1 = v.combLine1[(v.combWrite + kCombBufLen - d1) & (kCombBufLen - 1U)];
                    const float c2 = v.combLine2[(v.combWrite + kCombBufLen - d2) & (kCombBufLen - 1U)];
                    const float c3 = v.combLine3[(v.combWrite + kCombBufLen - d3) & (kCombBufLen - 1U)];
                    v.combLp1 = 0.7f * c1 + 0.3f * v.combLp1;
                    v.combLp2 = 0.7f * c2 + 0.3f * v.combLp2;
                    v.combLp3 = 0.7f * c3 + 0.3f * v.combLp3;
                    const float fb = 0.965f + 0.030f * params_[3];
                    v.combLine1[v.combWrite] = std::tanh(v.combLp1 * fb);
                    v.combLine2[v.combWrite] = std::tanh(v.combLp2 * fb);
                    v.combLine3[v.combWrite] = std::tanh(v.combLp3 * fb);
                    v.combWrite = (v.combWrite + 1U) & (kCombBufLen - 1U);
                    rawSigL = (c1 + c2 * 0.6f) * 0.5f;
                    rawSigR = (c1 + c3 * 0.6f) * 0.5f;
                    break;
                }

                case NativeSynthModel::Spector_2OscSpectralFilterBank: {
                    // 2 Rich Harmonics Oscillators -> 6-Band Morphable Spectral Filter Bank
                    v.phase1 += v.currentFreqHz / sampleRate_;
                    v.phase2 += (v.currentFreqHz * 1.008) / sampleRate_;
                    if (v.phase1 >= 1.0) v.phase1 -= 1.0;
                    if (v.phase2 >= 1.0) v.phase2 -= 1.0;
                    const float dualOsc = static_cast<float>((2.0 * v.phase1 - 1.0) + (2.0 * v.phase2 - 1.0)) * 0.35f;
                    float specSum = 0.0f;
                    const float morphShift = 0.6f + 1.4f * params_[2] + 0.25f * lfo;
                    for (std::size_t b = 0; b < 6U; ++b) {
                        const float bandFc = std::clamp((220.0f * static_cast<float>(b + 1)) * morphShift, 80.0f, static_cast<float>(sampleRate_ * 0.45));
                        const float g = std::tan(std::numbers::pi_v<float> * bandFc / static_cast<float>(sampleRate_));
                        const float k = 0.22f;
                        const float a1 = 1.0f / (1.0f + g * (g + k));
                        const float v3 = dualOsc - v.spectralBpIc2[b];
                        const float bp = a1 * v.spectralBpIc1[b] + g * a1 * v3;
                        const float lp = v.spectralBpIc2[b] + g * a1 * v.spectralBpIc1[b] + g * g * a1 * v3;
                        v.spectralBpIc1[b] = 2.0f * bp - v.spectralBpIc1[b];
                        v.spectralBpIc2[b] = 2.0f * lp - v.spectralBpIc2[b];
                        specSum += bp * ((b & 1U) ? -0.45f : 0.55f);
                    }
                    rawSigL = rawSigR = specSum;
                    break;
                }

                case NativeSynthModel::Embracer_SurroundPadSynth: {
                    // Dual Ensemble Pad Engine with Modulatable Stereo / Surround Width
                    v.phase1 += (v.currentFreqHz * 0.997) / sampleRate_;
                    v.phase2 += (v.currentFreqHz * 1.003) / sampleRate_;
                    v.phase3 += (v.currentFreqHz * 2.001) / sampleRate_;
                    if (v.phase1 >= 1.0) v.phase1 -= 1.0;
                    if (v.phase2 >= 1.0) v.phase2 -= 1.0;
                    if (v.phase3 >= 1.0) v.phase3 -= 1.0;
                    const float padA = static_cast<float>(2.0 * v.phase1 - 1.0);
                    const float padB = static_cast<float>(2.0 * v.phase2 - 1.0);
                    const float shimmer = std::sin(static_cast<float>(2.0 * std::numbers::pi * v.phase3)) * 0.35f;
                    const float width = std::clamp(params_[4] + widthMod, 0.0f, 1.0f);
                    rawSigL = (padA + padB * (1.0f - width) + shimmer) * 0.30f;
                    rawSigR = (padB + padA * (1.0f - width) - shimmer) * 0.30f;
                    frameSurround += (padA - padB) * width * v.ampEnv * v.velocity * 0.25f;
                    break;
                }

                case NativeSynthModel::Monologue_MonoAnalogOverdrive: {
                    // Punchy Mono Analog Saw/Square + Nonlinear Overdrive Stage
                    v.phase1 += v.currentFreqHz / sampleRate_;
                    v.phase2 += (v.currentFreqHz * 0.5) / sampleRate_;
                    if (v.phase1 >= 1.0) v.phase1 -= 1.0;
                    if (v.phase2 >= 1.0) v.phase2 -= 1.0;
                    const float saw = static_cast<float>(2.0 * v.phase1 - 1.0);
                    const float subSq = (v.phase2 < 0.5) ? 0.7f : -0.7f;
                    const float preDrive = (saw * 0.65f + subSq * 0.45f) * (1.5f + params_[3] * 8.0f);
                    rawSigL = rawSigR = std::tanh(preDrive) * 0.55f;
                    break;
                }
            }

            // Resonant SVF Filter Stage
            const float fc = std::clamp(baseCutoff + cutoffMod, 40.0f, static_cast<float>(sampleRate_ * 0.46));
            const float g = std::tan(std::numbers::pi_v<float> * fc / static_cast<float>(sampleRate_));
            const float k = 1.0f / std::max(0.25f, qRes);
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float midIn = 0.5f * (rawSigL + rawSigR);
            const float v3 = midIn - v.svfIc2;
            const float v1 = a1 * v.svfIc1 + g * a1 * v3;
            const float lp = v.svfIc2 + g * a1 * v.svfIc1 + g * g * a1 * v3;
            v.svfIc1 = 2.0f * v1 - v.svfIc1;
            v.svfIc2 = 2.0f * lp - v.svfIc2;

            const float sideDiff = 0.5f * (rawSigL - rawSigR);
            const float envScale = v.ampEnv * v.velocity * 0.45f;
            frameL += (lp + sideDiff) * envScale;
            frameR += (lp - sideDiff) * envScale;
        }

        outL[f] += frameL;
        if (outR != outL) outR[f] += frameR;
        if (outLs != nullptr && outRs != nullptr) {
            outLs[f] += frameSurround;
            outRs[f] -= frameSurround;
        }
    }
}

void UnifiedSynthesisInstrument::setParameterNormalized(std::uint32_t paramId, float value01) noexcept {
    if (paramId < params_.size()) {
        params_[paramId] = std::clamp(value01, 0.0f, 1.0f);
    }
}

float UnifiedSynthesisInstrument::getParameterNormalized(std::uint32_t paramId) const noexcept {
    return (paramId < params_.size()) ? params_[paramId] : 0.0f;
}

// ============================================================================
// InstrumentRegistry (All 8 Native Clean-Room Virtual Instruments)
// ============================================================================
InstrumentRegistry::InstrumentRegistry() {
    entries_.push_back({"HALion ONE", []() { return std::make_shared<HALionOneInstrument>(); }});
    entries_.push_back({"Groove Agent ONE", []() { return std::make_shared<GrooveAgentOneInstrument>(); }});
    entries_.push_back({"LoopMash", []() { return std::make_shared<LoopMashInstrument>(); }});
    entries_.push_back({"Prologue", []() {
        return std::make_shared<UnifiedSynthesisInstrument>("Prologue", NativeSynthModel::Prologue_3OscPolySubtractive);
    }});
    entries_.push_back({"Mystic", []() {
        return std::make_shared<UnifiedSynthesisInstrument>("Mystic", NativeSynthModel::Mystic_3CombPhysicalModel);
    }});
    entries_.push_back({"Spector", []() {
        return std::make_shared<UnifiedSynthesisInstrument>("Spector", NativeSynthModel::Spector_2OscSpectralFilterBank);
    }});
    entries_.push_back({"Embracer", []() {
        return std::make_shared<UnifiedSynthesisInstrument>("Embracer", NativeSynthModel::Embracer_SurroundPadSynth);
    }});
    entries_.push_back({"Monologue", []() {
        return std::make_shared<UnifiedSynthesisInstrument>("Monologue", NativeSynthModel::Monologue_MonoAnalogOverdrive);
    }});
}

std::shared_ptr<IVirtualInstrument> InstrumentRegistry::createByName(std::string_view exactName) const {
    for (const auto& e : entries_) {
        if (e.name == exactName) {
            return e.factory();
        }
    }
    return nullptr;
}

} // namespace AlphaStudio::Core::Instruments
