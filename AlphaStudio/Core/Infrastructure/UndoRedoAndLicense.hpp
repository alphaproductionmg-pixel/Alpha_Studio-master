#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// Command Pattern & Unlimited UndoRedoManager
// ============================================================================
class ICommand {
public:
    virtual ~ICommand() = default;

    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual void redo() { execute(); }

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::size_t memoryBytes() const noexcept { return sizeof(*this); }

    [[nodiscard]] virtual bool canMergeWith(const ICommand& /*other*/) const noexcept {
        return false;
    }
    virtual void mergeWith(const ICommand& /*other*/) {}
};

class LambdaCommand final : public ICommand {
public:
    LambdaCommand(std::string actionName, std::function<void()> doFn, std::function<void()> undoFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

private:
    std::string name_;
    std::function<void()> doFn_;
    std::function<void()> undoFn_;
};

class CompoundMacroCommand final : public ICommand {
public:
    explicit CompoundMacroCommand(std::string macroName);

    void addStep(std::unique_ptr<ICommand> step);
    [[nodiscard]] bool empty() const noexcept { return steps_.empty(); }

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept override;

private:
    std::string name_;
    std::vector<std::unique_ptr<ICommand>> steps_;
};

struct HistoryEntrySummary {
    std::size_t index{0};
    std::string name;
    bool isExecuted{false};
    bool isSavedMark{false};
};

class UndoRedoManager {
public:
    UndoRedoManager() = default;
    ~UndoRedoManager() = default;

    UndoRedoManager(const UndoRedoManager&) = delete;
    UndoRedoManager& operator=(const UndoRedoManager&) = delete;

    // Unlimited history by default (maxDepth == 0 means unlimited).
    void setMaxHistoryDepth(std::size_t maxDepth);
    [[nodiscard]] std::size_t maxHistoryDepth() const noexcept;

    void executeCommand(std::unique_ptr<ICommand> command, bool allowMerge = true);

    void beginTransaction(std::string transactionName);
    void endTransaction();

    bool undo();
    bool redo();
    bool jumpToHistoryIndex(std::size_t targetCursor);

    void markSavedState();
    [[nodiscard]] bool isDirty() const;

    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] std::string nextUndoName() const;
    [[nodiscard]] std::string nextRedoName() const;
    [[nodiscard]] std::size_t historyCount() const;
    [[nodiscard]] std::size_t cursorPosition() const;
    [[nodiscard]] std::vector<HistoryEntrySummary> inspectHistory() const;

    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ICommand>> history_;
    std::size_t cursor_{0};
    std::size_t savedCursor_{0};
    std::size_t maxDepth_{0}; // 0 == unlimited
    std::unique_ptr<CompoundMacroCommand> activeTransaction_;
    std::uint32_t transactionNesting_{0};
};

// ============================================================================
// LicenseSessionManager: Clean hardware-independent license & capability layer
// ============================================================================
enum class LicenseEdition : std::uint8_t {
    Evaluation  = 0,
    Artist      = 1,
    StudioPro   = 2
};

enum class StudioCapability : std::uint32_t {
    UnlimitedAudioMidiTracks = 1U << 0,
    Surround51Routing        = 1U << 1,
    ControlRoomMonitoring    = 1U << 2,
    VariAudioPitchEditing    = 1U << 3,
    REVerenceConvolution     = 1U << 4,
    ChannelBatchExport       = 1U << 5,
    VSTExpressionAndPLE      = 1U << 6,
    ExternalFXAndInstruments = 1U << 7,
    VSTSystemLinkSync        = 1U << 8
};

struct LicenseToken {
    std::string licenseeName{"AlphaStudio Licensed User"};
    std::string serialNumber{"ASPR-2026-0001-FULL-88F4"};
    LicenseEdition edition{LicenseEdition::StudioPro};
    std::uint32_t capabilityMask{0xFFFFFFFFU};
    std::uint64_t signatureHash{0};
};

class LicenseSessionManager {
public:
    LicenseSessionManager();

    bool activateSession(const LicenseToken& token);
    void deactivateSession() noexcept;

    [[nodiscard]] bool isSessionValid() const noexcept { return valid_; }
    [[nodiscard]] LicenseEdition edition() const noexcept { return token_.edition; }
    [[nodiscard]] bool hasCapability(StudioCapability cap) const noexcept;
    [[nodiscard]] std::uint32_t maxPhysicalIOChannels() const noexcept;
    [[nodiscard]] const LicenseToken& currentToken() const noexcept { return token_; }

    [[nodiscard]] static std::uint64_t computeTokenSignature(
        std::string_view licensee, std::string_view serial,
        LicenseEdition edition, std::uint32_t mask) noexcept;
    [[nodiscard]] static LicenseToken createDefaultStudioProToken(
        std::string licensee = "AlphaStudio Studio Pro Workstation");

private:
    LicenseToken token_{};
    bool valid_{false};
};

} // namespace AlphaStudio::Core
