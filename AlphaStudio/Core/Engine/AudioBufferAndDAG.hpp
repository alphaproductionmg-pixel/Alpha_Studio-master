#pragma once

#include "AlphaStudio/Core/AudioSystem/VSTAudioSystem.hpp"
#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// AudioBlock32: Pre-allocated 32-bit Floating-Point Multichannel Audio Buffer
// Supports Mono (1), Stereo (2), LCR (3), Quad/LCRS (4), 5.0 (5), Surround 5.1 (6)
// Zero heap allocations during processBlockRT.
// ============================================================================
class AudioBlock32 {
public:
    static constexpr std::uint32_t kMaxChannels = 6; // L, R, C, LFE, Ls, Rs
    static constexpr std::uint32_t kDefaultMaxFrames = 4096;

    AudioBlock32(SpeakerConfig config = SpeakerConfig::Stereo,
                 std::uint32_t maxFrames = kDefaultMaxFrames);

    void prepare(SpeakerConfig config, std::uint32_t maxFrames);
    void clearRT(std::uint32_t numFrames) noexcept;
    void copyFromRT(const AudioBlock32& src, std::uint32_t numFrames) noexcept;
    void accumulateFromRT(const AudioBlock32& src, std::uint32_t numFrames, float gain = 1.0f) noexcept;
    void applyGainRT(std::uint32_t numFrames, float gain) noexcept;

    [[nodiscard]] SpeakerConfig speakerConfig() const noexcept { return config_; }
    [[nodiscard]] std::uint32_t numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] std::uint32_t maxFrames() const noexcept { return maxFrames_; }

    [[nodiscard]] float* channel(std::uint32_t ch) noexcept {
        return (ch < numChannels_) ? channelPtrs_[ch] : nullptr;
    }
    [[nodiscard]] const float* channel(std::uint32_t ch) const noexcept {
        return (ch < numChannels_) ? channelPtrs_[ch] : nullptr;
    }
    [[nodiscard]] float* const* channelPointers() noexcept { return channelPtrs_.data(); }
    [[nodiscard]] const float* const* channelPointers() const noexcept { return channelPtrs_.data(); }

    [[nodiscard]] float computePeakRT(std::uint32_t ch, std::uint32_t numFrames) const noexcept;
    [[nodiscard]] float computeRmsRT(std::uint32_t ch, std::uint32_t numFrames) const noexcept;

private:
    SpeakerConfig config_{SpeakerConfig::Stereo};
    std::uint32_t numChannels_{2};
    std::uint32_t maxFrames_{kDefaultMaxFrames};
    std::array<std::vector<float>, kMaxChannels> storage_{};
    std::array<float*, kMaxChannels> channelPtrs_{};
};

// ============================================================================
// SignalDAG: Directed Acyclic Graph for Unlimited Tracks, Buses & Sidechains
// ============================================================================
enum class DAGNodeKind : std::uint8_t {
    PhysicalInput     = 0,
    AudioTrack        = 1,
    MIDITrack         = 2,
    InstrumentTrack   = 3,
    GroupChannel      = 4,
    FXReturnChannel   = 5,
    ExternalLoopback  = 6,
    PhysicalOutput    = 7,
    ControlRoomBus    = 8
};

enum class DAGEdgeRole : std::uint8_t {
    MainAudioRoute = 0,
    AuxFXSend      = 1,
    SidechainInput = 2,
    StudioCueSend  = 3
};

struct DAGEdge {
    std::uint32_t sourceNodeId{0};
    std::uint32_t destNodeId{0};
    DAGEdgeRole role{DAGEdgeRole::MainAudioRoute};
    float sendGainLinear{1.0f};
    std::uint32_t auxSlotIndex{0};
};

struct DAGNode {
    using NodeProcessCallback = void (*)(void* userObject, AudioBlock32& mainIo,
                                         const AudioBlock32* sidechainIn,
                                         std::uint32_t numFrames) noexcept;

    std::uint32_t nodeId{0};
    std::string name;
    DAGNodeKind kind{DAGNodeKind::AudioTrack};
    SpeakerConfig speakerConfig{SpeakerConfig::Stereo};
    std::uint32_t selfLatencySamples{0};
    bool bypassed{false};

    void* userObject{nullptr};
    NodeProcessCallback processCallback{nullptr};

    AudioBlock32 outputBuffer;
    AudioBlock32 sidechainBuffer;
    DelayCompensationLine pdcDelayLine;
};

struct DAGExecutionLayer {
    std::vector<std::uint32_t> nodeIndices; // Indices into SignalDAG::nodes_
};

struct DAGExecutionPlan {
    std::vector<DAGExecutionLayer> topologicalLayers;
    std::uint32_t totalSystemPDCSamples{0};
};

class SignalDAG {
public:
    SignalDAG() = default;
    ~SignalDAG() = default;

    SignalDAG(const SignalDAG&) = delete;
    SignalDAG& operator=(const SignalDAG&) = delete;

    std::uint32_t addNode(const std::string& name, DAGNodeKind kind,
                          SpeakerConfig config, std::uint32_t maxBlockFrames = 4096,
                          std::uint32_t selfLatencySamples = 0,
                          void* userObject = nullptr,
                          DAGNode::NodeProcessCallback cb = nullptr);

    // Rejects edges that would introduce a feedback cycle into the DAG
    bool connectNodes(std::uint32_t sourceNodeId, std::uint32_t destNodeId,
                      DAGEdgeRole role = DAGEdgeRole::MainAudioRoute,
                      float sendGainLinear = 1.0f,
                      std::uint32_t auxSlotIndex = 0);
    void disconnectNodes(std::uint32_t sourceNodeId, std::uint32_t destNodeId,
                         DAGEdgeRole role = DAGEdgeRole::MainAudioRoute);

    // Compiles topological execution layers + per-node PDC compensation and atomically swaps plan
    bool compileExecutionPlan();

    // Real-time safe execution of the active topological plan across MultiProcessingPool
    void processGraphRT(std::uint32_t numFrames, MultiProcessingPool* pool = nullptr) noexcept;

    [[nodiscard]] DAGNode* findNode(std::uint32_t nodeId) noexcept;
    [[nodiscard]] const DAGNode* findNode(std::uint32_t nodeId) const noexcept;
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::uint32_t totalSystemPDCSamples() const noexcept;

private:
    [[nodiscard]] int findNodeIndex(std::uint32_t nodeId) const noexcept;
    [[nodiscard]] bool wouldCreateCycle(std::uint32_t srcId, std::uint32_t dstId) const;
    static void executeNodeTaskRT(void* context, std::uint32_t nodeIndex, std::uint32_t numFrames) noexcept;

    std::uint32_t nextNodeId_{1};
    std::vector<std::unique_ptr<DAGNode>> nodes_;
    std::vector<DAGEdge> edges_;
    std::vector<std::vector<DAGEdge>> incomingEdgesByNodeIdx_;

    std::array<DAGExecutionPlan, 2> planBuffers_{};
    std::size_t writePlanIdx_{0};
    std::atomic<const DAGExecutionPlan*> activePlan_{nullptr};
};

} // namespace AlphaStudio::Core
