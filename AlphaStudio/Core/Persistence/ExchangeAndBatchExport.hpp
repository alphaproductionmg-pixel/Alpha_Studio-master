#pragma once

#include "AlphaStudio/Core/DSP/AudioEffectsSuite.hpp"
#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core::Persistence {

// ============================================================================
// 1. TrackArchive (XML Interchange with Media Copy/Inclusion vs Exclusion)
// ============================================================================
struct TrackArchiveOptions {
    bool includeAndCopyMediaFiles{true}; // True = bundle/copy media, False = exclude media
    std::string targetArchiveDirectory;
};

class TrackArchiveManager {
public:
    [[nodiscard]] static std::string serializeTracksToXml(
        std::span<const TrackBase* const> selectedTracks,
        const AudioPool& pool,
        const TrackArchiveOptions& options);

    static bool importTracksFromXml(const std::string& xmlText,
                                    AlphaProjectDocument& targetProject);
};

// ============================================================================
// 2. StandardMIDIFile (SMF Format 0 & 1, 24 to 960 PPQ)
// ============================================================================
class StandardMIDIFileIO {
public:
    [[nodiscard]] static std::vector<std::uint8_t> exportSMFFormat1(
        std::span<const MIDITrack* const> midiTracks,
        const MusicalClock& clock,
        std::uint16_t exportResolutionPPQ = 480); // Configurable 24..960 PPQ!

    static bool importSMFBytes(std::span<const std::uint8_t> smfBytes,
                               std::vector<MidiPartContainer>& outTrackParts,
                               std::uint16_t& outFilePPQ,
                               double& outInitialTempoBpm);
};

// ============================================================================
// 3. MusicXML (Partwise Score Interchange for BasicScoreEditor) & OMF 2.0
// ============================================================================
class MusicXMLAndOMFInterchange {
public:
    [[nodiscard]] static std::string exportPartToMusicXML(
        const MidiPartContainer& part,
        const MusicalClock& clock);
    static bool importPartFromMusicXML(
        const std::string& musicXmlText,
        MidiPartContainer& outPart,
        const MusicalClock& clock);

    [[nodiscard]] static std::vector<std::uint8_t> exportProjectToOMF2(
        const AlphaProjectDocument& doc,
        bool embedAudioMedia = true);
    static bool importOMF2HeaderSummary(
        std::span<const std::uint8_t> omfBytes,
        std::uint32_t& outTrackCount,
        std::uint32_t& outClipCount);
};

// ============================================================================
// 4. Channel Batch Export:
// - Parallel/Offline Multi-Channel Bounce of Audio/Instrument Tracks, Groups,
//   FX Returns, or Output Buses
// - Option to Include or Exclude ChannelEQ & Inserts (dry stem export!)
// - Dynamic Naming Scheme ({Project}, {Type}, {Name}, {Index})
// - Formats: WAV, Broadcast Wave (BWF with exact origin TimeReference timecode!),
//            MP3, and WMA
// - Integrated UV22 HR Dithering on 16-bit & 20/24-bit exports
// ============================================================================
enum class ExportAudioFormat : std::uint8_t {
    WAV_PCM             = 0,
    BroadcastWave_BWF   = 1, // Embeds standardized 'bext' chunk + exact origin timecode!
    MPEG1_Layer3_MP3    = 2,
    WindowsMedia_WMA    = 3
};

struct BatchExportJobSettings {
    std::string outputDirectory{"Export"};
    std::string namingPattern{"{Project}_{Type}_{Index}_{Name}"};
    ExportAudioFormat format{ExportAudioFormat::BroadcastWave_BWF};
    double sampleRate{48000.0};
    std::uint32_t bitDepth{24}; // 16, 24, or 32-bit float
    bool includeInsertsAndEQ{true}; // False = bypass EQs & Inserts for raw stem export!
    bool applyUV22HRDither{true};
    std::uint64_t originTimeReferenceSamples{0}; // Exact Broadcast Wave origin timecode!
    std::size_t exportFrameLength{48000};
    std::vector<std::uint32_t> selectedChannelIds;
};

struct BatchExportResultItem {
    std::uint32_t channelId{0};
    std::string outputFilePath;
    std::size_t bytesWritten{0};
    bool success{false};
};

class ChannelBatchExporter {
public:
    static std::vector<BatchExportResultItem> executeBatchExport(
        AlphaProjectDocument& project,
        const BatchExportJobSettings& settings);

    [[nodiscard]] static std::string resolveDynamicFileName(
        const std::string& pattern,
        const std::string& projectTitle,
        MixerChannelKind kind,
        const std::string& channelName,
        std::size_t index1Based,
        ExportAudioFormat format);

    [[nodiscard]] static std::vector<std::uint8_t> encodeWaveOrBroadcastWave(
        std::span<const float> interleavedAudio,
        std::uint32_t channels,
        std::uint32_t sampleRate,
        std::uint32_t bitDepth,
        bool writeBroadcastWaveBext,
        std::uint64_t originTimecodeSamples,
        bool applyUV22HRDither);

    [[nodiscard]] static std::vector<std::uint8_t> encodeMP3Container(
        std::span<const float> interleavedAudio,
        std::uint32_t channels,
        std::uint32_t sampleRate,
        const std::string& titleTag);

    [[nodiscard]] static std::vector<std::uint8_t> encodeWMAContainer(
        std::span<const float> interleavedAudio,
        std::uint32_t channels,
        std::uint32_t sampleRate);
};

} // namespace AlphaStudio::Core::Persistence
