#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// WaveformPeakCache: Multi-resolution min/max waveform peak mipmaps (.apk)
// ============================================================================
struct PeakMinMax {
    float minVal{0.0f};
    float maxVal{0.0f};
};

class WaveformPeakCache {
public:
    static constexpr std::array<std::uint32_t, 4> kFramesPerPeakLevels = {64U, 256U, 1024U, 4096U};

    void buildFromInterleaved(std::span<const float> interleavedSamples,
                              std::uint32_t channels, std::size_t numFrames);

    [[nodiscard]] std::span<const PeakMinMax> peaksForResolution(
        std::uint32_t channel, std::uint32_t framesPerPixel) const noexcept;

private:
    std::uint32_t channels_{0};
    // mipmaps_[level][channel][bucket]
    std::array<std::vector<std::vector<PeakMinMax>>, kFramesPerPeakLevels.size()> mipmaps_{};
};

// ============================================================================
// AudioPoolAsset & AudioPool Manager
// - Reference counting, optional windowed-sinc Sample Rate Conversion on import
// - Safe file & clip renaming across referencing timeline clips
// - Multi-resolution waveform peak cache
// - Red Book 44.1 kHz / 16-bit Audio CD Track import
// ============================================================================
struct AudioPoolAsset {
    std::uint32_t assetId{0};
    std::string name;
    std::string filePath;
    double sampleRate{48000.0};
    std::uint32_t bitDepth{32};
    std::uint32_t channels{2};
    std::size_t numFrames{0};
    double musicalTempoBpm{120.0};
    bool musicalMode{false};
    std::uint64_t originTimeReferenceSamples{0}; // Broadcast Wave origin timecode
    std::uint32_t usageReferenceCount{0};
    bool isInTrash{false};

    std::vector<float> interleavedSamples;
    WaveformPeakCache peakCache;
};

class AudioPool {
public:
    using RenameNotificationHook = std::function<void(std::uint32_t assetId, const std::string& newName)>;

    explicit AudioPool(double projectSampleRate = 48000.0);

    void setProjectSampleRate(double sampleRate) noexcept { projectSampleRate_ = sampleRate; }
    [[nodiscard]] double projectSampleRate() const noexcept { return projectSampleRate_; }
    void setPoolDirectory(std::string dir) { poolDirectory_ = std::move(dir); }
    [[nodiscard]] const std::string& poolDirectory() const noexcept { return poolDirectory_; }

    // High-precision Windowed-Sinc Sample Rate Converter
    static std::vector<float> convertSampleRateWindowedSinc(
        std::span<const float> interleavedIn,
        std::uint32_t channels,
        double sourceRate,
        double targetRate);

    // Import from memory or WAV file with optional automatic SRC to project sample rate
    std::uint32_t importAudioBuffer(const std::string& name,
                                    const std::string& filePath,
                                    std::span<const float> interleavedIn,
                                    std::uint32_t channels,
                                    double sourceSampleRate,
                                    std::uint32_t originalBitDepth = 24,
                                    bool convertSampleRateToProject = true,
                                    std::uint64_t originTimecodeSamples = 0);

    std::uint32_t importWavFile(const std::string& wavFilePath,
                                bool convertSampleRateToProject = true);

    // Red Book CD-DA (44,100 Hz, 16-bit Stereo, 2352-byte sectors) track import
    std::uint32_t importAudioCDTrack(std::uint32_t cdTrackNumber,
                                     const std::string& trackTitle,
                                     std::span<const std::int16_t> redBookPcm16Stereo,
                                     bool convertSampleRateToProject = true);

    // Safe renaming updating asset name, physical file path (if present), and timeline clips
    bool renameAssetSafely(std::uint32_t assetId, const std::string& newBaseName);
    void registerRenameListener(RenameNotificationHook listener);

    void incrementReference(std::uint32_t assetId) noexcept;
    void decrementReference(std::uint32_t assetId) noexcept;
    bool moveUnusedToTrash(std::uint32_t assetId) noexcept;
    std::size_t emptyTrash();

    [[nodiscard]] AudioPoolAsset* findAsset(std::uint32_t assetId) noexcept;
    [[nodiscard]] const AudioPoolAsset* findAsset(std::uint32_t assetId) const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<AudioPoolAsset>>& assets() const noexcept { return assets_; }

private:
    double projectSampleRate_{48000.0};
    std::string poolDirectory_{"Audio"};
    std::uint32_t nextAssetId_{1};
    std::vector<std::unique_ptr<AudioPoolAsset>> assets_;
    std::vector<RenameNotificationHook> renameListeners_;
};

} // namespace AlphaStudio::Core
