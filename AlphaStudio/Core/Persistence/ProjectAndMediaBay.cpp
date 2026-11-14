#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace AlphaStudio::Core::Persistence {

// ============================================================================
// 1. AlphaProjSerializer (.alphaproj)
// ============================================================================
std::string AlphaProjSerializer::serializeToString(const AlphaProjectDocument& doc) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<AlphaStudioProject version=\"1.0\" sampleRate=\"" << doc.sampleRate
        << "\" displayPPQ=\"" << doc.midiDisplayPPQ << "\">\n";
    oss << "  <Attributes title=\"" << doc.attributes.projectTitle
        << "\" artist=\"" << doc.attributes.artistOrClient
        << "\" engineer=\"" << doc.attributes.engineerName
        << "\" copyright=\"" << doc.attributes.copyrightNotice << "\"/>\n";

    oss << "  <TempoMap>\n";
    for (const auto& tn : doc.clock.tempoNodes()) {
        oss << "    <Tempo tick=\"" << tn.tickPosition << "\" bpm=\"" << tn.bpm
            << "\" ramp=\"" << (tn.rampToNext ? 1 : 0) << "\"/>\n";
    }
    oss << "  </TempoMap>\n";

    oss << "  <VSTConnections>\n";
    for (const auto& bus : doc.connections.outputBuses()) {
        oss << "    <OutputBus id=\"" << bus.busId << "\" name=\"" << bus.name
            << "\" cfg=\"" << static_cast<int>(bus.speakerConfig) << "\"/>\n";
    }
    oss << "  </VSTConnections>\n";

    oss << "  <MixerChannels>\n";
    for (const auto& ch : doc.mixer.channels()) {
        oss << "    <Channel id=\"" << ch->channelId()
            << "\" name=\"" << ch->name()
            << "\" kind=\"" << static_cast<int>(ch->kind())
            << "\" faderDb=\"" << ch->faderDb()
            << "\" pan=\"" << ch->panner().pan()
            << "\" phaseRev=\"" << (ch->phaseReverse() ? 1 : 0) << "\"/>\n";
    }
    oss << "  </MixerChannels>\n";

    oss << "  <Tracks>\n";
    for (const auto& t : doc.tracks) {
        oss << "    <Track id=\"" << t->trackId()
            << "\" name=\"" << t->name()
            << "\" type=\"" << static_cast<int>(t->type())
            << "\" mute=\"" << (t->isMuted() ? 1 : 0)
            << "\" solo=\"" << (t->isSoloed() ? 1 : 0) << "\">\n";
        if (const auto* at = dynamic_cast<const AudioTrack*>(t.get())) {
            for (const auto& clip : at->clips()) {
                oss << "      <AudioClip id=\"" << clip.eventId
                    << "\" name=\"" << clip.name
                    << "\" asset=\"" << clip.poolAssetId
                    << "\" startSample=\"" << clip.startSample
                    << "\" lenFrames=\"" << clip.lengthFrames << "\"/>\n";
            }
        } else if (const auto* mt = dynamic_cast<const MIDITrack*>(t.get())) {
            for (const auto& part : mt->parts()) {
                oss << "      <MidiPart id=\"" << part.partId
                    << "\" name=\"" << part.name
                    << "\" startTick=\"" << part.startTick
                    << "\" lenTicks=\"" << part.lengthTicks
                    << "\" events=\"" << part.events.size() << "\"/>\n";
            }
        }
        oss << "    </Track>\n";
    }
    oss << "  </Tracks>\n";

    oss << "  <Automation>\n";
    for (const auto& lane : doc.automationLanes) {
        oss << "    <Lane paramId=\"" << lane.parameterId() << "\" name=\"" << lane.parameterName() << "\">\n";
        for (const auto& pt : lane.points()) {
            oss << "      <Pt sample=\"" << pt.samplePosition << "\" val=\"" << pt.normalizedValue << "\"/>\n";
        }
        oss << "    </Lane>\n";
    }
    oss << "  </Automation>\n";

    oss << "  <PluginStates>\n";
    for (const auto& ps : doc.pluginStates) {
        oss << "    <PluginState ch=\"" << ps.ownerChannelId << "\" slot=\"" << ps.insertSlotIndex
            << "\" name=\"" << ps.pluginName << "\" paramCount=\"" << ps.normalizedParameters.size() << "\"/>\n";
    }
    oss << "  </PluginStates>\n";
    oss << "</AlphaStudioProject>\n";
    return oss.str();
}

bool AlphaProjSerializer::saveToFile(const std::string& filePath, const AlphaProjectDocument& doc) {
    std::error_code ec;
    const auto parent = std::filesystem::path(filePath).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    const std::string xml = serializeToString(doc);
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    return out.good();
}

bool AlphaProjSerializer::deserializeFromString(const std::string& content,
                                                AlphaProjectDocument& outDoc) {
    if (content.find("<AlphaStudioProject") == std::string::npos) {
        return false;
    }
    auto getAttr = [](const std::string& line, const char* key) -> std::string {
        const std::string needle = std::string(key) + "=\"";
        const auto p0 = line.find(needle);
        if (p0 == std::string::npos) return {};
        const auto s = p0 + needle.size();
        const auto e = line.find('"', s);
        return (e != std::string::npos) ? line.substr(s, e - s) : std::string{};
    };

    outDoc.tracks.clear();
    outDoc.automationLanes.clear();

    std::istringstream iss(content);
    std::string line;
    std::uint32_t nextTrkId = 1;

    while (std::getline(iss, line)) {
        if (line.find("<AlphaStudioProject ") != std::string::npos) {
            const std::string sr = getAttr(line, "sampleRate");
            if (!sr.empty()) outDoc.sampleRate = std::strtod(sr.c_str(), nullptr);
            const std::string ppq = getAttr(line, "displayPPQ");
            if (!ppq.empty()) outDoc.midiDisplayPPQ = static_cast<std::int32_t>(std::strtol(ppq.c_str(), nullptr, 10));
            outDoc.clock.setSampleRate(outDoc.sampleRate);
            outDoc.clock.setMidiDisplayResolutionPPQ(outDoc.midiDisplayPPQ);
        } else if (line.find("<Attributes ") != std::string::npos) {
            outDoc.attributes.projectTitle = getAttr(line, "title");
            outDoc.attributes.artistOrClient = getAttr(line, "artist");
            outDoc.attributes.engineerName = getAttr(line, "engineer");
            outDoc.attributes.copyrightNotice = getAttr(line, "copyright");
        } else if (line.find("<Tempo ") != std::string::npos) {
            const auto tick = std::strtoll(getAttr(line, "tick").c_str(), nullptr, 10);
            const double bpm = std::strtod(getAttr(line, "bpm").c_str(), nullptr);
            const bool ramp = (std::strtol(getAttr(line, "ramp").c_str(), nullptr, 10) != 0);
            outDoc.clock.addOrUpdateTempoNode(tick, bpm, ramp);
        } else if (line.find("<Track ") != std::string::npos) {
            const std::string tName = getAttr(line, "name");
            const auto tType = static_cast<TrackType>(std::strtol(getAttr(line, "type").c_str(), nullptr, 10));
            const bool muted = (std::strtol(getAttr(line, "mute").c_str(), nullptr, 10) != 0);
            std::unique_ptr<TrackBase> trk;
            switch (tType) {
                case TrackType::AudioTrack:
                    trk = std::make_unique<AudioTrack>(nextTrkId++, tName, outDoc.mixer.addAudioOrInstrumentChannel(tName, false));
                    break;
                case TrackType::InstrumentTrack:
                    trk = std::make_unique<InstrumentTrack>(nextTrkId++, tName, outDoc.mixer.addAudioOrInstrumentChannel(tName, true), "Prologue");
                    break;
                case TrackType::MIDITrack:
                    trk = std::make_unique<MIDITrack>(nextTrkId++, tName);
                    break;
                case TrackType::MarkerTrack:
                    trk = std::make_unique<MarkerTrack>(nextTrkId++, tName);
                    break;
                default:
                    trk = std::make_unique<FolderTrack>(nextTrkId++, tName);
                    break;
            }
            trk->setMuted(muted);
            outDoc.tracks.push_back(std::move(trk));
        }
    }
    return true;
}

bool AlphaProjSerializer::loadFromFile(const std::string& filePath, AlphaProjectDocument& outDoc) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return deserializeFromString(ss.str(), outDoc);
}

// ============================================================================
// 2. MediaBayAndVSTSound (Async Indexer + Uppercase OS Session Safe Presets)
// ============================================================================
MediaBayAndVSTSound::MediaBayAndVSTSound() {
    // Seed factory VST Sound loops, Track Presets, and Project Templates
    registerOrUpdateEntry({0, "VSTSound://Factory/Loops/AnalogKick_124.wav",
                           MediaAssetKind::AudioLoop, "Analog Kick 124", "Drums", "Techno", "Kick", "A Min", 124.0, 5});
    registerOrUpdateEntry({0, "VSTSound://Factory/Presets/LeadVocalChain.trackpreset",
                           MediaAssetKind::TrackPreset, "Lead Vocal Studio Chain", "Vocals", "Pop/Rock", "Voice", "C Maj", 120.0, 5});
    registerOrUpdateEntry({0, "VSTSound://Factory/Templates/24TrackStudioRecording.alphaproj",
                           MediaAssetKind::ProjectTemplate, "24-Track Console Session", "Recording", "Multi-Genre", "Band", "C Maj", 120.0, 5});
}

MediaBayAndVSTSound::~MediaBayAndVSTSound() {
    waitForAsyncScanCompletion();
}

void MediaBayAndVSTSound::startAsyncScan(std::vector<std::string> directories) {
    waitForAsyncScanCompletion();
    scanning_.store(true, std::memory_order_release);
    asyncScanTask_ = std::async(std::launch::async, [this, dirs = std::move(directories)]() {
        for (const auto& dir : dirs) {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const auto ext = entry.path().extension().string();
                MediaAssetKind kind = MediaAssetKind::AudioLoop;
                if (ext == ".mid") kind = MediaAssetKind::MidiLoop;
                else if (ext == ".vstsound") kind = MediaAssetKind::VSTSoundArchive;
                else if (ext == ".trackpreset") kind = MediaAssetKind::TrackPreset;
                else if (ext == ".alphaproj") kind = MediaAssetKind::ProjectTemplate;
                else if (ext != ".wav" && ext != ".aif" && ext != ".aiff") continue;

                registerOrUpdateEntry(MediaBayTagRecord{
                    .assetId = 0,
                    .filePath = entry.path().string(),
                    .kind = kind,
                    .displayName = entry.path().stem().string()
                });
            }
        }
        scanning_.store(false, std::memory_order_release);
    });
}

void MediaBayAndVSTSound::waitForAsyncScanCompletion() {
    if (asyncScanTask_.valid()) {
        asyncScanTask_.wait();
    }
}

std::uint64_t MediaBayAndVSTSound::registerOrUpdateEntry(MediaBayTagRecord record) {
    std::scoped_lock lock(dbMutex_);
    for (auto& existing : catalog_) {
        if (existing.filePath == record.filePath) {
            record.assetId = existing.assetId;
            existing = std::move(record);
            return existing.assetId;
        }
    }
    record.assetId = nextId_++;
    catalog_.push_back(std::move(record));
    return catalog_.back().assetId;
}

bool MediaBayAndVSTSound::editAssetTags(std::uint64_t assetId, const std::string& category,
                                        const std::string& styleGenre, const std::string& musicalKey,
                                        double tempoBpm, int rating0To5) {
    std::scoped_lock lock(dbMutex_);
    for (auto& rec : catalog_) {
        if (rec.assetId == assetId) {
            rec.category = category;
            rec.styleGenre = styleGenre;
            rec.musicalKey = musicalKey;
            rec.tempoBpm = std::clamp(tempoBpm, 20.0, 360.0);
            rec.rating0To5 = std::clamp(rating0To5, 0, 5);
            return true;
        }
    }
    return false;
}

std::string MediaBayAndVSTSound::buildSafeUserPresetFilePath(
    const std::string& userHomeOrAppDataDir,
    const std::string& osSessionUsername,
    const std::string& presetName,
    bool isProjectTemplate) {
    // CRITICAL BUG-PREVENTION GUARANTEE:
    // Never apply whole-path tolower()! When the OS session username is ALL UPPERCASE
    // (e.g. "/home/ALPHASTUDIO" or "C:/Users/ADMINISTRATOR"), mutating parent path casing
    // breaks case-sensitive filesystems or preset folder lookup. We preserve the exact
    // directory path & OS session name verbatim and sanitize ONLY the leaf preset filename!
    std::string safeLeaf;
    safeLeaf.reserve(presetName.size());
    for (char c : presetName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            safeLeaf.push_back('_');
        } else {
            safeLeaf.push_back(c);
        }
    }
    if (safeLeaf.empty()) {
        safeLeaf = "Preset";
    }

    std::filesystem::path base(userHomeOrAppDataDir);
    if (!osSessionUsername.empty() && userHomeOrAppDataDir.find(osSessionUsername) == std::string::npos) {
        base /= osSessionUsername; // Preserves exact UPPERCASE OS session name!
    }
    base /= "AlphaStudio";
    base /= isProjectTemplate ? "ProjectTemplates" : "TrackPresets";

    const std::string canonicalExt = isProjectTemplate ? ".alphaproj" : ".trackpreset";
    base /= (safeLeaf + canonicalExt);
    return base.string();
}

bool MediaBayAndVSTSound::saveTrackPresetSafe(const std::string& userRootDir,
                                              const std::string& osSessionUsername,
                                              const std::string& presetName,
                                              const ChannelStrip& strip,
                                              const std::string& categoryTag) {
    const std::string targetPath =
        buildSafeUserPresetFilePath(userRootDir, osSessionUsername, presetName, false);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(targetPath).parent_path(), ec);

    std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << "<TrackPreset name=\"" << presetName << "\" user=\"" << osSessionUsername
        << "\" faderDb=\"" << strip.faderDb() << "\" pan=\"" << strip.panner().pan() << "\"/>\n";
    out.close();

    registerOrUpdateEntry(MediaBayTagRecord{
        .assetId = 0,
        .filePath = targetPath,
        .kind = MediaAssetKind::TrackPreset,
        .displayName = presetName,
        .category = categoryTag,
        .rating0To5 = 5
    });
    return true;
}

std::vector<MediaBayTagRecord> MediaBayAndVSTSound::queryByFilter(
    const std::string& textQuery, int minRating) const {
    std::scoped_lock lock(dbMutex_);
    std::vector<MediaBayTagRecord> matches;
    for (const auto& rec : catalog_) {
        if (rec.rating0To5 < minRating) continue;
        if (textQuery.empty() ||
            rec.displayName.find(textQuery) != std::string::npos ||
            rec.category.find(textQuery) != std::string::npos ||
            rec.styleGenre.find(textQuery) != std::string::npos) {
            matches.push_back(rec);
        }
    }
    return matches;
}

std::string MediaBayAndVSTSound::exportCatalogAsSQLInserts() const {
    std::scoped_lock lock(dbMutex_);
    std::ostringstream oss;
    for (const auto& r : catalog_) {
        oss << "INSERT INTO media_assets (guid, file_path, display_name, category, style_genre, tempo_bpm, rating) VALUES ('"
            << "GUID-" << r.assetId << "', '" << r.filePath << "', '" << r.displayName
            << "', '" << r.category << "', '" << r.styleGenre << "', "
            << r.tempoBpm << ", " << r.rating0To5 << ");\n";
    }
    return oss.str();
}

} // namespace AlphaStudio::Core::Persistence
