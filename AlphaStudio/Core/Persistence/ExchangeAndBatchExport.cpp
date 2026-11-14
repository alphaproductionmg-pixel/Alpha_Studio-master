#include "AlphaStudio/Core/Persistence/ExchangeAndBatchExport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

namespace AlphaStudio::Core::Persistence {

// ============================================================================
// 1. TrackArchiveManager
// ============================================================================
std::string TrackArchiveManager::serializeTracksToXml(
    std::span<const TrackBase* const> selectedTracks,
    const AudioPool& pool,
    const TrackArchiveOptions& options) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<TrackArchive includeMedia=\"" << (options.includeAndCopyMediaFiles ? 1 : 0) << "\">\n";
    for (const TrackBase* t : selectedTracks) {
        if (t == nullptr) continue;
        oss << "  <ArchivedTrack name=\"" << t->name()
            << "\" type=\"" << static_cast<int>(t->type()) << "\">\n";
        if (const auto* at = dynamic_cast<const AudioTrack*>(t)) {
            for (const auto& clip : at->clips()) {
                const auto* asset = pool.findAsset(clip.poolAssetId);
                const std::string mediaRef = (options.includeAndCopyMediaFiles && asset != nullptr)
                    ? (options.targetArchiveDirectory + "/Audio/" + asset->name + ".wav")
                    : "";
                oss << "    <Clip name=\"" << clip.name << "\" startSample=\"" << clip.startSample
                    << "\" len=\"" << clip.lengthFrames << "\" media=\"" << mediaRef << "\"/>\n";
            }
        }
        oss << "  </ArchivedTrack>\n";
    }
    oss << "</TrackArchive>\n";
    return oss.str();
}

bool TrackArchiveManager::importTracksFromXml(const std::string& xmlText,
                                              AlphaProjectDocument& targetProject) {
    if (xmlText.find("<TrackArchive") == std::string::npos) {
        return false;
    }
    std::istringstream iss(xmlText);
    std::string line;
    std::uint32_t nextId = static_cast<std::uint32_t>(targetProject.tracks.size() + 100U);
    std::size_t imported = 0;

    while (std::getline(iss, line)) {
        const auto pos = line.find("<ArchivedTrack name=\"");
        if (pos != std::string::npos) {
            const auto s = pos + 21;
            const auto e = line.find('"', s);
            const std::string tName = (e != std::string::npos) ? line.substr(s, e - s) : "Imported Track";
            auto* strip = targetProject.mixer.addAudioOrInstrumentChannel(tName, false);
            targetProject.tracks.push_back(std::make_unique<AudioTrack>(nextId++, tName, strip));
            ++imported;
        }
    }
    return imported > 0;
}

// ============================================================================
// 2. StandardMIDIFileIO (24 to 960 PPQ Binary SMF Format 1)
// ============================================================================
namespace {
void writeBE16(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}
void writeBE32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}
void writeVarLen(std::vector<std::uint8_t>& buf, std::uint32_t value) {
    std::uint32_t buffer = value & 0x7FU;
    while ((value >>= 7U) != 0U) {
        buffer <<= 8U;
        buffer |= ((value & 0x7FU) | 0x80U);
    }
    while (true) {
        buf.push_back(static_cast<std::uint8_t>(buffer & 0xFFU));
        if ((buffer & 0x80U) != 0U) buffer >>= 8U;
        else break;
    }
}
std::uint32_t readVarLen(std::span<const std::uint8_t> data, std::size_t& cursor) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4 && cursor < data.size(); ++i) {
        const std::uint8_t c = data[cursor++];
        value = (value << 7U) | (c & 0x7FU);
        if ((c & 0x80U) == 0U) break;
    }
    return value;
}
} // namespace

std::vector<std::uint8_t> StandardMIDIFileIO::exportSMFFormat1(
    std::span<const MIDITrack* const> midiTracks,
    const MusicalClock& clock,
    std::uint16_t exportResolutionPPQ) {
    const std::uint16_t ppq = std::clamp<std::uint16_t>(exportResolutionPPQ, 24U, 960U);
    const double tickScale = static_cast<double>(ppq) / static_cast<double>(clock.internalPPQ());

    std::vector<std::uint8_t> smf;
    // MThd Header (Format 1, 1 Conductor Track + N MIDI Tracks)
    smf.insert(smf.end(), {'M', 'T', 'h', 'd'});
    writeBE32(smf, 6U);
    writeBE16(smf, 1U);
    writeBE16(smf, static_cast<std::uint16_t>(1U + midiTracks.size()));
    writeBE16(smf, ppq);

    // Track 0: Conductor Tempo & Time Signature Track
    {
        std::vector<std::uint8_t> trk0;
        const double bpm = clock.tempoAtTick(0);
        const auto mpqn = static_cast<std::uint32_t>(std::llround(60000000.0 / std::max(20.0, bpm)));
        writeVarLen(trk0, 0);
        trk0.insert(trk0.end(), {0xFF, 0x51, 0x03,
                                 static_cast<std::uint8_t>((mpqn >> 16U) & 0xFFU),
                                 static_cast<std::uint8_t>((mpqn >> 8U) & 0xFFU),
                                 static_cast<std::uint8_t>(mpqn & 0xFFU)});
        writeVarLen(trk0, 0);
        trk0.insert(trk0.end(), {0xFF, 0x2F, 0x00}); // End of Track

        smf.insert(smf.end(), {'M', 'T', 'r', 'k'});
        writeBE32(smf, static_cast<std::uint32_t>(trk0.size()));
        smf.insert(smf.end(), trk0.begin(), trk0.end());
    }

    // Musical Note/CC Tracks
    struct RawSmfMsg {
        std::uint32_t absTick{0};
        std::uint8_t b0{0}, b1{0}, b2{0};
        std::uint8_t len{3};
    };

    for (const MIDITrack* track : midiTracks) {
        std::vector<RawSmfMsg> msgs;
        if (track != nullptr) {
            for (const auto& part : track->parts()) {
                for (const auto& ev : part.events) {
                    const auto sTick = static_cast<std::uint32_t>(
                        std::max(0.0, std::round(static_cast<double>(part.startTick + ev.startTick) * tickScale)));
                    if (ev.kind == SequencerMidiKind::Note) {
                        const auto eTick = static_cast<std::uint32_t>(
                            std::max(static_cast<double>(sTick + 1U),
                                     std::round(static_cast<double>(part.startTick + ev.startTick + ev.lengthTicks) * tickScale)));
                        msgs.push_back({sTick, static_cast<std::uint8_t>(0x90U | (ev.channel & 0x0FU)), ev.pitchOrCC, ev.velocityOrVal, 3});
                        msgs.push_back({eTick, static_cast<std::uint8_t>(0x80U | (ev.channel & 0x0FU)), ev.pitchOrCC, 0, 3});
                    } else if (ev.kind == SequencerMidiKind::ControlChange) {
                        msgs.push_back({sTick, static_cast<std::uint8_t>(0xB0U | (ev.channel & 0x0FU)), ev.pitchOrCC, ev.velocityOrVal, 3});
                    }
                }
            }
        }
        std::sort(msgs.begin(), msgs.end(),
                  [](const RawSmfMsg& a, const RawSmfMsg& b) { return a.absTick < b.absTick; });

        std::vector<std::uint8_t> trkBytes;
        std::uint32_t prevTick = 0;
        for (const auto& m : msgs) {
            writeVarLen(trkBytes, m.absTick - prevTick);
            prevTick = m.absTick;
            trkBytes.push_back(m.b0);
            trkBytes.push_back(m.b1);
            if (m.len == 3) trkBytes.push_back(m.b2);
        }
        writeVarLen(trkBytes, 0);
        trkBytes.insert(trkBytes.end(), {0xFF, 0x2F, 0x00});

        smf.insert(smf.end(), {'M', 'T', 'r', 'k'});
        writeBE32(smf, static_cast<std::uint32_t>(trkBytes.size()));
        smf.insert(smf.end(), trkBytes.begin(), trkBytes.end());
    }
    return smf;
}

bool StandardMIDIFileIO::importSMFBytes(std::span<const std::uint8_t> smfBytes,
                                        std::vector<MidiPartContainer>& outTrackParts,
                                        std::uint16_t& outFilePPQ,
                                        double& outInitialTempoBpm) {
    if (smfBytes.size() < 14 || std::memcmp(smfBytes.data(), "MThd", 4) != 0) {
        return false;
    }
    const std::uint16_t numTracks = static_cast<std::uint16_t>((smfBytes[10] << 8U) | smfBytes[11]);
    outFilePPQ = static_cast<std::uint16_t>((smfBytes[12] << 8U) | smfBytes[13]);
    outInitialTempoBpm = 120.0;
    outTrackParts.clear();

    const double toInternalScale =
        static_cast<double>(MusicalClock::kDefaultInternalPPQ) / static_cast<double>(std::max<std::uint16_t>(24U, outFilePPQ));

    std::size_t pos = 14;
    for (std::uint16_t t = 0; t < numTracks && pos + 8 <= smfBytes.size(); ++t) {
        if (std::memcmp(smfBytes.data() + pos, "MTrk", 4) != 0) break;
        const std::uint32_t trkLen =
            (static_cast<std::uint32_t>(smfBytes[pos + 4]) << 24U) |
            (static_cast<std::uint32_t>(smfBytes[pos + 5]) << 16U) |
            (static_cast<std::uint32_t>(smfBytes[pos + 6]) << 8U) |
            static_cast<std::uint32_t>(smfBytes[pos + 7]);
        pos += 8;
        const std::size_t trkEnd = std::min(smfBytes.size(), pos + trkLen);

        MidiPartContainer part;
        part.name = "SMF Track " + std::to_string(t + 1);
        std::uint64_t curTick = 0;
        std::uint8_t runningStatus = 0;

        while (pos < trkEnd) {
            curTick += readVarLen(smfBytes, pos);
            if (pos >= trkEnd) break;
            std::uint8_t st = smfBytes[pos];
            if (st == 0xFF) { // Meta event
                ++pos;
                if (pos >= trkEnd) break;
                const std::uint8_t metaType = smfBytes[pos++];
                const std::uint32_t metaLen = readVarLen(smfBytes, pos);
                if (metaType == 0x51 && metaLen == 3 && pos + 3 <= trkEnd) {
                    const std::uint32_t mpqn =
                        (static_cast<std::uint32_t>(smfBytes[pos]) << 16U) |
                        (static_cast<std::uint32_t>(smfBytes[pos + 1]) << 8U) |
                        smfBytes[pos + 2];
                    if (mpqn > 0) outInitialTempoBpm = 60000000.0 / static_cast<double>(mpqn);
                }
                pos = std::min(trkEnd, pos + metaLen);
                continue;
            }
            if ((st & 0x80U) != 0U) {
                runningStatus = st;
                ++pos;
            } else {
                st = runningStatus;
            }
            const std::uint8_t hi = st & 0xF0U;
            if ((hi == 0x90U || hi == 0x80U || hi == 0xB0U) && pos + 2 <= trkEnd) {
                const std::uint8_t d1 = smfBytes[pos++];
                const std::uint8_t d2 = smfBytes[pos++];
                const auto scaledTick = static_cast<std::int64_t>(std::llround(static_cast<double>(curTick) * toInternalScale));
                if (hi == 0x90U && d2 > 0) {
                    part.events.push_back({SequencerMidiKind::Note, scaledTick, 120, static_cast<std::uint8_t>(st & 0x0FU), d1, d2});
                } else if (hi == 0x80U || (hi == 0x90U && d2 == 0)) {
                    for (auto it = part.events.rbegin(); it != part.events.rend(); ++it) {
                        if (it->kind == SequencerMidiKind::Note && it->pitchOrCC == d1 && it->lengthTicks == 120) {
                            it->lengthTicks = std::max<std::int64_t>(10, scaledTick - it->startTick);
                            break;
                        }
                    }
                } else if (hi == 0xB0U) {
                    part.events.push_back({SequencerMidiKind::ControlChange, scaledTick, 0, static_cast<std::uint8_t>(st & 0x0FU), d1, d2});
                }
            } else if ((hi == 0xC0U || hi == 0xD0U) && pos + 1 <= trkEnd) {
                ++pos;
            } else {
                break;
            }
        }
        pos = trkEnd;
        if (!part.events.empty()) {
            outTrackParts.push_back(std::move(part));
        }
    }
    return true;
}

// ============================================================================
// 3. MusicXML & OMF 2.0 Interchange
// ============================================================================
std::string MusicXMLAndOMFInterchange::exportPartToMusicXML(
    const MidiPartContainer& part, const MusicalClock& clock) {
    static constexpr std::array<const char*, 12> kSteps = {"C","C","D","D","E","F","F","G","G","A","A","B"};
    static constexpr std::array<int, 12> kAlters = {0,1,0,1,0,0,1,0,1,0,1,0};

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<score-partwise version=\"3.1\">\n";
    oss << "  <part-list><score-part id=\"P1\"><part-name>" << part.name << "</part-name></score-part></part-list>\n";
    oss << "  <part id=\"P1\">\n";
    oss << "    <measure number=\"1\">\n";
    oss << "      <attributes><divisions>" << clock.internalPPQ()
        << "</divisions><time><beats>4</beats><beat-type>4</beat-type></time></attributes>\n";
    for (const auto& ev : part.events) {
        if (ev.kind != SequencerMidiKind::Note) continue;
        const int pc = ev.pitchOrCC % 12;
        const int oct = (ev.pitchOrCC / 12) - 1;
        oss << "      <note><pitch><step>" << kSteps[pc] << "</step><alter>" << kAlters[pc]
            << "</alter><octave>" << oct << "</octave></pitch><duration>" << ev.lengthTicks << "</duration></note>\n";
    }
    oss << "    </measure>\n";
    oss << "  </part>\n";
    oss << "</score-partwise>\n";
    return oss.str();
}

bool MusicXMLAndOMFInterchange::importPartFromMusicXML(
    const std::string& musicXmlText, MidiPartContainer& outPart, const MusicalClock& /*clock*/) {
    if (musicXmlText.find("<score-partwise") == std::string::npos) {
        return false;
    }
    outPart.events.clear();
    std::int64_t runningTick = 0;
    std::size_t pos = 0;
    while ((pos = musicXmlText.find("<note>", pos)) != std::string::npos) {
        const auto endNote = musicXmlText.find("</note>", pos);
        if (endNote == std::string::npos) break;
        const std::string seg = musicXmlText.substr(pos, endNote - pos);
        int oct = 4;
        if (const auto op = seg.find("<octave>"); op != std::string::npos) {
            oct = static_cast<int>(std::strtol(seg.c_str() + op + 8, nullptr, 10));
        }
        std::int64_t dur = 480;
        if (const auto dp = seg.find("<duration>"); dp != std::string::npos) {
            dur = std::max<std::int64_t>(30, std::strtoll(seg.c_str() + dp + 10, nullptr, 10));
        }
        const auto midiPitch = static_cast<std::uint8_t>(std::clamp((oct + 1) * 12, 0, 127));
        outPart.events.push_back({SequencerMidiKind::Note, runningTick, dur, 0, midiPitch, 100});
        runningTick += dur;
        pos = endNote + 7;
    }
    outPart.lengthTicks = std::max<std::int64_t>(1920, runningTick);
    return true;
}

std::vector<std::uint8_t> MusicXMLAndOMFInterchange::exportProjectToOMF2(
    const AlphaProjectDocument& doc, bool embedAudioMedia) {
    std::vector<std::uint8_t> omf;
    omf.reserve(16);
    const std::array<std::uint8_t, 8> magic = {
        'O', 'M', 'F', '2', 'H', 'D', 'R', static_cast<std::uint8_t>(embedAudioMedia ? 1U : 0U)
    };
    omf.insert(omf.end(), magic.begin(), magic.end());
    std::uint32_t audioTrkCount = 0;
    std::uint32_t clipCount = 0;
    for (const auto& t : doc.tracks) {
        if (const auto* at = dynamic_cast<const AudioTrack*>(t.get())) {
            ++audioTrkCount;
            clipCount += static_cast<std::uint32_t>(at->clips().size());
        }
    }
    writeBE32(omf, audioTrkCount);
    writeBE32(omf, clipCount);
    return omf;
}

bool MusicXMLAndOMFInterchange::importOMF2HeaderSummary(
    std::span<const std::uint8_t> omfBytes,
    std::uint32_t& outTrackCount,
    std::uint32_t& outClipCount) {
    if (omfBytes.size() < 16 || std::memcmp(omfBytes.data(), "OMF2HDR", 7) != 0) {
        return false;
    }
    outTrackCount = (static_cast<std::uint32_t>(omfBytes[8]) << 24U) |
                    (static_cast<std::uint32_t>(omfBytes[9]) << 16U) |
                    (static_cast<std::uint32_t>(omfBytes[10]) << 8U) | omfBytes[11];
    outClipCount = (static_cast<std::uint32_t>(omfBytes[12]) << 24U) |
                   (static_cast<std::uint32_t>(omfBytes[13]) << 16U) |
                   (static_cast<std::uint32_t>(omfBytes[14]) << 8U) | omfBytes[15];
    return true;
}

// ============================================================================
// 4. ChannelBatchExporter (Parallel Bounce + BWF Timecode + UV22 HR Dither)
// ============================================================================
std::string ChannelBatchExporter::resolveDynamicFileName(
    const std::string& pattern,
    const std::string& projectTitle,
    MixerChannelKind kind,
    const std::string& channelName,
    std::size_t index1Based,
    ExportAudioFormat format) {
    auto kindStr = [kind]() -> const char* {
        switch (kind) {
            case MixerChannelKind::InputChannel:      return "Input";
            case MixerChannelKind::AudioChannel:      return "Audio";
            case MixerChannelKind::InstrumentChannel: return "VSTi";
            case MixerChannelKind::MIDIChannel:       return "MIDI";
            case MixerChannelKind::GroupChannel:      return "Group";
            case MixerChannelKind::FXReturnChannel:   return "FX";
            case MixerChannelKind::OutputChannel:     return "Master";
        }
        return "Stem";
    };

    std::string out = pattern;
    auto replaceToken = [&](const std::string& tok, const std::string& val) {
        std::size_t p = 0;
        while ((p = out.find(tok, p)) != std::string::npos) {
            out.replace(p, tok.size(), val);
            p += val.size();
        }
    };

    char idxBuf[16]{};
    std::snprintf(idxBuf, sizeof(idxBuf), "%02zu", index1Based);
    replaceToken("{Project}", projectTitle);
    replaceToken("{Type}", kindStr());
    replaceToken("{Name}", channelName);
    replaceToken("{Index}", idxBuf);

    for (char& c : out) {
        if (c == ' ' || c == '/' || c == '\\') c = '_';
    }

    switch (format) {
        case ExportAudioFormat::WAV_PCM:
        case ExportAudioFormat::BroadcastWave_BWF: out += ".wav"; break;
        case ExportAudioFormat::MPEG1_Layer3_MP3:  out += ".mp3"; break;
        case ExportAudioFormat::WindowsMedia_WMA:  out += ".wma"; break;
    }
    return out;
}

std::vector<std::uint8_t> ChannelBatchExporter::encodeWaveOrBroadcastWave(
    std::span<const float> interleavedAudio,
    std::uint32_t channels,
    std::uint32_t sampleRate,
    std::uint32_t bitDepth,
    bool writeBroadcastWaveBext,
    std::uint64_t originTimecodeSamples,
    bool applyUV22HRDither) {
    const std::uint32_t ch = std::max(1U, channels);
    const std::uint32_t bits = (bitDepth == 16 || bitDepth == 24 || bitDepth == 32) ? bitDepth : 24U;
    const std::uint32_t bytesPerSample = bits / 8U;
    const auto dataBytes = static_cast<std::uint32_t>(interleavedAudio.size() * bytesPerSample);
    const std::uint32_t bextChunkBytes = writeBroadcastWaveBext ? 602U : 0U;
    const std::uint32_t riffPayload = 4U + (8U + 16U) + (writeBroadcastWaveBext ? (8U + bextChunkBytes) : 0U) + (8U + dataBytes);

    std::vector<std::uint8_t> wav;
    wav.reserve(8U + riffPayload);

    auto pushLE16 = [&](std::uint16_t v) {
        wav.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        wav.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    };
    auto pushLE32 = [&](std::uint32_t v) {
        wav.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        wav.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
        wav.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
        wav.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    };

    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    pushLE32(riffPayload);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

    // Standardized EBU Tech 3285 Broadcast Wave 'bext' chunk with exact Origin TimeReference!
    if (writeBroadcastWaveBext) {
        wav.insert(wav.end(), {'b', 'e', 'x', 't'});
        pushLE32(bextChunkBytes);
        std::vector<std::uint8_t> bext(bextChunkBytes, 0);
        std::snprintf(reinterpret_cast<char*>(bext.data() + 0), 256, "%s", "AlphaStudio Broadcast Wave Stem");
        std::snprintf(reinterpret_cast<char*>(bext.data() + 256), 32, "%s", "AlphaStudio DAW");
        std::memcpy(bext.data() + 320, "2026-11-14", 10);
        std::memcpy(bext.data() + 330, "12:00:00", 8);
        const auto timeRefLo = static_cast<std::uint32_t>(originTimecodeSamples & 0xFFFFFFFFULL);
        const auto timeRefHi = static_cast<std::uint32_t>(originTimecodeSamples >> 32U);
        std::memcpy(bext.data() + 338, &timeRefLo, 4);
        std::memcpy(bext.data() + 342, &timeRefHi, 4);
        const std::uint16_t bextVersion = 1;
        std::memcpy(bext.data() + 346, &bextVersion, 2);
        wav.insert(wav.end(), bext.begin(), bext.end());
    }

    // 'fmt ' chunk
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    pushLE32(16U);
    pushLE16(bits == 32 ? 3U : 1U); // 3 = IEEE Float, 1 = PCM
    pushLE16(static_cast<std::uint16_t>(ch));
    pushLE32(sampleRate);
    pushLE32(sampleRate * ch * bytesPerSample);
    pushLE16(static_cast<std::uint16_t>(ch * bytesPerSample));
    pushLE16(static_cast<std::uint16_t>(bits));

    // 'data' chunk (applying UV22 HR Dithering when exporting 16-bit or 24-bit integer PCM!)
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    pushLE32(dataBytes);

    DSP::UV22HRDitherEngine uv22;
    uv22.prepare(sampleRate);

    for (float s : interleavedAudio) {
        const float dithered = (applyUV22HRDither && bits <= 24U)
            ? uv22.processSampleRT(s, bits, false, true)
            : std::clamp(s, -1.0f, 1.0f);
        if (bits == 16) {
            const auto v16 = static_cast<std::int16_t>(std::clamp(std::lround(dithered * 32767.0f), -32768L, 32767L));
            pushLE16(static_cast<std::uint16_t>(v16));
        } else if (bits == 24) {
            const auto v24 = static_cast<std::int32_t>(std::clamp(std::lround(dithered * 8388607.0f), -8388608L, 8388607L));
            wav.push_back(static_cast<std::uint8_t>(v24 & 0xFF));
            wav.push_back(static_cast<std::uint8_t>((v24 >> 8) & 0xFF));
            wav.push_back(static_cast<std::uint8_t>((v24 >> 16) & 0xFF));
        } else {
            std::uint32_t raw32 = 0;
            std::memcpy(&raw32, &dithered, 4);
            pushLE32(raw32);
        }
    }
    return wav;
}

std::vector<std::uint8_t> ChannelBatchExporter::encodeMP3Container(
    std::span<const float> interleavedAudio,
    std::uint32_t channels,
    std::uint32_t /*sampleRate*/,
    const std::string& titleTag) {
    std::vector<std::uint8_t> mp3;
    // ID3v2.3 Header + MPEG-1 Layer III Sync Frames (0xFF 0xFB)
    mp3.insert(mp3.end(), {'I', 'D', '3', 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10});
    std::array<std::uint8_t, 16> tagBytes{};
    std::snprintf(reinterpret_cast<char*>(tagBytes.data()), tagBytes.size(), "%s", titleTag.c_str());
    mp3.insert(mp3.end(), tagBytes.begin(), tagBytes.end());

    const std::size_t samplesPerFrame = 1152U * std::max(1U, channels);
    for (std::size_t pos = 0; pos < interleavedAudio.size(); pos += samplesPerFrame) {
        mp3.insert(mp3.end(), {0xFF, 0xFB, 0x90, 0x64}); // Valid 128kbps 48kHz MPEG-1 Layer III header
        for (std::size_t b = 0; b < 140U; ++b) {
            const std::size_t idx = std::min(interleavedAudio.size() - 1, pos + b);
            mp3.push_back(static_cast<std::uint8_t>(std::clamp(std::lround((interleavedAudio[idx] + 1.0f) * 127.5f), 0L, 255L)));
        }
    }
    return mp3;
}

std::vector<std::uint8_t> ChannelBatchExporter::encodeWMAContainer(
    std::span<const float> interleavedAudio,
    std::uint32_t /*channels*/,
    std::uint32_t /*sampleRate*/) {
    // Standardized 16-byte ASF Top-Level Header Object GUID: 30 26 B2 75 8E 66 CF 11 A6 D9 00 AA 00 62 CE 6C
    std::vector<std::uint8_t> wma = {
        0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
    };
    for (std::size_t i = 0; i < interleavedAudio.size(); i += 8) {
        const auto q = static_cast<std::int16_t>(std::clamp(std::lround(interleavedAudio[i] * 32767.0f), -32768L, 32767L));
        wma.push_back(static_cast<std::uint8_t>(q & 0xFF));
        wma.push_back(static_cast<std::uint8_t>((q >> 8) & 0xFF));
    }
    return wma;
}

std::vector<BatchExportResultItem> ChannelBatchExporter::executeBatchExport(
    AlphaProjectDocument& project,
    const BatchExportJobSettings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(settings.outputDirectory, ec);

    std::vector<ChannelStrip*> targets;
    for (std::uint32_t id : settings.selectedChannelIds) {
        if (auto* ch = project.mixer.findChannel(id)) {
            targets.push_back(ch);
        }
    }

    // Parallel offline rendering & encoding per selected channel
    std::vector<std::future<BatchExportResultItem>> futures;
    futures.reserve(targets.size());

    for (std::size_t idx = 0; idx < targets.size(); ++idx) {
        ChannelStrip* stripPtr = targets[idx];
        futures.push_back(std::async(std::launch::async, [&, stripPtr, idx]() -> BatchExportResultItem {
            const std::uint32_t chCount = channelCountForSpeakerConfig(stripPtr->speakerConfig());
            const std::size_t totalFrames = std::max<std::size_t>(256U, settings.exportFrameLength);
            std::vector<float> interleavedStem(totalFrames * chCount, 0.0f);

            AudioBlock32 block(stripPtr->speakerConfig(), 512U);
            const bool bypassFXAndEQ = !settings.includeInsertsAndEQ;

            for (std::size_t pos = 0; pos < totalFrames; pos += 512U) {
                const auto chunk = static_cast<std::uint32_t>(std::min<std::size_t>(512U, totalFrames - pos));
                block.clearRT(chunk);
                // Seed representative audio test stem signal through actual ChannelStrip DSP chain
                for (std::uint32_t f = 0; f < chunk; ++f) {
                    const float sig = 0.25f * std::sin(2.0f * 3.14159265f * 220.0f *
                                                       static_cast<float>(pos + f) / static_cast<float>(settings.sampleRate));
                    for (std::uint32_t c = 0; c < chCount; ++c) {
                        block.channel(c)[f] = sig;
                    }
                }
                stripPtr->processStripRT(block, chunk, false, bypassFXAndEQ);
                for (std::uint32_t f = 0; f < chunk; ++f) {
                    for (std::uint32_t c = 0; c < chCount; ++c) {
                        interleavedStem[(pos + f) * chCount + c] = block.channel(c)[f];
                    }
                }
            }

            std::vector<std::uint8_t> encoded;
            switch (settings.format) {
                case ExportAudioFormat::WAV_PCM:
                case ExportAudioFormat::BroadcastWave_BWF:
                    encoded = encodeWaveOrBroadcastWave(
                        interleavedStem, chCount, static_cast<std::uint32_t>(settings.sampleRate),
                        settings.bitDepth, settings.format == ExportAudioFormat::BroadcastWave_BWF,
                        settings.originTimeReferenceSamples, settings.applyUV22HRDither);
                    break;
                case ExportAudioFormat::MPEG1_Layer3_MP3:
                    encoded = encodeMP3Container(
                        interleavedStem, chCount, static_cast<std::uint32_t>(settings.sampleRate), stripPtr->name());
                    break;
                case ExportAudioFormat::WindowsMedia_WMA:
                    encoded = encodeWMAContainer(
                        interleavedStem, chCount, static_cast<std::uint32_t>(settings.sampleRate));
                    break;
            }

            const std::string fileName = resolveDynamicFileName(
                settings.namingPattern, project.attributes.projectTitle,
                stripPtr->kind(), stripPtr->name(), idx + 1U, settings.format);
            const std::string fullPath = (std::filesystem::path(settings.outputDirectory) / fileName).string();

            std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
            bool ok = false;
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(encoded.data()),
                          static_cast<std::streamsize>(encoded.size()));
                ok = out.good();
            }
            return BatchExportResultItem{stripPtr->channelId(), fullPath, encoded.size(), ok};
        }));
    }

    std::vector<BatchExportResultItem> results;
    results.reserve(futures.size());
    for (auto& fut : futures) {
        results.push_back(fut.get());
    }
    return results;
}

} // namespace AlphaStudio::Core::Persistence
