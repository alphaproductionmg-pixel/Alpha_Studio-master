#include "AlphaStudio/Core/Infrastructure/UndoRedoAndLicense.hpp"

#include <utility>

namespace AlphaStudio::Core {

// ============================================================================
// LambdaCommand & CompoundMacroCommand
// ============================================================================
LambdaCommand::LambdaCommand(std::string actionName,
                             std::function<void()> doFn,
                             std::function<void()> undoFn)
    : name_(std::move(actionName)),
      doFn_(std::move(doFn)),
      undoFn_(std::move(undoFn)) {}

void LambdaCommand::execute() {
    if (doFn_) {
        doFn_();
    }
}

void LambdaCommand::undo() {
    if (undoFn_) {
        undoFn_();
    }
}

CompoundMacroCommand::CompoundMacroCommand(std::string macroName)
    : name_(std::move(macroName)) {}

void CompoundMacroCommand::addStep(std::unique_ptr<ICommand> step) {
    if (step) {
        steps_.push_back(std::move(step));
    }
}

void CompoundMacroCommand::execute() {
    for (auto& step : steps_) {
        step->execute();
    }
}

void CompoundMacroCommand::undo() {
    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
        (*it)->undo();
    }
}

void CompoundMacroCommand::redo() {
    for (auto& step : steps_) {
        step->redo();
    }
}

std::size_t CompoundMacroCommand::memoryBytes() const noexcept {
    std::size_t total = sizeof(*this) + name_.capacity();
    for (const auto& step : steps_) {
        total += step->memoryBytes();
    }
    return total;
}

// ============================================================================
// UndoRedoManager
// ============================================================================
void UndoRedoManager::setMaxHistoryDepth(std::size_t maxDepth) {
    std::scoped_lock lock(mutex_);
    maxDepth_ = maxDepth;
    if (maxDepth_ > 0 && history_.size() > maxDepth_) {
        const std::size_t excess = history_.size() - maxDepth_;
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(excess));
        cursor_ = (cursor_ > excess) ? (cursor_ - excess) : 0;
        savedCursor_ = (savedCursor_ > excess) ? (savedCursor_ - excess) : 0;
    }
}

std::size_t UndoRedoManager::maxHistoryDepth() const noexcept {
    std::scoped_lock lock(mutex_);
    return maxDepth_;
}

void UndoRedoManager::executeCommand(std::unique_ptr<ICommand> command, bool allowMerge) {
    if (!command) {
        return;
    }
    std::scoped_lock lock(mutex_);
    command->execute();

    if (transactionNesting_ > 0 && activeTransaction_) {
        activeTransaction_->addStep(std::move(command));
        return;
    }

    if (cursor_ < history_.size()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    }

    if (allowMerge && !history_.empty() && cursor_ == history_.size()) {
        ICommand& previous = *history_.back();
        if (previous.canMergeWith(*command)) {
            previous.mergeWith(*command);
            return;
        }
    }

    history_.push_back(std::move(command));
    cursor_ = history_.size();

    if (maxDepth_ > 0 && history_.size() > maxDepth_) {
        history_.erase(history_.begin());
        --cursor_;
        if (savedCursor_ > 0) {
            --savedCursor_;
        }
    }
}

void UndoRedoManager::beginTransaction(std::string transactionName) {
    std::scoped_lock lock(mutex_);
    if (transactionNesting_ == 0) {
        activeTransaction_ = std::make_unique<CompoundMacroCommand>(std::move(transactionName));
    }
    ++transactionNesting_;
}

void UndoRedoManager::endTransaction() {
    std::scoped_lock lock(mutex_);
    if (transactionNesting_ == 0) {
        return;
    }
    --transactionNesting_;
    if (transactionNesting_ == 0 && activeTransaction_) {
        if (!activeTransaction_->empty()) {
            if (cursor_ < history_.size()) {
                history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
            }
            history_.push_back(std::move(activeTransaction_));
            cursor_ = history_.size();
        }
        activeTransaction_.reset();
    }
}

bool UndoRedoManager::undo() {
    std::scoped_lock lock(mutex_);
    if (cursor_ == 0 || history_.empty()) {
        return false;
    }
    --cursor_;
    history_[cursor_]->undo();
    return true;
}

bool UndoRedoManager::redo() {
    std::scoped_lock lock(mutex_);
    if (cursor_ >= history_.size()) {
        return false;
    }
    history_[cursor_]->redo();
    ++cursor_;
    return true;
}

bool UndoRedoManager::jumpToHistoryIndex(std::size_t targetCursor) {
    std::scoped_lock lock(mutex_);
    if (targetCursor > history_.size()) {
        return false;
    }
    while (cursor_ > targetCursor) {
        --cursor_;
        history_[cursor_]->undo();
    }
    while (cursor_ < targetCursor) {
        history_[cursor_]->redo();
        ++cursor_;
    }
    return true;
}

void UndoRedoManager::markSavedState() {
    std::scoped_lock lock(mutex_);
    savedCursor_ = cursor_;
}

bool UndoRedoManager::isDirty() const {
    std::scoped_lock lock(mutex_);
    return cursor_ != savedCursor_;
}

bool UndoRedoManager::canUndo() const {
    std::scoped_lock lock(mutex_);
    return cursor_ > 0;
}

bool UndoRedoManager::canRedo() const {
    std::scoped_lock lock(mutex_);
    return cursor_ < history_.size();
}

std::string UndoRedoManager::nextUndoName() const {
    std::scoped_lock lock(mutex_);
    if (cursor_ == 0) {
        return {};
    }
    return std::string(history_[cursor_ - 1]->name());
}

std::string UndoRedoManager::nextRedoName() const {
    std::scoped_lock lock(mutex_);
    if (cursor_ >= history_.size()) {
        return {};
    }
    return std::string(history_[cursor_]->name());
}

std::size_t UndoRedoManager::historyCount() const {
    std::scoped_lock lock(mutex_);
    return history_.size();
}

std::size_t UndoRedoManager::cursorPosition() const {
    std::scoped_lock lock(mutex_);
    return cursor_;
}

std::vector<HistoryEntrySummary> UndoRedoManager::inspectHistory() const {
    std::scoped_lock lock(mutex_);
    std::vector<HistoryEntrySummary> list;
    list.reserve(history_.size());
    for (std::size_t i = 0; i < history_.size(); ++i) {
        list.push_back(HistoryEntrySummary{
            .index = i + 1,
            .name = std::string(history_[i]->name()),
            .isExecuted = (i < cursor_),
            .isSavedMark = ((i + 1) == savedCursor_)
        });
    }
    return list;
}

void UndoRedoManager::clear() {
    std::scoped_lock lock(mutex_);
    history_.clear();
    cursor_ = 0;
    savedCursor_ = 0;
    activeTransaction_.reset();
    transactionNesting_ = 0;
}

// ============================================================================
// LicenseSessionManager
// ============================================================================
LicenseSessionManager::LicenseSessionManager() {
    activateSession(createDefaultStudioProToken());
}

std::uint64_t LicenseSessionManager::computeTokenSignature(
    std::string_view licensee, std::string_view serial,
    LicenseEdition edition, std::uint32_t mask) noexcept {
    // Clean-room 64-bit FNV-1a keyed mixing
    std::uint64_t hash = 14695981039346656037ULL ^ 0xA1F4570D102026ULL;
    auto mixByte = [&hash](std::uint8_t b) noexcept {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 1099511628211ULL;
    };
    for (char c : licensee) {
        mixByte(static_cast<std::uint8_t>(c));
    }
    mixByte(0xFFU);
    for (char c : serial) {
        mixByte(static_cast<std::uint8_t>(c));
    }
    mixByte(static_cast<std::uint8_t>(edition));
    for (int shift = 0; shift < 32; shift += 8) {
        mixByte(static_cast<std::uint8_t>((mask >> shift) & 0xFFU));
    }
    return hash;
}

LicenseToken LicenseSessionManager::createDefaultStudioProToken(std::string licensee) {
    LicenseToken token;
    token.licenseeName = std::move(licensee);
    token.serialNumber = "ASPR-2026-0001-FULL-88F4";
    token.edition = LicenseEdition::StudioPro;
    token.capabilityMask = 0xFFFFFFFFU;
    token.signatureHash = computeTokenSignature(
        token.licenseeName, token.serialNumber, token.edition, token.capabilityMask);
    return token;
}

bool LicenseSessionManager::activateSession(const LicenseToken& token) {
    if (token.licenseeName.empty() || token.serialNumber.empty()) {
        valid_ = false;
        return false;
    }
    const std::uint64_t expected = computeTokenSignature(
        token.licenseeName, token.serialNumber, token.edition, token.capabilityMask);
    if (expected != token.signatureHash) {
        valid_ = false;
        return false;
    }
    token_ = token;
    valid_ = true;
    return true;
}

void LicenseSessionManager::deactivateSession() noexcept {
    valid_ = false;
}

bool LicenseSessionManager::hasCapability(StudioCapability cap) const noexcept {
    if (!valid_) {
        return false;
    }
    return (token_.capabilityMask & static_cast<std::uint32_t>(cap)) != 0U;
}

std::uint32_t LicenseSessionManager::maxPhysicalIOChannels() const noexcept {
    if (!valid_) {
        return 2U;
    }
    switch (token_.edition) {
        case LicenseEdition::Evaluation: return 8U;
        case LicenseEdition::Artist:     return 32U;
        case LicenseEdition::StudioPro:  return 256U;
    }
    return 256U;
}

} // namespace AlphaStudio::Core
