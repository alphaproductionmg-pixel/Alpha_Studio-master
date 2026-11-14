#pragma once

#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Engine/AudioPool.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Mixer/ControlRoom.hpp"
#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"
#include "AlphaStudio/Core/Workflow/AutomationAndQuickControls.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::Persistence {

// ============================================================================
// 1. .alphaproj Complete Studio Project Document & Serializer
// ============================================================================
struct EmbeddedProjectAttributes {
    std::string projectTitle{"Untitled AlphaStudio Session"};
    std::string artistOrClient{"AlphaStudio Production"};
    std::string engineerName{"Lead Audio Engineer"};
    std::string copyrightNotice{"(C) 2026 AlphaStudio"};
    std::string sessionNotes;
};

struct PluginBinaryStateRecord {
    std::uint32_t ownerChannelId{0};
    std::uint32_t insertSlotIndex{0};
    std::string pluginName;
    std::vector<float> normalizedParameters;
};

struct AlphaProjectDocument {
    EmbeddedProjectAttributes attributes{};
    double sampleRate{48000.0};
    std::int32_t midiDisplayPPQ{480};
    MusicalClock clock{};
    VSTConnections connections{};
    AudioPool pool{48000.0};
    VSTMixer mixer{48000.0, 4096};
    std::vector<std::unique_ptr<TrackBase>> tracks;
    std::vector<AutomationLane> automationLanes;
    std::vector<PluginBinaryStateRecord> pluginStates;
};

class AlphaProjSerializer {
public:
    [[nodiscard]] static std::string serializeToString(const AlphaProjectDocument& doc);
    static bool saveToFile(const std::string& filePath, const AlphaProjectDocument& doc);
    static bool deserializeFromString(const std::string& content, AlphaProjectDocument& outDoc);
    static bool loadFromFile(const std::string& filePath, AlphaProjectDocument& outDoc);
};

// ============================================================================
// 2. MediaBay, VST Sound Archive & TagEditor
// - Asynchronous background indexer
// - Local SQL-compatible database for Loops, VSTSound, TrackPresets & ProjectTemplates
// - Explicit protection of TrackPreset / VSTPreset creation when the OS login/session
//   username is 100% UPPERCASE (never lowercasing directory components above filename!)
// ============================================================================
enum class MediaAssetKind : std::uint8_t {
    AudioLoop       = 0,
    MidiLoop        = 1,
    VSTSoundArchive = 2,
    TrackPreset     = 3,
    ProjectTemplate = 4
};

struct MediaBayTagRecord {
    std::uint64_t assetId{0};
    std::string filePath;
    MediaAssetKind kind{MediaAssetKind::AudioLoop};
    std::string displayName;
    std::string category{"Drums & Percussion"};
    std::string styleGenre{"Electronic"};
    std::string instrumentTag{"Synthesizer"};
    std::string musicalKey{"C Maj"};
    double tempoBpm{120.0};
    int rating0To5{4};
};

class MediaBayAndVSTSound {
public:
    MediaBayAndVSTSound();
    ~MediaBayAndVSTSound();

    // Asynchronous directory & .vstsound scanner
    void startAsyncScan(std::vector<std::string> directories);
    void waitForAsyncScanCompletion();
    [[nodiscard]] bool isScanning() const noexcept { return scanning_.load(std::memory_order_acquire); }

    std::uint64_t registerOrUpdateEntry(MediaBayTagRecord record);

    // TagEditor modification API (works on Loops, TrackPresets, and ProjectTemplates!)
    bool editAssetTags(std::uint64_t assetId, const std::string& category,
                       const std::string& styleGenre, const std::string& musicalKey,
                       double tempoBpm, int rating0To5);

    // Safe TrackPreset & ProjectTemplate Path Builder & Writer:
    // Explicitly preserves exact OS user directory casing even when OS session user
    // name is ALL UPPERCASE (e.g. "ALPHASTUDIO" or "STUDIO_ADMIN")!
    [[nodiscard]] static std::string buildSafeUserPresetFilePath(
        const std::string& userHomeOrAppDataDir,
        const std::string& osSessionUsername,
        const std::string& presetName,
        bool isProjectTemplate = false);

    bool saveTrackPresetSafe(const std::string& userRootDir,
                             const std::string& osSessionUsername,
                             const std::string& presetName,
                             const ChannelStrip& strip,
                             const std::string& categoryTag = "Channel Strip");

    [[nodiscard]] std::vector<MediaBayTagRecord> queryByFilter(
        const std::string& textQuery = "",
        int minRating = 0) const;

    [[nodiscard]] std::string exportCatalogAsSQLInserts() const;

private:
    mutable std::mutex dbMutex_;
    std::atomic<bool> scanning_{false};
    std::uint64_t nextId_{1};
    std::vector<MediaBayTagRecord> catalog_;
    std::future<void> asyncScanTask_;
};

} // namespace AlphaStudio::Core::Persistence
