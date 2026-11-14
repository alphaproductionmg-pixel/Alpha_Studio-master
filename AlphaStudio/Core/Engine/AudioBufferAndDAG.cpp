#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"

#include <algorithm>
#include <queue>

namespace AlphaStudio::Core {

// ============================================================================
// AudioBlock32
// ============================================================================
AudioBlock32::AudioBlock32(SpeakerConfig config, std::uint32_t maxFrames) {
    prepare(config, maxFrames);
}

void AudioBlock32::prepare(SpeakerConfig config, std::uint32_t maxFrames) {
    config_ = config;
    numChannels_ = std::clamp<std::uint32_t>(
        channelCountForSpeakerConfig(config), 1U, kMaxChannels);
    maxFrames_ = std::max(16U, maxFrames);
    channelPtrs_.fill(nullptr);
    for (std::uint32_t c = 0; c < numChannels_; ++c) {
        storage_[c].assign(maxFrames_, 0.0f);
        channelPtrs_[c] = storage_[c].data();
    }
}

void AudioBlock32::clearRT(std::uint32_t numFrames) noexcept {
    const std::uint32_t n = std::min(numFrames, maxFrames_);
    for (std::uint32_t c = 0; c < numChannels_; ++c) {
        if (channelPtrs_[c] != nullptr) {
            std::memset(channelPtrs_[c], 0, static_cast<std::size_t>(n) * sizeof(float));
        }
    }
}

void AudioBlock32::copyFromRT(const AudioBlock32& src, std::uint32_t numFrames) noexcept {
    const std::uint32_t n = std::min({numFrames, maxFrames_, src.maxFrames_});
    for (std::uint32_t c = 0; c < numChannels_; ++c) {
        if (c < src.numChannels_ && src.channelPtrs_[c] != nullptr) {
            std::memcpy(channelPtrs_[c], src.channelPtrs_[c], static_cast<std::size_t>(n) * sizeof(float));
        } else if (src.numChannels_ == 1U && src.channelPtrs_[0] != nullptr && c < 2U) {
            // Mono source upmixes cleanly to L/R
            std::memcpy(channelPtrs_[c], src.channelPtrs_[0], static_cast<std::size_t>(n) * sizeof(float));
        } else {
            std::memset(channelPtrs_[c], 0, static_cast<std::size_t>(n) * sizeof(float));
        }
    }
}

void AudioBlock32::accumulateFromRT(const AudioBlock32& src, std::uint32_t numFrames, float gain) noexcept {
    const std::uint32_t n = std::min({numFrames, maxFrames_, src.maxFrames_});
    if (std::abs(gain) < 1e-12f || n == 0) {
        return;
    }
    for (std::uint32_t c = 0; c < numChannels_; ++c) {
        const float* inPtr = nullptr;
        if (c < src.numChannels_) {
            inPtr = src.channelPtrs_[c];
        } else if (src.numChannels_ == 1U && c < 2U) {
            inPtr = src.channelPtrs_[0];
        }
        if (inPtr != nullptr) {
            float* outPtr = channelPtrs_[c];
            for (std::uint32_t f = 0; f < n; ++f) {
                outPtr[f] += inPtr[f] * gain;
            }
        }
    }
}

void AudioBlock32::applyGainRT(std::uint32_t numFrames, float gain) noexcept {
    const std::uint32_t n = std::min(numFrames, maxFrames_);
    for (std::uint32_t c = 0; c < numChannels_; ++c) {
        float* ptr = channelPtrs_[c];
        for (std::uint32_t f = 0; f < n; ++f) {
            ptr[f] *= gain;
        }
    }
}

float AudioBlock32::computePeakRT(std::uint32_t ch, std::uint32_t numFrames) const noexcept {
    if (ch >= numChannels_ || channelPtrs_[ch] == nullptr) {
        return 0.0f;
    }
    const std::uint32_t n = std::min(numFrames, maxFrames_);
    float peak = 0.0f;
    const float* ptr = channelPtrs_[ch];
    for (std::uint32_t f = 0; f < n; ++f) {
        peak = std::max(peak, std::abs(ptr[f]));
    }
    return peak;
}

float AudioBlock32::computeRmsRT(std::uint32_t ch, std::uint32_t numFrames) const noexcept {
    if (ch >= numChannels_ || channelPtrs_[ch] == nullptr || numFrames == 0) {
        return 0.0f;
    }
    const std::uint32_t n = std::min(numFrames, maxFrames_);
    double sumSq = 0.0;
    const float* ptr = channelPtrs_[ch];
    for (std::uint32_t f = 0; f < n; ++f) {
        sumSq += static_cast<double>(ptr[f]) * static_cast<double>(ptr[f]);
    }
    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(n)));
}

// ============================================================================
// SignalDAG
// ============================================================================
std::uint32_t SignalDAG::addNode(const std::string& name, DAGNodeKind kind,
                                 SpeakerConfig config, std::uint32_t maxBlockFrames,
                                 std::uint32_t selfLatencySamples,
                                 void* userObject,
                                 DAGNode::NodeProcessCallback cb) {
    auto node = std::make_unique<DAGNode>();
    node->nodeId = nextNodeId_++;
    node->name = name;
    node->kind = kind;
    node->speakerConfig = config;
    node->selfLatencySamples = selfLatencySamples;
    node->userObject = userObject;
    node->processCallback = cb;
    node->outputBuffer.prepare(config, maxBlockFrames);
    node->sidechainBuffer.prepare(SpeakerConfig::Stereo, maxBlockFrames);
    node->pdcDelayLine.prepare(channelCountForSpeakerConfig(config));

    const std::uint32_t id = node->nodeId;
    nodes_.push_back(std::move(node));
    incomingEdgesByNodeIdx_.resize(nodes_.size());
    compileExecutionPlan();
    return id;
}

int SignalDAG::findNodeIndex(std::uint32_t nodeId) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i]->nodeId == nodeId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

DAGNode* SignalDAG::findNode(std::uint32_t nodeId) noexcept {
    const int idx = findNodeIndex(nodeId);
    return (idx >= 0) ? nodes_[static_cast<std::size_t>(idx)].get() : nullptr;
}

const DAGNode* SignalDAG::findNode(std::uint32_t nodeId) const noexcept {
    const int idx = findNodeIndex(nodeId);
    return (idx >= 0) ? nodes_[static_cast<std::size_t>(idx)].get() : nullptr;
}

bool SignalDAG::wouldCreateCycle(std::uint32_t srcId, std::uint32_t dstId) const {
    if (srcId == dstId) {
        return true;
    }
    // BFS from dstId to check whether srcId is reachable
    std::queue<std::uint32_t> q;
    std::vector<bool> visited(nodes_.size(), false);
    const int dstIdx = findNodeIndex(dstId);
    if (dstIdx < 0) {
        return true;
    }
    q.push(dstId);
    visited[static_cast<std::size_t>(dstIdx)] = true;

    while (!q.empty()) {
        const std::uint32_t cur = q.front();
        q.pop();
        if (cur == srcId) {
            return true;
        }
        for (const auto& e : edges_) {
            if (e.sourceNodeId == cur) {
                const int nextIdx = findNodeIndex(e.destNodeId);
                if (nextIdx >= 0 && !visited[static_cast<std::size_t>(nextIdx)]) {
                    visited[static_cast<std::size_t>(nextIdx)] = true;
                    q.push(e.destNodeId);
                }
            }
        }
    }
    return false;
}

bool SignalDAG::connectNodes(std::uint32_t sourceNodeId, std::uint32_t destNodeId,
                             DAGEdgeRole role, float sendGainLinear,
                             std::uint32_t auxSlotIndex) {
    if (findNodeIndex(sourceNodeId) < 0 || findNodeIndex(destNodeId) < 0) {
        return false;
    }
    if (wouldCreateCycle(sourceNodeId, destNodeId)) {
        return false; // Acyclic guarantee
    }
    edges_.push_back(DAGEdge{
        .sourceNodeId = sourceNodeId,
        .destNodeId = destNodeId,
        .role = role,
        .sendGainLinear = sendGainLinear,
        .auxSlotIndex = auxSlotIndex
    });
    return compileExecutionPlan();
}

void SignalDAG::disconnectNodes(std::uint32_t sourceNodeId, std::uint32_t destNodeId,
                                DAGEdgeRole role) {
    edges_.erase(
        std::remove_if(edges_.begin(), edges_.end(), [&](const DAGEdge& e) {
            return e.sourceNodeId == sourceNodeId && e.destNodeId == destNodeId && e.role == role;
        }),
        edges_.end());
    compileExecutionPlan();
}

bool SignalDAG::compileExecutionPlan() {
    const std::size_t n = nodes_.size();
    incomingEdgesByNodeIdx_.assign(n, {});
    std::vector<int> inDegree(n, 0);
    std::vector<std::vector<std::size_t>> adj(n);

    for (const auto& e : edges_) {
        const int u = findNodeIndex(e.sourceNodeId);
        const int v = findNodeIndex(e.destNodeId);
        if (u >= 0 && v >= 0) {
            adj[static_cast<std::size_t>(u)].push_back(static_cast<std::size_t>(v));
            incomingEdgesByNodeIdx_[static_cast<std::size_t>(v)].push_back(e);
            ++inDegree[static_cast<std::size_t>(v)];
        }
    }

    // Kahn's topological layering + Automatic Plugin Delay Compensation (PDC)
    DAGExecutionPlan& nextPlan = planBuffers_[writePlanIdx_];
    nextPlan.topologicalLayers.clear();
    nextPlan.totalSystemPDCSamples = 0;

    std::vector<std::size_t> currentLayer;
    for (std::size_t i = 0; i < n; ++i) {
        if (inDegree[i] == 0) {
            currentLayer.push_back(i);
        }
    }

    std::vector<std::uint32_t> cumulativeArrivalLatency(n, 0);
    std::size_t visitedCount = 0;

    while (!currentLayer.empty()) {
        DAGExecutionLayer execLayer;
        execLayer.nodeIndices.reserve(currentLayer.size());
        std::vector<std::size_t> nextLayer;

        for (std::size_t u : currentLayer) {
            ++visitedCount;
            execLayer.nodeIndices.push_back(static_cast<std::uint32_t>(u));
            const std::uint32_t departLatency =
                cumulativeArrivalLatency[u] +
                (nodes_[u]->bypassed ? 0U : nodes_[u]->selfLatencySamples);
            nextPlan.totalSystemPDCSamples = std::max(nextPlan.totalSystemPDCSamples, departLatency);

            for (std::size_t v : adj[u]) {
                cumulativeArrivalLatency[v] = std::max(cumulativeArrivalLatency[v], departLatency);
                if (--inDegree[v] == 0) {
                    nextLayer.push_back(v);
                }
            }
        }

        nextPlan.topologicalLayers.push_back(std::move(execLayer));
        currentLayer = std::move(nextLayer);
    }

    if (visitedCount != n) {
        return false;
    }

    // Configure per-node PDC delay lines so leaf output/sink nodes align to totalSystemPDCSamples
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t nodeTotalLat =
            cumulativeArrivalLatency[i] +
            (nodes_[i]->bypassed ? 0U : nodes_[i]->selfLatencySamples);
        if (adj[i].empty() && nextPlan.totalSystemPDCSamples > nodeTotalLat) {
            nodes_[i]->pdcDelayLine.setDelaySamples(nextPlan.totalSystemPDCSamples - nodeTotalLat);
        } else {
            nodes_[i]->pdcDelayLine.setDelaySamples(0);
        }
    }

    activePlan_.store(&nextPlan, std::memory_order_release);
    writePlanIdx_ = 1U - writePlanIdx_;
    return true;
}

void SignalDAG::executeNodeTaskRT(void* context, std::uint32_t nodeIndex,
                                  std::uint32_t numFrames) noexcept {
    auto* self = static_cast<SignalDAG*>(context);
    if (nodeIndex >= self->nodes_.size()) {
        return;
    }
    DAGNode& node = *self->nodes_[nodeIndex];
    node.outputBuffer.clearRT(numFrames);
    node.sidechainBuffer.clearRT(numFrames);

    bool hasSidechain = false;
    for (const auto& edge : self->incomingEdgesByNodeIdx_[nodeIndex]) {
        const int srcIdx = self->findNodeIndex(edge.sourceNodeId);
        if (srcIdx < 0) {
            continue;
        }
        const AudioBlock32& srcBuf = self->nodes_[static_cast<std::size_t>(srcIdx)]->outputBuffer;
        if (edge.role == DAGEdgeRole::SidechainInput) {
            node.sidechainBuffer.accumulateFromRT(srcBuf, numFrames, edge.sendGainLinear);
            hasSidechain = true;
        } else {
            node.outputBuffer.accumulateFromRT(srcBuf, numFrames, edge.sendGainLinear);
        }
    }

    if (!node.bypassed && node.processCallback != nullptr) {
        node.processCallback(node.userObject, node.outputBuffer,
                             hasSidechain ? &node.sidechainBuffer : nullptr,
                             numFrames);
    }

    if (node.pdcDelayLine.delaySamples() > 0) {
        node.pdcDelayLine.processInPlaceRT(
            node.outputBuffer.channelPointers(),
            node.outputBuffer.numChannels(),
            numFrames);
    }
}

void SignalDAG::processGraphRT(std::uint32_t numFrames, MultiProcessingPool* pool) noexcept {
    const DAGExecutionPlan* plan = activePlan_.load(std::memory_order_acquire);
    if (plan == nullptr || numFrames == 0) {
        return;
    }

    std::array<ParallelAudioTask, MultiProcessingPool::kMaxTasksPerLayer> taskSlab{};
    for (const auto& layer : plan->topologicalLayers) {
        std::size_t offset = 0;
        while (offset < layer.nodeIndices.size()) {
            const std::size_t count = std::min<std::size_t>(
                layer.nodeIndices.size() - offset, taskSlab.size());
            for (std::size_t i = 0; i < count; ++i) {
                taskSlab[i] = ParallelAudioTask{
                    .fn = &SignalDAG::executeNodeTaskRT,
                    .context = this,
                    .nodeIndex = layer.nodeIndices[offset + i],
                    .numFrames = numFrames
                };
            }
            if (pool != nullptr && count > 1) {
                pool->executeLayerParallel(std::span<const ParallelAudioTask>(taskSlab.data(), count));
            } else {
                for (std::size_t i = 0; i < count; ++i) {
                    executeNodeTaskRT(this, taskSlab[i].nodeIndex, numFrames);
                }
            }
            offset += count;
        }
    }
}

std::uint32_t SignalDAG::totalSystemPDCSamples() const noexcept {
    const DAGExecutionPlan* plan = activePlan_.load(std::memory_order_acquire);
    return (plan != nullptr) ? plan->totalSystemPDCSamples : 0U;
}

} // namespace AlphaStudio::Core
