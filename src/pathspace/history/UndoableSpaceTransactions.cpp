#include "history/UndoableSpace.hpp"

#include "history/UndoableSpaceState.hpp"
#include "log/TaggedLogger.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace SP::History {

using SP::Error;
using SP::Expected;

auto UndoableSpace::commitAndDeactivate(UndoableSpace* owner,
                                        std::shared_ptr<RootState>& state,
                                        bool& active) -> Expected<void> {
    if (!active || !owner || !state) {
        active = false;
        return {};
    }
    active = false;
    return owner->commitTransaction(*state);
}

void UndoableSpace::commitOnScopeExit(UndoableSpace* owner,
                                      std::shared_ptr<RootState>& state,
                                      bool& active,
                                      std::string_view context) {
    if (!active || !owner || !state) {
        active = false;
        return;
    }
    auto const result = owner->commitTransaction(*state);
    if (!result) {
        std::string message(context);
        message.append(result.error().message.value_or("unknown"));
        sp_log(message, "UndoableSpace");
    }
    active = false;
}

UndoableSpace::OperationScope::OperationScope(UndoableSpace& owner,
                                              RootState& state,
                                              std::string_view type)
    : owner(owner)
    , state(state)
    , type(type)
    , startSteady(std::chrono::steady_clock::now())
    , undoBefore(state.undoStack.size())
    , redoBefore(state.redoStack.size())
    , bytesBefore(UndoableSpace::computeTotalBytesLocked(state)) {}

void UndoableSpace::OperationScope::setResult(bool success, std::string message) {
    succeeded   = success;
    messageText = std::move(message);
}

UndoableSpace::OperationScope::~OperationScope() {
    owner.recordOperation(state,
                          type,
                          std::chrono::steady_clock::now() - startSteady,
                          succeeded,
                          undoBefore,
                          redoBefore,
                          bytesBefore,
                          messageText);
}

UndoableSpace::TransactionHandleBase::TransactionHandleBase(
    UndoableSpace& owner,
    std::shared_ptr<RootState> state,
    bool active,
    std::string_view context)
    : owner_(&owner)
    , state_(std::move(state))
    , active_(active)
    , context_(context) {}

UndoableSpace::TransactionHandleBase::TransactionHandleBase(TransactionHandleBase&& other) noexcept
    : owner_(other.owner_)
    , state_(std::move(other.state_))
    , active_(other.active_)
    , context_(std::move(other.context_)) {
    other.owner_  = nullptr;
    other.active_ = false;
    other.context_.clear();
}

auto UndoableSpace::TransactionHandleBase::operator=(TransactionHandleBase&& other) noexcept
    -> TransactionHandleBase& {
    if (this == &other)
        return *this;
    finalizeHandle();
    owner_   = other.owner_;
    state_   = std::move(other.state_);
    active_  = other.active_;
    context_ = std::move(other.context_);
    other.owner_  = nullptr;
    other.active_ = false;
    other.context_.clear();
    return *this;
}

UndoableSpace::TransactionHandleBase::~TransactionHandleBase() {
    finalizeHandle();
}

auto UndoableSpace::TransactionHandleBase::commitHandle() -> Expected<void> {
    if (!owner_ || !state_) {
        active_ = false;
        return {};
    }
    return commitAndDeactivate(owner_, state_, active_);
}

void UndoableSpace::TransactionHandleBase::deactivateHandle() {
    active_ = false;
}

void UndoableSpace::TransactionHandleBase::finalizeHandle() {
    if (!owner_ || !state_) {
        active_ = false;
        return;
    }
    commitOnScopeExit(owner_, state_, active_, context_);
    owner_  = nullptr;
    state_.reset();
    active_ = false;
}

UndoableSpace::TransactionGuard::TransactionGuard(UndoableSpace& owner,
                                                  std::shared_ptr<RootState> state,
                                                  bool active)
    : handle_(owner,
              std::move(state),
              active,
              "UndoableSpace::TransactionGuard commit failed during destruction: ") {}

void UndoableSpace::TransactionGuard::markDirty() {
    if (!handle_.isHandleActive())
        return;
    auto* ownerPtr = handle_.ownerHandle();
    auto& statePtr = handle_.stateHandle();
    if (!ownerPtr || !statePtr)
        return;
    ownerPtr->markTransactionDirty(*statePtr);
}

auto UndoableSpace::TransactionGuard::commit() -> Expected<void> {
    return handle_.commitHandle();
}

void UndoableSpace::TransactionGuard::deactivate() {
    handle_.deactivateHandle();
}

UndoableSpace::HistoryTransaction::HistoryTransaction(UndoableSpace& owner,
                                                      std::shared_ptr<RootState> state)
    : handle_(owner,
              std::move(state),
              true,
              "UndoableSpace::HistoryTransaction auto-commit failed: ") {}

UndoableSpace::HistoryTransaction::HistoryTransaction(HistoryTransaction&& other) noexcept = default;
auto UndoableSpace::HistoryTransaction::operator=(HistoryTransaction&& other) noexcept
    -> HistoryTransaction& = default;

auto UndoableSpace::HistoryTransaction::commit() -> Expected<void> {
    return handle_.commitHandle();
}

auto UndoableSpace::beginTransactionInternal(std::shared_ptr<RootState> const& state)
    -> Expected<TransactionGuard> {
    if (!state)
        return std::unexpected(Error{Error::Code::UnknownError, "History root missing"});

    std::unique_lock lock(state->mutex);
    auto const currentThread = std::this_thread::get_id();
    if (state->activeTransaction.has_value()) {
        auto& tx = *state->activeTransaction;
        if (tx.owner != currentThread) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "History transaction already active on another thread"});
        }
        tx.depth += 1;
    } else {
        state->activeTransaction = RootState::TransactionState{
            .owner          = currentThread,
            .depth          = 1,
            .dirty          = false,
            .snapshotBefore = state->liveSnapshot};
    }
    return TransactionGuard(*this, state, true);
}

void UndoableSpace::markTransactionDirty(RootState& state) {
    std::scoped_lock lock(state.mutex);
    if (state.activeTransaction)
        state.activeTransaction->dirty = true;
}

auto UndoableSpace::commitTransaction(RootState& state) -> Expected<void> {
    std::unique_lock lock(state.mutex);
    if (!state.activeTransaction) {
        return {};
    }
    auto currentThread = std::this_thread::get_id();
    if (state.activeTransaction->owner != currentThread) {
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "History transaction owned by another thread"});
    }

    auto before = state.activeTransaction->snapshotBefore;
    bool dirty  = state.activeTransaction->dirty;
    auto depth  = state.activeTransaction->depth;

    if (depth == 0) {
        state.activeTransaction.reset();
        return {};
    }

    state.activeTransaction->depth -= 1;
    if (state.activeTransaction->depth > 0) {
        return {};
    }

    state.activeTransaction.reset();

    OperationScope scope(*this, state, "commit");

    if (!dirty) {
        scope.setResult(true, "no_changes");
        return {};
    }

    auto snapshotExpected = captureSnapshotLocked(state);
    if (!snapshotExpected) {
        auto revert = applySnapshotLocked(state, before);
        if (!revert) {
            sp_log("UndoableSpace::commitTransaction rollback failed: "
                       + revert.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
        state.liveSnapshot = before;
        auto beforeMetrics  = state.prototype.analyze(state.liveSnapshot);
        state.liveBytes     = beforeMetrics.payloadBytes;
        scope.setResult(false, snapshotExpected.error().message.value_or("capture_failed"));
        return std::unexpected(snapshotExpected.error());
    }

    auto latest     = snapshotExpected.value();
    if (latest.root == before.root) {
        scope.setResult(true, "no_snapshot");
        return {};
    }
    auto now        = std::chrono::system_clock::now();
    auto undoBytes  = state.liveBytes;
    RootState::Entry undoEntry;
    undoEntry.snapshot  = before;
    undoEntry.bytes     = undoBytes;
    undoEntry.timestamp = now;
    undoEntry.persisted = !state.persistenceEnabled;
    undoEntry.cached    = true;
    state.undoStack.push_back(std::move(undoEntry));
    state.telemetry.undoBytes += undoBytes;

    state.liveSnapshot = latest;
    auto latestMetrics = state.prototype.analyze(latest);
    state.liveBytes    = latestMetrics.payloadBytes;
    for (auto const& redoEntry : state.redoStack) {
        if (redoEntry.persisted)
            removeEntryFiles(state, redoEntry.snapshot.generation);
    }
    state.redoStack.clear();
    state.telemetry.redoBytes = 0;
    state.stateDirty          = true;

    TrimStats trimStats{};
    if (!state.options.manualGarbageCollect) {
        trimStats = applyRetentionLocked(state, "commit");
        if (trimStats.entriesRemoved > 0) {
            scope.setResult(true, "trimmed=" + std::to_string(trimStats.entriesRemoved));
        }
    }

    applyRamCachePolicyLocked(state);
    updateCacheTelemetryLocked(state);
    auto persistResult = persistStacksLocked(state, false);
    if (!persistResult)
        return persistResult;

    return {};
}

} // namespace SP::History
