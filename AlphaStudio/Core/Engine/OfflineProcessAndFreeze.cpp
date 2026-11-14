#include "AlphaStudio/Core/Engine/OfflineProcessAndFreeze.hpp"

#include <algorithm>
#include <cmath>

namespace AlphaStudio::Core {

// ============================================================================
// OfflineProcessHistory
// ============================================================================
OfflineProcessHistory::OfflineProcessHistory(std::span<const float> originalInterleaved,
                                             std::uint32_t channels, double sampleRate)
    : pristineOriginal_(originalInterleaved.begin(), originalInterleaved.end()),
      currentRendered_(originalInterleaved.begin(), originalInterleaved.end()),
      channels_(std::max(1U, channels)),
      sampleRate_(std::max(8000.0, sampleRate)) {}

std::uint32_t OfflineProcessHistory::appendStep(OfflineProcessStep step) {
    step.stepId = nextStepId_++;
    steps_.push_back(std::move(step));
    if (steps_.back().enabled) {
        applySingleStep(currentRendered_, channels_, sampleRate_, steps_.back());
    }
    return steps_.back().stepId;
}

bool OfflineProcessHistory::removeStepAt(std::size_t stackIndex) {
    if (stackIndex >= steps_.size()) {
        return false;
    }
    steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(stackIndex));
    recomputeFromPristine();
    return true;
}

bool OfflineProcessHistory::modifyStepParameter(std::size_t stackIndex, float newParameterDbOrRatio) {
    if (stackIndex >= steps_.size()) {
        return false;
    }
    steps_[stackIndex].parameterDbOrRatio = newParameterDbOrRatio;
    recomputeFromPristine();
    return true;
}

bool OfflineProcessHistory::toggleStepBypass(std::size_t stackIndex, bool enabled) {
    if (stackIndex >= steps_.size()) {
        return false;
    }
    steps_[stackIndex].enabled = enabled;
    recomputeFromPristine();
    return true;
}

void OfflineProcessHistory::revertAll() {
    steps_.clear();
    currentRendered_ = pristineOriginal_;
}

void OfflineProcessHistory::recomputeFromPristine() {
    currentRendered_ = pristineOriginal_;
    for (const auto& s : steps_) {
        if (s.enabled) {
            applySingleStep(currentRendered_, channels_, sampleRate_, s);
        }
    }
}

void OfflineProcessHistory::applySingleStep(std::vector<float>& buf, std::uint32_t channels,
                                            double sampleRate, const OfflineProcessStep& step) {
    if (buf.empty() || channels == 0) {
        return;
    }
    const std::size_t totalFrames = buf.size() / channels;
    const std::size_t fStart = std::min(step.startFrame, totalFrames);
    const std::size_t fLen = (step.frameCount == 0)
        ? (totalFrames - fStart)
        : std::min(step.frameCount, totalFrames - fStart);
    const std::size_t fEnd = fStart + fLen;
    if (fLen == 0) {
        return;
    }

    switch (step.type) {
        case OfflineOpType::Gain: {
            const float lin = std::pow(10.0f, step.parameterDbOrRatio / 20.0f);
            for (std::size_t i = fStart * channels; i < fEnd * channels; ++i) {
                buf[i] *= lin;
            }
            break;
        }
        case OfflineOpType::Normalize: {
            float maxAbs = 0.0f;
            for (std::size_t i = fStart * channels; i < fEnd * channels; ++i) {
                maxAbs = std::max(maxAbs, std::abs(buf[i]));
            }
            if (maxAbs > 1e-9f) {
                const float targetLin = std::pow(10.0f, step.parameterDbOrRatio / 20.0f);
                const float scale = targetLin / maxAbs;
                for (std::size_t i = fStart * channels; i < fEnd * channels; ++i) {
                    buf[i] *= scale;
                }
            }
            break;
        }
        case OfflineOpType::PhaseInvert: {
            for (std::size_t i = fStart * channels; i < fEnd * channels; ++i) {
                buf[i] = -buf[i];
            }
            break;
        }
        case OfflineOpType::Reverse: {
            for (std::size_t k = 0; k < fLen / 2; ++k) {
                const std::size_t leftF = fStart + k;
                const std::size_t rightF = fEnd - 1 - k;
                for (std::uint32_t c = 0; c < channels; ++c) {
                    std::swap(buf[leftF * channels + c], buf[rightF * channels + c]);
                }
            }
            break;
        }
        case OfflineOpType::FadeIn: {
            for (std::size_t k = 0; k < fLen; ++k) {
                const float env = static_cast<float>(k) / static_cast<float>(std::max<std::size_t>(1, fLen - 1));
                for (std::uint32_t c = 0; c < channels; ++c) {
                    buf[(fStart + k) * channels + c] *= env;
                }
            }
            break;
        }
        case OfflineOpType::FadeOut: {
            for (std::size_t k = 0; k < fLen; ++k) {
                const float env = 1.0f - (static_cast<float>(k) / static_cast<float>(std::max<std::size_t>(1, fLen - 1)));
                for (std::uint32_t c = 0; c < channels; ++c) {
                    buf[(fStart + k) * channels + c] *= env;
                }
            }
            break;
        }
        case OfflineOpType::RemoveDCOffset: {
            for (std::uint32_t c = 0; c < channels; ++c) {
                double sum = 0.0;
                for (std::size_t f = fStart; f < fEnd; ++f) {
                    sum += buf[f * channels + c];
                }
                const float mean = static_cast<float>(sum / static_cast<double>(fLen));
                for (std::size_t f = fStart; f < fEnd; ++f) {
                    buf[f * channels + c] -= mean;
                }
            }
            break;
        }
        case OfflineOpType::SilenceRegion: {
            std::fill(buf.begin() + static_cast<std::ptrdiff_t>(fStart * channels),
                      buf.begin() + static_cast<std::ptrdiff_t>(fEnd * channels), 0.0f);
            break;
        }
        case OfflineOpType::OfflinePluginDSP: {
            if (step.pluginProcessor) {
                step.pluginProcessor(buf, channels, sampleRate);
            }
            break;
        }
    }
}

// ============================================================================
// ExtendedFreezeManager
// ============================================================================
ExtendedFreezeManager::ExtendedFreezeManager(AudioPool& pool)
    : pool_(pool) {}

bool ExtendedFreezeManager::freezeTrack(std::uint32_t trackId,
                                        const std::string& trackName,
                                        bool isInstrumentTrack,
                                        std::size_t contentFrames,
                                        double tailSeconds,
                                        bool unloadPlugins,
                                        const SavedBusRoutingSnapshot& currentRouting,
                                        const OfflineTrackRenderFn& renderFn) {
    if (!renderFn) {
        return false;
    }
    const std::uint32_t ch = channelCountForSpeakerConfig(currentRouting.speakerConfig);
    const auto tailFrames = static_cast<std::size_t>(
        std::max(0.0, tailSeconds) * pool_.projectSampleRate());
    const std::size_t totalFreezeFrames = std::max<std::size_t>(1, contentFrames + tailFrames);

    std::vector<float> rendered32 = renderFn(totalFreezeFrames, ch);
    if (rendered32.empty()) {
        return false;
    }

    const std::string freezeName = "Freeze_" + trackName + "_" + std::to_string(trackId);
    const std::string freezePath = "Freeze/" + freezeName + ".wav";
    const std::uint32_t poolId = pool_.importAudioBuffer(
        freezeName, freezePath, rendered32, ch, pool_.projectSampleRate(), 32U, false);
    pool_.incrementReference(poolId);

    for (auto& rec : records_) {
        if (rec.trackId == trackId) {
            rec.trackName = trackName;
            rec.isInstrumentTrack = isInstrumentTrack;
            rec.unloadPluginsOnFreeze = unloadPlugins;
            rec.tailDurationSeconds = tailSeconds;
            rec.freezePoolAssetId = poolId;
            rec.freezeFilePath = freezePath;
            rec.savedRouting = currentRouting;
            rec.currentlyFrozen = true;
            return true;
        }
    }

    records_.push_back(FrozenTrackRecord{
        .trackId = trackId,
        .trackName = trackName,
        .isInstrumentTrack = isInstrumentTrack,
        .unloadPluginsOnFreeze = unloadPlugins,
        .tailDurationSeconds = tailSeconds,
        .freezePoolAssetId = poolId,
        .freezeFilePath = freezePath,
        .savedRouting = currentRouting,
        .currentlyFrozen = true
    });
    return true;
}

bool ExtendedFreezeManager::unfreezeTrack(std::uint32_t trackId,
                                          SavedBusRoutingSnapshot& restoredRoutingOut,
                                          bool deleteFreezeFile) {
    for (auto& rec : records_) {
        if (rec.trackId == trackId && rec.currentlyFrozen) {
            restoredRoutingOut = rec.savedRouting;
            rec.currentlyFrozen = false;
            pool_.decrementReference(rec.freezePoolAssetId);
            if (deleteFreezeFile) {
                pool_.moveUnusedToTrash(rec.freezePoolAssetId);
                pool_.emptyTrash();
                rec.freezePoolAssetId = 0;
            }
            return true;
        }
    }
    return false;
}

bool ExtendedFreezeManager::isTrackFrozen(std::uint32_t trackId) const noexcept {
    for (const auto& rec : records_) {
        if (rec.trackId == trackId) {
            return rec.currentlyFrozen;
        }
    }
    return false;
}

const FrozenTrackRecord* ExtendedFreezeManager::getFrozenRecord(std::uint32_t trackId) const noexcept {
    for (const auto& rec : records_) {
        if (rec.trackId == trackId) {
            return &rec;
        }
    }
    return nullptr;
}

} // namespace AlphaStudio::Core
