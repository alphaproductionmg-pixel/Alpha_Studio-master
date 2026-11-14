#include "AlphaStudio/SDK/Bridge/VSTBridgeIPC.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AlphaStudio::SDK::Bridge {

// ============================================================================
// SharedMemoryRegion
// ============================================================================
SharedMemoryRegion::~SharedMemoryRegion() noexcept {
    close();
}

std::string SharedMemoryRegion::makeUACCompliantSessionName(const std::string& token) {
    std::string sanitized;
    sanitized.reserve(token.size());
    for (char c : token) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            sanitized.push_back(c);
        } else {
            sanitized.push_back('_');
        }
    }
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    // UAC-compliant Local\ session namespace (never Global\, zero admin privileges)
    return "Local\\AlphaStudioBridge_" + sanitized;
#else
    // POSIX / macOS XNU PSHMNAMLEN <= 31 bytes strict compliance:
    // "/asb_" (5 chars) + max 24 chars = <= 29 chars (+ NUL <= 30 bytes)
    if (sanitized.size() > 24U) {
        sanitized.resize(24U);
    }
    return "/asb_" + sanitized;
#endif
}

std::string SharedMemoryRegion::generateUACAsInvokerManifestXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
           "  <assemblyIdentity version=\"1.0.0.0\" processorArchitecture=\"x86\" "
           "name=\"AlphaStudio.VSTBridge32\" type=\"win32\"/>\n"
           "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
           "    <security>\n"
           "      <requestedPrivileges>\n"
           "        <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>\n"
           "      </requestedPrivileges>\n"
           "    </security>\n"
           "  </trustInfo>\n"
           "</assembly>\n";
}

bool SharedMemoryRegion::createOrOpen(const std::string& sessionToken, bool createNew) {
    close();
    ipcName_ = makeUACCompliantSessionName(sessionToken);
    isCreator_ = createNew;

#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    HANDLE hMap = nullptr;
    if (createNew) {
        hMap = ::CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(SharedBridgeMemoryLayout)), ipcName_.c_str());
    } else {
        hMap = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, ipcName_.c_str());
    }
    if (hMap == nullptr) {
        return false;
    }
    void* ptr = ::MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBridgeMemoryLayout));
    if (ptr == nullptr) {
        ::CloseHandle(hMap);
        return false;
    }
    winMappingHandle_ = hMap;
    if (createNew) {
        mapped_ = new (ptr) SharedBridgeMemoryLayout();
    } else {
        mapped_ = static_cast<SharedBridgeMemoryLayout*>(ptr);
    }
    return true;
#else
    if (createNew) {
        ::shm_unlink(ipcName_.c_str());
    }
    const int flags = createNew ? (O_CREAT | O_RDWR) : O_RDWR;
    const int fd = ::shm_open(ipcName_.c_str(), flags, 0600);
    if (fd < 0) {
        return false;
    }
    if (createNew) {
        if (::ftruncate(fd, static_cast<off_t>(sizeof(SharedBridgeMemoryLayout))) != 0) {
            ::close(fd);
            ::shm_unlink(ipcName_.c_str());
            return false;
        }
    }
    void* ptr = ::mmap(nullptr, sizeof(SharedBridgeMemoryLayout),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd);
        if (createNew) {
            ::shm_unlink(ipcName_.c_str());
        }
        return false;
    }
    shmFd_ = fd;
    if (createNew) {
        mapped_ = new (ptr) SharedBridgeMemoryLayout();
    } else {
        mapped_ = static_cast<SharedBridgeMemoryLayout*>(ptr);
    }
    return true;
#endif
}

void SharedMemoryRegion::close() noexcept {
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
    if (mapped_ != nullptr) {
        ::UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (winMappingHandle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(winMappingHandle_));
        winMappingHandle_ = nullptr;
    }
#else
    if (mapped_ != nullptr) {
        ::munmap(mapped_, sizeof(SharedBridgeMemoryLayout));
        mapped_ = nullptr;
    }
    if (shmFd_ >= 0) {
        ::close(shmFd_);
        shmFd_ = -1;
    }
    if (isCreator_ && !ipcName_.empty()) {
        ::shm_unlink(ipcName_.c_str());
    }
#endif
    ipcName_.clear();
}

// ============================================================================
// VSTBridgeHostProxy
// ============================================================================
VSTBridgeHostProxy::~VSTBridgeHostProxy() noexcept {
    disconnect();
}

bool VSTBridgeHostProxy::connectSession(const std::string& sessionToken,
                                        double sampleRate,
                                        std::uint32_t blockFrames,
                                        std::uint32_t numInputs,
                                        std::uint32_t numOutputs) {
    if (!shm_.createOrOpen(sessionToken, true)) {
        return false;
    }
    auto* mem = shm_.layout();
    mem->sampleRate = sampleRate;
    mem->blockFrames = std::min(blockFrames, SharedBridgeMemoryLayout::kMaxBlockFrames);
    // Explicitly supports Mono (1 channel) as well as Stereo (2) and Surround (up to 6)
    mem->numInputChannels = std::clamp(numInputs, 0U, SharedBridgeMemoryLayout::kMaxChannels);
    mem->numOutputChannels = std::clamp(numOutputs, 1U, SharedBridgeMemoryLayout::kMaxChannels);
    return true;
}

void VSTBridgeHostProxy::disconnect() noexcept {
    shm_.close();
}

bool VSTBridgeHostProxy::executeRoundTripCommand(BridgeCommand cmd) noexcept {
    auto* mem = shm_.layout();
    if (mem == nullptr) {
        return false;
    }
    mem->command.store(static_cast<std::uint32_t>(cmd), std::memory_order_release);
    const std::uint64_t seq = mem->requestSeq.fetch_add(1, std::memory_order_acq_rel) + 1ULL;

    if (mem->workerAlive.load(std::memory_order_acquire) == 0U) {
        return false;
    }

    std::uint32_t spinGuard = 0;
    while (mem->responseSeq.load(std::memory_order_acquire) < seq) {
        if (++spinGuard > 2000000U) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool VSTBridgeHostProxy::processBridgedBlockRT(
    const float* const* inputs, std::uint32_t numIn,
    float* const* outputs, std::uint32_t numOut,
    std::uint32_t sampleFrames,
    std::span<const BridgeMidiItem> midiIn,
    std::vector<BridgeMidiItem>* midiOut) noexcept {
    auto* mem = shm_.layout();
    if (mem == nullptr || outputs == nullptr) {
        return false;
    }
    const std::uint32_t frames = std::min(sampleFrames, SharedBridgeMemoryLayout::kMaxBlockFrames);
    const std::uint32_t inCh = std::min({numIn, mem->numInputChannels, SharedBridgeMemoryLayout::kMaxChannels});
    const std::uint32_t outCh = std::min({numOut, mem->numOutputChannels, SharedBridgeMemoryLayout::kMaxChannels});

    mem->blockFrames = frames;
    mem->numInputChannels = inCh;
    mem->numOutputChannels = outCh;

    for (std::uint32_t c = 0; c < inCh; ++c) {
        if (inputs != nullptr && inputs[c] != nullptr) {
            std::memcpy(mem->audioIn[c].data(), inputs[c], frames * sizeof(float));
        } else {
            std::memset(mem->audioIn[c].data(), 0, frames * sizeof(float));
        }
    }

    const std::uint32_t mCount = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(midiIn.size()), SharedBridgeMemoryLayout::kMaxMidiEvents);
    mem->numMidiIn = mCount;
    for (std::uint32_t i = 0; i < mCount; ++i) {
        mem->midiIn[i] = midiIn[i];
    }

    if (!executeRoundTripCommand(BridgeCommand::ProcessAudioAndMidi)) {
        return false;
    }

    for (std::uint32_t c = 0; c < outCh; ++c) {
        if (outputs[c] != nullptr) {
            std::memcpy(outputs[c], mem->audioOut[c].data(), frames * sizeof(float));
        }
    }
    if (midiOut != nullptr) {
        const std::uint32_t outEvents = std::min(mem->numMidiOut, SharedBridgeMemoryLayout::kMaxMidiEvents);
        midiOut->assign(mem->midiOut.begin(), mem->midiOut.begin() + outEvents);
    }
    return true;
}

std::vector<BridgeParameterEntry> VSTBridgeHostProxy::fetchAllParametersPaged() {
    std::vector<BridgeParameterEntry> result;
    auto* mem = shm_.layout();
    if (mem == nullptr) {
        return result;
    }
    std::uint32_t offset = 0;
    while (true) {
        mem->pageOffset = offset;
        if (!executeRoundTripCommand(BridgeCommand::ReadParamPage)) {
            break;
        }
        if (result.empty() && mem->totalPluginParameters > 0) {
            result.reserve(mem->totalPluginParameters);
        }
        const std::uint32_t count = std::min(mem->pageCount, SharedBridgeMemoryLayout::kEntriesPerPage);
        if (count == 0) {
            break;
        }
        result.insert(result.end(), mem->paramPage.begin(), mem->paramPage.begin() + count);
        offset += count;
        if (offset >= mem->totalPluginParameters) {
            break;
        }
    }
    return result;
}

bool VSTBridgeHostProxy::pushParametersPaged(std::span<const BridgeParameterEntry> updates) {
    auto* mem = shm_.layout();
    if (mem == nullptr) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < updates.size()) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(
            std::min<std::size_t>(updates.size() - offset, SharedBridgeMemoryLayout::kEntriesPerPage));
        for (std::uint32_t i = 0; i < chunk; ++i) {
            mem->paramPage[i] = updates[offset + i];
        }
        mem->pageOffset = static_cast<std::uint32_t>(offset);
        mem->pageCount = chunk;
        if (!executeRoundTripCommand(BridgeCommand::WriteParamPage)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

std::vector<BridgePresetEntry> VSTBridgeHostProxy::fetchAllPresetsPaged() {
    std::vector<BridgePresetEntry> result;
    auto* mem = shm_.layout();
    if (mem == nullptr) {
        return result;
    }
    std::uint32_t offset = 0;
    while (true) {
        mem->pageOffset = offset;
        if (!executeRoundTripCommand(BridgeCommand::ReadPresetListPage)) {
            break;
        }
        if (result.empty() && mem->totalPluginPresets > 0) {
            result.reserve(mem->totalPluginPresets);
        }
        const std::uint32_t count = std::min(mem->pageCount, SharedBridgeMemoryLayout::kEntriesPerPage);
        if (count == 0) {
            break;
        }
        result.insert(result.end(), mem->presetPage.begin(), mem->presetPage.begin() + count);
        offset += count;
        if (offset >= mem->totalPluginPresets) {
            break;
        }
    }
    return result;
}

// ============================================================================
// VSTBridgeWorkerServer
// ============================================================================
VSTBridgeWorkerServer::VSTBridgeWorkerServer(std::uint32_t simulatedParamCount,
                                             std::uint32_t simulatedPresetCount) {
    fullParameterBank_.resize(simulatedParamCount);
    for (std::uint32_t i = 0; i < simulatedParamCount; ++i) {
        fullParameterBank_[i].paramIndex = i;
        fullParameterBank_[i].normalizedValue = (i == 0) ? 1.0f : 0.5f;
        std::snprintf(fullParameterBank_[i].name, sizeof(fullParameterBank_[i].name),
                      "Bridged32 Param %u", i + 1U);
        std::snprintf(fullParameterBank_[i].display, sizeof(fullParameterBank_[i].display),
                      "%.2f", fullParameterBank_[i].normalizedValue);
    }

    fullPresetBank_.resize(simulatedPresetCount);
    for (std::uint32_t p = 0; p < simulatedPresetCount; ++p) {
        fullPresetBank_[p].presetIndex = p;
        std::snprintf(fullPresetBank_[p].name, sizeof(fullPresetBank_[p].name),
                      "Bank Preset %04u", p + 1U);
    }
}

VSTBridgeWorkerServer::~VSTBridgeWorkerServer() noexcept {
    detach();
}

bool VSTBridgeWorkerServer::attachSession(const std::string& sessionToken) {
    if (!shm_.createOrOpen(sessionToken, false)) {
        return false;
    }
    auto* mem = shm_.layout();
    if (mem == nullptr || mem->magic != SharedBridgeMemoryLayout::kProtocolMagic) {
        shm_.close();
        return false;
    }
    mem->totalPluginParameters = static_cast<std::uint32_t>(fullParameterBank_.size());
    mem->totalPluginPresets = static_cast<std::uint32_t>(fullPresetBank_.size());
    mem->workerAlive.store(1U, std::memory_order_release);
    lastHandledSeq_ = mem->responseSeq.load(std::memory_order_acquire);
    return true;
}

void VSTBridgeWorkerServer::detach() noexcept {
    if (auto* mem = shm_.layout()) {
        mem->workerAlive.store(0U, std::memory_order_release);
    }
    shm_.close();
}

bool VSTBridgeWorkerServer::pollAndExecuteOnce() noexcept {
    auto* mem = shm_.layout();
    if (mem == nullptr) {
        return false;
    }
    const std::uint64_t req = mem->requestSeq.load(std::memory_order_acquire);
    if (req <= lastHandledSeq_) {
        return false;
    }

    const auto cmd = static_cast<BridgeCommand>(mem->command.load(std::memory_order_acquire));
    switch (cmd) {
        case BridgeCommand::ConfigureIO:
            break;

        case BridgeCommand::ProcessAudioAndMidi: {
            const std::uint32_t frames = std::min(mem->blockFrames, SharedBridgeMemoryLayout::kMaxBlockFrames);
            const std::uint32_t inCh = std::min(mem->numInputChannels, SharedBridgeMemoryLayout::kMaxChannels);
            const std::uint32_t outCh = std::min(mem->numOutputChannels, SharedBridgeMemoryLayout::kMaxChannels);

            // Handles true Mono (inCh == 1, outCh == 1), Mono->Stereo (1->2), and Multichannel
            for (std::uint32_t c = 0; c < outCh; ++c) {
                const std::uint32_t srcCh = (inCh == 1U) ? 0U : c;
                if (inCh > 0U && srcCh < inCh) {
                    for (std::uint32_t f = 0; f < frames; ++f) {
                        mem->audioOut[c][f] = mem->audioIn[srcCh][f] * bridgedGainLinear_;
                    }
                } else {
                    std::memset(mem->audioOut[c].data(), 0, frames * sizeof(float));
                }
            }
            mem->numMidiOut = mem->numMidiIn;
            for (std::uint32_t m = 0; m < mem->numMidiOut; ++m) {
                mem->midiOut[m] = mem->midiIn[m];
            }
            break;
        }

        case BridgeCommand::ReadParamPage: {
            mem->totalPluginParameters = static_cast<std::uint32_t>(fullParameterBank_.size());
            const std::size_t off = mem->pageOffset;
            if (off >= fullParameterBank_.size()) {
                mem->pageCount = 0;
            } else {
                const std::size_t count = std::min<std::size_t>(
                    fullParameterBank_.size() - off, SharedBridgeMemoryLayout::kEntriesPerPage);
                for (std::size_t i = 0; i < count; ++i) {
                    mem->paramPage[i] = fullParameterBank_[off + i];
                }
                mem->pageCount = static_cast<std::uint32_t>(count);
            }
            break;
        }

        case BridgeCommand::WriteParamPage: {
            const std::uint32_t count = std::min(mem->pageCount, SharedBridgeMemoryLayout::kEntriesPerPage);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& entry = mem->paramPage[i];
                if (entry.paramIndex < fullParameterBank_.size()) {
                    fullParameterBank_[entry.paramIndex].normalizedValue =
                        std::clamp(entry.normalizedValue, 0.0f, 1.0f);
                    if (entry.paramIndex == 0U) {
                        bridgedGainLinear_ = fullParameterBank_[0].normalizedValue;
                    }
                }
            }
            break;
        }

        case BridgeCommand::ReadPresetListPage: {
            mem->totalPluginPresets = static_cast<std::uint32_t>(fullPresetBank_.size());
            const std::size_t off = mem->pageOffset;
            if (off >= fullPresetBank_.size()) {
                mem->pageCount = 0;
            } else {
                const std::size_t count = std::min<std::size_t>(
                    fullPresetBank_.size() - off, SharedBridgeMemoryLayout::kEntriesPerPage);
                for (std::size_t i = 0; i < count; ++i) {
                    mem->presetPage[i] = fullPresetBank_[off + i];
                }
                mem->pageCount = static_cast<std::uint32_t>(count);
            }
            break;
        }

        case BridgeCommand::SelectPreset:
        case BridgeCommand::Idle:
        case BridgeCommand::ShutdownWorker:
            break;
    }

    lastHandledSeq_ = req;
    mem->responseSeq.store(req, std::memory_order_release);
    return true;
}

} // namespace AlphaStudio::SDK::Bridge
