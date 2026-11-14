#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::SDK::Bridge {

// ============================================================================
// VST Bridge (IPC 32/64-bit Shared Memory Isolation Engine)
// - Runs 32-bit VST 2.4 / VST 3 plugins inside 64-bit AlphaStudio via IPC
// - Explicit support for Mono (1ch), Stereo (2ch), and Surround (up to 6ch)
// - Paged shared-memory transfer for arbitrarily large parameter & preset lists
// - Strict UAC compliance (runs at standard user integrity "asInvoker", using
//   session-local user-namespace IPC objects without HKLM or admin elevation)
// ============================================================================
enum class BridgeCommand : std::uint32_t {
    Idle                  = 0,
    ConfigureIO           = 1,
    ProcessAudioAndMidi   = 2,
    ReadParamPage         = 3,
    WriteParamPage        = 4,
    ReadPresetListPage    = 5,
    SelectPreset          = 6,
    ShutdownWorker        = 7
};

struct BridgeParameterEntry {
    std::uint32_t paramIndex{0};
    float normalizedValue{0.0f};
    char name[48]{};
    char display[24]{};
};

struct BridgePresetEntry {
    std::uint32_t presetIndex{0};
    char name[60]{};
};

struct BridgeMidiItem {
    std::int32_t deltaFrames{0};
    std::uint8_t status{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::uint8_t padding{0};
};

// Shared Memory Layout (POD, fixed-offset across 32-bit and 64-bit ABIs)
struct alignas(64) SharedBridgeMemoryLayout {
    static constexpr std::uint32_t kProtocolMagic   = 0x41534252U; // 'ASBR'
    static constexpr std::uint32_t kMaxChannels     = 6;           // Mono=1, Stereo=2 .. 5.1=6
    static constexpr std::uint32_t kMaxBlockFrames  = 4096;
    static constexpr std::uint32_t kEntriesPerPage  = 256;         // Paged for 10,000+ params/presets
    static constexpr std::uint32_t kMaxMidiEvents   = 256;

    std::uint32_t magic{kProtocolMagic};
    std::uint32_t version{100};
    std::atomic<std::uint32_t> command{static_cast<std::uint32_t>(BridgeCommand::Idle)};
    std::atomic<std::uint64_t> requestSeq{0};
    std::atomic<std::uint64_t> responseSeq{0};
    std::atomic<std::uint32_t> workerAlive{0};

    double sampleRate{48000.0};
    std::uint32_t blockFrames{256};
    std::uint32_t numInputChannels{2};  // Explicitly supports 1 (Mono), 2, ..., 6
    std::uint32_t numOutputChannels{2}; // Explicitly supports 1 (Mono), 2, ..., 6

    // Paging metadata for large parameter & preset lists
    std::uint32_t totalPluginParameters{0};
    std::uint32_t totalPluginPresets{0};
    std::uint32_t pageOffset{0};
    std::uint32_t pageCount{0};
    std::uint32_t selectedPresetIndex{0};

    std::uint32_t numMidiIn{0};
    std::uint32_t numMidiOut{0};
    std::array<BridgeMidiItem, kMaxMidiEvents> midiIn{};
    std::array<BridgeMidiItem, kMaxMidiEvents> midiOut{};

    std::array<BridgeParameterEntry, kEntriesPerPage> paramPage{};
    std::array<BridgePresetEntry, kEntriesPerPage> presetPage{};

    std::array<std::array<float, kMaxBlockFrames>, kMaxChannels> audioIn{};
    std::array<std::array<float, kMaxBlockFrames>, kMaxChannels> audioOut{};
};

class SharedMemoryRegion {
public:
    SharedMemoryRegion() = default;
    ~SharedMemoryRegion() noexcept;

    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

    bool createOrOpen(const std::string& sessionToken, bool createNew);
    void close() noexcept;

    [[nodiscard]] SharedBridgeMemoryLayout* layout() noexcept { return mapped_; }
    [[nodiscard]] const SharedBridgeMemoryLayout* layout() const noexcept { return mapped_; }
    [[nodiscard]] const std::string& ipcObjectName() const noexcept { return ipcName_; }

    // Generates UAC-compliant user-session IPC object name & manifest
    [[nodiscard]] static std::string makeUACCompliantSessionName(const std::string& token);
    [[nodiscard]] static std::string generateUACAsInvokerManifestXml();

private:
    std::string ipcName_;
    bool isCreator_{false};
    SharedBridgeMemoryLayout* mapped_{nullptr};
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    void* winMappingHandle_{nullptr};
#else
    int shmFd_{-1};
#endif
};

// Host-side 64-bit Proxy communicating with the 32-bit Worker Server
class VSTBridgeHostProxy {
public:
    VSTBridgeHostProxy() = default;
    ~VSTBridgeHostProxy() noexcept;

    bool connectSession(const std::string& sessionToken,
                        double sampleRate,
                        std::uint32_t blockFrames,
                        std::uint32_t numInputs,  // Supports 1 (Mono) .. 6
                        std::uint32_t numOutputs);
    void disconnect() noexcept;

    // Real-time safe audio + MIDI block exchange (handles mono & multi-channel)
    bool processBridgedBlockRT(const float* const* inputs, std::uint32_t numIn,
                               float* const* outputs, std::uint32_t numOut,
                               std::uint32_t sampleFrames,
                               std::span<const BridgeMidiItem> midiIn,
                               std::vector<BridgeMidiItem>* midiOut = nullptr) noexcept;

    // Paged retrieval of arbitrarily long parameter & preset lists (e.g. > 4096 entries)
    std::vector<BridgeParameterEntry> fetchAllParametersPaged();
    bool pushParametersPaged(std::span<const BridgeParameterEntry> updates);
    std::vector<BridgePresetEntry> fetchAllPresetsPaged();

    [[nodiscard]] SharedBridgeMemoryLayout* sharedLayout() noexcept { return shm_.layout(); }

private:
    bool executeRoundTripCommand(BridgeCommand cmd) noexcept;
    SharedMemoryRegion shm_;
};

// Worker-side 32-bit Isolated Plugin Host Server
class VSTBridgeWorkerServer {
public:
    VSTBridgeWorkerServer(std::uint32_t simulatedParamCount = 2048,
                          std::uint32_t simulatedPresetCount = 1024);
    ~VSTBridgeWorkerServer() noexcept;

    bool attachSession(const std::string& sessionToken);
    void detach() noexcept;

    // Processes one pending IPC command from SharedBridgeMemoryLayout
    bool pollAndExecuteOnce() noexcept;

private:
    SharedMemoryRegion shm_;
    std::uint64_t lastHandledSeq_{0};
    float bridgedGainLinear_{1.0f};
    std::vector<BridgeParameterEntry> fullParameterBank_;
    std::vector<BridgePresetEntry> fullPresetBank_;
};

} // namespace AlphaStudio::SDK::Bridge
