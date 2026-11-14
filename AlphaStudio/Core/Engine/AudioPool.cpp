#include "AlphaStudio/Core/Engine/AudioPool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>

namespace AlphaStudio::Core {

// ============================================================================
// WaveformPeakCache
// ============================================================================
void WaveformPeakCache::buildFromInterleaved(std::span<const float> interleavedSamples,
                                             std::uint32_t channels, std::size_t numFrames) {
    channels_ = std::max(1U, channels);
    for (std::size_t lvl = 0; lvl < kFramesPerPeakLevels.size(); ++lvl) {
        const std::uint32_t block = kFramesPerPeakLevels[lvl];
        const std::size_t buckets = (numFrames + block - 1U) / block;
        mipmaps_[lvl].assign(channels_, std::vector<PeakMinMax>(buckets));

        for (std::size_t b = 0; b < buckets; ++b) {
            const std::size_t fStart = b * block;
            const std::size_t fEnd = std::min(numFrames, fStart + block);
            for (std::uint32_t c = 0; c < channels_; ++c) {
                float lo = 1.0f;
                float hi = -1.0f;
                for (std::size_t f = fStart; f < fEnd; ++f) {
                    const std::size_t idx = f * channels_ + c;
                    if (idx < interleavedSamples.size()) {
                        const float v = interleavedSamples[idx];
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                }
                if (lo > hi) {
                    lo = 0.0f;
                    hi = 0.0f;
                }
                mipmaps_[lvl][c][b] = PeakMinMax{lo, hi};
            }
        }
    }
}

std::span<const PeakMinMax> WaveformPeakCache::peaksForResolution(
    std::uint32_t channel, std::uint32_t framesPerPixel) const noexcept {
    if (channel >= channels_) {
        return {};
    }
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < kFramesPerPeakLevels.size(); ++i) {
        if (framesPerPixel >= kFramesPerPeakLevels[i]) {
            chosen = i;
        }
    }
    return mipmaps_[chosen][channel];
}

// ============================================================================
// AudioPool
// ============================================================================
AudioPool::AudioPool(double projectSampleRate)
    : projectSampleRate_(std::max(8000.0, projectSampleRate)) {}

std::vector<float> AudioPool::convertSampleRateWindowedSinc(
    std::span<const float> interleavedIn,
    std::uint32_t channels,
    double sourceRate,
    double targetRate) {
    if (channels == 0 || interleavedIn.empty() || sourceRate <= 0.0 || targetRate <= 0.0) {
        return {};
    }
    if (std::abs(sourceRate - targetRate) < 0.5) {
        return std::vector<float>(interleavedIn.begin(), interleavedIn.end());
    }

    const std::size_t inFrames = interleavedIn.size() / channels;
    const double ratio = targetRate / sourceRate;
    const std::size_t outFrames = static_cast<std::size_t>(
        std::max<long long>(1LL, std::llround(static_cast<double>(inFrames) * ratio)));
    std::vector<float> out(outFrames * channels, 0.0f);

    constexpr int kHalfTaps = 16;
    const double cutoff = std::min(1.0, ratio);

    for (std::size_t of = 0; of < outFrames; ++of) {
        const double srcPos = static_cast<double>(of) / ratio;
        const auto center = static_cast<std::int64_t>(std::floor(srcPos));

        for (std::uint32_t c = 0; c < channels; ++c) {
            double acc = 0.0;
            double weightSum = 0.0;

            for (int k = -kHalfTaps; k <= kHalfTaps; ++k) {
                const std::int64_t idx = center + k;
                const double x = (srcPos - static_cast<double>(idx)) * cutoff;
                double sincVal = 1.0;
                if (std::abs(x) > 1e-9) {
                    const double pix = std::numbers::pi * x;
                    sincVal = std::sin(pix) / pix;
                }
                // Blackman window
                const double winPhase = (srcPos - static_cast<double>(idx)) / static_cast<double>(kHalfTaps);
                double window = 0.0;
                if (std::abs(winPhase) <= 1.0) {
                    window = 0.42 + 0.5 * std::cos(std::numbers::pi * winPhase) +
                             0.08 * std::cos(2.0 * std::numbers::pi * winPhase);
                }
                const double w = sincVal * window * cutoff;
                weightSum += w;
                if (idx >= 0 && static_cast<std::size_t>(idx) < inFrames) {
                    acc += static_cast<double>(
                        interleavedIn[static_cast<std::size_t>(idx) * channels + c]) * w;
                }
            }

            out[of * channels + c] = (std::abs(weightSum) > 1e-12)
                ? static_cast<float>(acc / weightSum)
                : 0.0f;
        }
    }
    return out;
}

std::uint32_t AudioPool::importAudioBuffer(const std::string& name,
                                           const std::string& filePath,
                                           std::span<const float> interleavedIn,
                                           std::uint32_t channels,
                                           double sourceSampleRate,
                                           std::uint32_t originalBitDepth,
                                           bool convertSampleRateToProject,
                                           std::uint64_t originTimecodeSamples) {
    const std::uint32_t ch = std::max(1U, channels);
    auto asset = std::make_unique<AudioPoolAsset>();
    asset->assetId = nextAssetId_++;
    asset->name = name;
    asset->filePath = filePath;
    asset->channels = ch;
    asset->bitDepth = originalBitDepth;
    asset->originTimeReferenceSamples = originTimecodeSamples;

    if (convertSampleRateToProject && std::abs(sourceSampleRate - projectSampleRate_) >= 0.5) {
        asset->interleavedSamples = convertSampleRateWindowedSinc(
            interleavedIn, ch, sourceSampleRate, projectSampleRate_);
        asset->sampleRate = projectSampleRate_;
        if (sourceSampleRate > 0.0) {
            asset->originTimeReferenceSamples = static_cast<std::uint64_t>(
                std::llround(static_cast<double>(originTimecodeSamples) *
                             (projectSampleRate_ / sourceSampleRate)));
        }
    } else {
        asset->interleavedSamples.assign(interleavedIn.begin(), interleavedIn.end());
        asset->sampleRate = sourceSampleRate;
    }

    asset->numFrames = asset->interleavedSamples.size() / ch;
    asset->peakCache.buildFromInterleaved(asset->interleavedSamples, ch, asset->numFrames);

    const std::uint32_t id = asset->assetId;
    assets_.push_back(std::move(asset));
    return id;
}

std::uint32_t AudioPool::importWavFile(const std::string& wavFilePath,
                                       bool convertSampleRateToProject) {
    std::ifstream in(wavFilePath, std::ios::binary);
    if (!in.is_open()) {
        return 0;
    }

    char riffHeader[12]{};
    in.read(riffHeader, 12);
    if (std::memcmp(riffHeader, "RIFF", 4) != 0 || std::memcmp(riffHeader + 8, "WAVE", 4) != 0) {
        return 0;
    }

    std::uint16_t audioFormat = 1;
    std::uint16_t numCh = 2;
    std::uint32_t sampleRate = 48000;
    std::uint16_t bitsPerSample = 16;
    std::uint64_t bextTimeRef = 0;
    std::vector<std::uint8_t> rawData;

    while (in.good()) {
        char chunkId[4]{};
        std::uint32_t chunkSize = 0;
        if (!in.read(chunkId, 4) || !in.read(reinterpret_cast<char*>(&chunkSize), 4)) {
            break;
        }
        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            std::vector<std::uint8_t> fmtBuf(chunkSize);
            in.read(reinterpret_cast<char*>(fmtBuf.data()), chunkSize);
            std::memcpy(&audioFormat, fmtBuf.data() + 0, 2);
            std::memcpy(&numCh, fmtBuf.data() + 2, 2);
            std::memcpy(&sampleRate, fmtBuf.data() + 4, 4);
            std::memcpy(&bitsPerSample, fmtBuf.data() + 14, 2);
        } else if (std::memcmp(chunkId, "bext", 4) == 0 && chunkSize >= 346) {
            std::vector<std::uint8_t> bextBuf(chunkSize);
            in.read(reinterpret_cast<char*>(bextBuf.data()), chunkSize);
            std::uint32_t timeRefLow = 0;
            std::uint32_t timeRefHigh = 0;
            std::memcpy(&timeRefLow, bextBuf.data() + 338, 4);
            std::memcpy(&timeRefHigh, bextBuf.data() + 342, 4);
            bextTimeRef = (static_cast<std::uint64_t>(timeRefHigh) << 32U) | timeRefLow;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            rawData.resize(chunkSize);
            in.read(reinterpret_cast<char*>(rawData.data()), chunkSize);
        } else {
            in.seekg(chunkSize, std::ios::cur);
        }
        if ((chunkSize & 1U) != 0U) {
            in.seekg(1, std::ios::cur);
        }
    }

    if (rawData.empty() || numCh == 0) {
        return 0;
    }

    std::vector<float> floats;
    if (audioFormat == 3 && bitsPerSample == 32) {
        const std::size_t count = rawData.size() / sizeof(float);
        floats.resize(count);
        std::memcpy(floats.data(), rawData.data(), count * sizeof(float));
    } else if (bitsPerSample == 16) {
        const std::size_t count = rawData.size() / 2;
        floats.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::int16_t s = 0;
            std::memcpy(&s, rawData.data() + i * 2, 2);
            floats[i] = static_cast<float>(s) / 32768.0f;
        }
    } else if (bitsPerSample == 24) {
        const std::size_t count = rawData.size() / 3;
        floats.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::int32_t s = (static_cast<std::int32_t>(rawData[i * 3 + 0])) |
                             (static_cast<std::int32_t>(rawData[i * 3 + 1]) << 8) |
                             (static_cast<std::int32_t>(static_cast<std::int8_t>(rawData[i * 3 + 2])) << 16);
            floats[i] = static_cast<float>(s) / 8388608.0f;
        }
    }

    const std::string stem = std::filesystem::path(wavFilePath).stem().string();
    return importAudioBuffer(stem, wavFilePath, floats, numCh, sampleRate,
                             bitsPerSample, convertSampleRateToProject, bextTimeRef);
}

std::uint32_t AudioPool::importAudioCDTrack(std::uint32_t cdTrackNumber,
                                            const std::string& trackTitle,
                                            std::span<const std::int16_t> redBookPcm16Stereo,
                                            bool convertSampleRateToProject) {
    std::vector<float> stereoFloat(redBookPcm16Stereo.size());
    for (std::size_t i = 0; i < redBookPcm16Stereo.size(); ++i) {
        stereoFloat[i] = static_cast<float>(redBookPcm16Stereo[i]) / 32768.0f;
    }
    char nameBuf[128]{};
    std::snprintf(nameBuf, sizeof(nameBuf), "CD Track %02u - %s",
                  cdTrackNumber, trackTitle.empty() ? "Audio" : trackTitle.c_str());
    const std::string targetPath = poolDirectory_ + "/" + std::string(nameBuf) + ".wav";
    return importAudioBuffer(nameBuf, targetPath, stereoFloat, 2U, 44100.0,
                             16U, convertSampleRateToProject, 0ULL);
}

bool AudioPool::renameAssetSafely(std::uint32_t assetId, const std::string& newBaseName) {
    if (newBaseName.empty()) {
        return false;
    }
    for (const auto& existing : assets_) {
        if (existing->assetId != assetId && existing->name == newBaseName) {
            return false; // Prevent collision in Pool
        }
    }
    AudioPoolAsset* asset = findAsset(assetId);
    if (asset == nullptr) {
        return false;
    }

    if (!asset->filePath.empty()) {
        std::error_code ec;
        const std::filesystem::path oldPath(asset->filePath);
        const std::filesystem::path newPath =
            oldPath.parent_path() / (newBaseName + oldPath.extension().string());
        if (std::filesystem::exists(oldPath, ec)) {
            std::filesystem::rename(oldPath, newPath, ec);
            if (!ec) {
                asset->filePath = newPath.string();
            }
        } else {
            asset->filePath = newPath.string();
        }
    }

    asset->name = newBaseName;
    for (const auto& listener : renameListeners_) {
        if (listener) {
            listener(assetId, newBaseName);
        }
    }
    return true;
}

void AudioPool::registerRenameListener(RenameNotificationHook listener) {
    if (listener) {
        renameListeners_.push_back(std::move(listener));
    }
}

void AudioPool::incrementReference(std::uint32_t assetId) noexcept {
    if (auto* asset = findAsset(assetId)) {
        ++asset->usageReferenceCount;
        asset->isInTrash = false;
    }
}

void AudioPool::decrementReference(std::uint32_t assetId) noexcept {
    if (auto* asset = findAsset(assetId)) {
        if (asset->usageReferenceCount > 0) {
            --asset->usageReferenceCount;
        }
    }
}

bool AudioPool::moveUnusedToTrash(std::uint32_t assetId) noexcept {
    if (auto* asset = findAsset(assetId)) {
        if (asset->usageReferenceCount == 0) {
            asset->isInTrash = true;
            return true;
        }
    }
    return false;
}

std::size_t AudioPool::emptyTrash() {
    const std::size_t before = assets_.size();
    assets_.erase(
        std::remove_if(assets_.begin(), assets_.end(),
                       [](const std::unique_ptr<AudioPoolAsset>& a) {
                           return a->isInTrash && a->usageReferenceCount == 0;
                       }),
        assets_.end());
    return before - assets_.size();
}

AudioPoolAsset* AudioPool::findAsset(std::uint32_t assetId) noexcept {
    for (auto& a : assets_) {
        if (a->assetId == assetId) {
            return a.get();
        }
    }
    return nullptr;
}

const AudioPoolAsset* AudioPool::findAsset(std::uint32_t assetId) const noexcept {
    for (const auto& a : assets_) {
        if (a->assetId == assetId) {
            return a.get();
        }
    }
    return nullptr;
}

} // namespace AlphaStudio::Core
