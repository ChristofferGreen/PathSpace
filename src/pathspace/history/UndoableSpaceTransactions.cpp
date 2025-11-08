#include "history/UndoableSpace.hpp"

#include "history/UndoHistoryUtils.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "history/UndoableSpaceState.hpp"
#include "log/TaggedLogger.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace SP::History {

using SP::Error;
using SP::Expected;
namespace UndoUtilsAlias = SP::History::UndoUtils;

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

auto UndoableSpace::commitJournalAndDeactivate(UndoableSpace* owner,
                                               std::shared_ptr<UndoJournalRootState>& state,
                                               bool& active) -> Expected<void> {
    if (!active || !owner || !state) {
        active = false;
        return {};
    }
    active = false;
    return owner->commitJournalTransaction(*state);
}

void UndoableSpace::commitJournalOnScopeExit(UndoableSpace* owner,
                                             std::shared_ptr<UndoJournalRootState>& state,
                                             bool& active,
                                             std::string_view context) {
    if (!active || !owner || !state) {
        active = false;
        return;
    }
    auto const result = owner->commitJournalTransaction(*state);
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

UndoableSpace::JournalOperationScope::JournalOperationScope(UndoableSpace& owner,
                                                            UndoJournalRootState& state,
                                                            std::string_view type)
    : owner(owner)
    , state(state)
    , type(type)
    , startSteady(std::chrono::steady_clock::now())
    , beforeStats(state.journal.stats()) {}

void UndoableSpace::JournalOperationScope::setResult(bool success, std::string message) {
    succeeded   = success;
    messageText = std::move(message);
}

UndoableSpace::JournalOperationScope::~JournalOperationScope() {
    owner.recordJournalOperation(state,
                                 type,
                                 std::chrono::steady_clock::now() - startSteady,
                                 succeeded,
                                 beforeStats,
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

UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::JournalTransactionHandleBase(
    UndoableSpace& owner,
    std::shared_ptr<UndoJournalRootState> state,
    bool active,
    std::string_view context)
    : owner_(&owner)
    , state_(std::move(state))
    , active_(active)
    , context_(context) {}

UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::JournalTransactionHandleBase(
    JournalTransactionHandleBase&& other) noexcept
    : owner_(other.owner_)
    , state_(std::move(other.state_))
    , active_(other.active_)
    , context_(std::move(other.context_)) {
    other.owner_  = nullptr;
    other.active_ = false;
    other.context_.clear();
}

auto UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::operator=(
    JournalTransactionHandleBase&& other) noexcept -> JournalTransactionHandleBase& {
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

UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::~JournalTransactionHandleBase() {
    finalizeHandle();
}

auto UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::commitHandle()
    -> Expected<void> {
    if (!owner_ || !state_) {
        active_ = false;
        return {};
    }
    return commitJournalAndDeactivate(owner_, state_, active_);
}

void UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::deactivateHandle() {
    active_ = false;
}

void UndoableSpace::JournalTransactionGuard::JournalTransactionHandleBase::finalizeHandle() {
    if (!owner_ || !state_) {
        active_ = false;
        return;
    }
    commitJournalOnScopeExit(owner_, state_, active_, context_);
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

UndoableSpace::JournalTransactionGuard::JournalTransactionGuard(
    UndoableSpace& owner,
    std::shared_ptr<UndoJournalRootState> state,
    bool active)
    : handle_(owner,
              std::move(state),
              active,
              "UndoableSpace::JournalTransactionGuard commit failed during destruction: ") {}

void UndoableSpace::JournalTransactionGuard::markDirty() {
    if (!handle_.isHandleActive())
        return;
    auto* ownerPtr = handle_.ownerHandle();
    auto& statePtr = handle_.stateHandle();
    if (!ownerPtr || !statePtr)
        return;
    ownerPtr->markJournalTransactionDirty(*statePtr);
}

auto UndoableSpace::JournalTransactionGuard::commit() -> Expected<void> {
    return handle_.commitHandle();
}

void UndoableSpace::JournalTransactionGuard::deactivate() {
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

auto UndoableSpace::beginJournalTransactionInternal(
    std::shared_ptr<UndoJournalRootState> const& state)
    -> Expected<JournalTransactionGuard> {
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
        state->activeTransaction = UndoJournalRootState::TransactionState{
            .owner          = currentThread,
            .depth          = 1,
            .dirty          = false,
            .pendingEntries = {}};
    }
    return JournalTransactionGuard(*this, state, true);
}

void UndoableSpace::recordJournalOperation(UndoJournalRootState& state,
                                           std::string_view type,
                                           std::chrono::steady_clock::duration duration,
                                           bool success,
                                           UndoJournal::JournalState::Stats const& beforeStats,
                                           std::string const& message) {
    auto afterStats = state.journal.stats();

    RootState::OperationRecord record;
    record.type            = std::string(type);
    record.timestamp       = std::chrono::system_clock::now();
    record.duration        = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    record.success         = success;
    record.undoCountBefore = beforeStats.undoCount;
   record.undoCountAfter  = afterStats.undoCount;
   record.redoCountBefore = beforeStats.redoCount;
   record.redoCountAfter  = afterStats.redoCount;
    record.bytesBefore     = beforeStats.undoBytes + beforeStats.redoBytes;
    record.bytesAfter      = afterStats.undoBytes + afterStats.redoBytes;
    record.message         = message;
    state.telemetry.lastOperation = std::move(record);

    state.telemetry.undoBytes        = afterStats.undoBytes;
    state.telemetry.redoBytes        = afterStats.redoBytes;
    state.telemetry.trimmedEntries   = afterStats.trimmedEntries;
    state.telemetry.trimmedBytes     = afterStats.trimmedBytes;
    state.telemetry.cachedUndo       = afterStats.undoCount;
    state.telemetry.cachedRedo       = afterStats.redoCount;
    state.telemetry.persistenceDirty = state.persistenceDirty;
}

void UndoableSpace::markJournalTransactionDirty(UndoJournalRootState& state) {
    std::scoped_lock lock(state.mutex);
    if (state.activeTransaction)
        state.activeTransaction->dirty = true;
}

auto UndoableSpace::commitJournalTransaction(UndoJournalRootState& state) -> Expected<void> {
    std::unique_lock lock(state.mutex);
    if (!state.activeTransaction)
        return {};
    auto const currentThread = std::this_thread::get_id();
    auto& tx = *state.activeTransaction;
    if (tx.owner != currentThread) {
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "History transaction owned by another thread"});
    }

    if (tx.depth == 0) {
        state.activeTransaction.reset();
        return {};
    }

    tx.depth -= 1;
    if (tx.depth > 0) {
        return {};
    }

    bool dirty = tx.dirty;
    auto pendingEntries = std::move(tx.pendingEntries);
    state.activeTransaction.reset();

    if (!dirty || pendingEntries.empty())
        return {};

    JournalOperationScope scope(*this, state, "commit");
    auto beforeStats = state.journal.stats();

    if (state.persistenceEnabled && !state.persistenceWriter) {
        state.persistenceWriter =
            std::make_unique<UndoJournal::JournalFileWriter>(state.journalPath);
        if (auto open = state.persistenceWriter->open(true); !open) {
            scope.setResult(false, open.error().message.value_or("open_failed"));
            return open;
        }
    }

    auto const monotonicBase =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());

    bool flushOnCommit = !state.options.manualGarbageCollect;
    for (std::size_t i = 0; i < pendingEntries.size(); ++i) {
        auto& entry = pendingEntries[i];
        if (entry.timestampMs == 0) {
            entry.timestampMs = UndoUtilsAlias::toMillis(std::chrono::system_clock::now());
        }
        if (entry.monotonicNs == 0) {
            entry.monotonicNs = monotonicBase + static_cast<std::uint64_t>(i);
        }
        entry.sequence = state.nextSequence++;
        auto enforceRetention = !state.options.manualGarbageCollect;
        state.journal.append(entry, enforceRetention);
        if (state.persistenceEnabled) {
            bool fsyncThisEntry = flushOnCommit && (i + 1 == pendingEntries.size());
            if (auto append = state.persistenceWriter->append(entry, fsyncThisEntry); !append) {
                scope.setResult(false, append.error().message.value_or("append_failed"));
                return append;
            }
        }
    }

    auto afterStats = state.journal.stats();
    state.telemetry.cachedUndo = afterStats.undoCount;
    state.telemetry.cachedRedo = afterStats.redoCount;
    state.telemetry.undoBytes  = afterStats.undoBytes;
    state.telemetry.redoBytes  = afterStats.redoBytes;

    auto trimmedEntriesDelta =
        afterStats.trimmedEntries >= beforeStats.trimmedEntries
            ? afterStats.trimmedEntries - beforeStats.trimmedEntries
            : std::size_t{0};
    auto trimmedBytesDelta =
        afterStats.trimmedBytes >= beforeStats.trimmedBytes
            ? afterStats.trimmedBytes - beforeStats.trimmedBytes
            : std::size_t{0};
    std::string resultMessage;
    if (trimmedEntriesDelta > 0) {
        state.telemetry.trimOperations += 1;
        state.telemetry.trimmedEntries += trimmedEntriesDelta;
        state.telemetry.trimmedBytes += trimmedBytesDelta;
        state.telemetry.lastTrimTimestamp = std::chrono::system_clock::now();
        resultMessage = "trimmed=" + std::to_string(trimmedEntriesDelta);
    }

    if (state.persistenceEnabled) {
        if (trimmedEntriesDelta > 0) {
            auto compact = compactJournalPersistence(
                state, !state.options.manualGarbageCollect);
            if (!compact)
                return compact;
        }
        updateJournalDiskTelemetry(state);
    }

    state.stateDirty       = true;
    if (state.persistenceEnabled) {
        state.persistenceDirty           = state.options.manualGarbageCollect;
        state.telemetry.persistenceDirty = state.persistenceDirty;
    }
    scope.setResult(true, resultMessage);
    return {};
}

auto UndoableSpace::recordJournalMutation(UndoJournalRootState& state,
                                          UndoJournal::OperationKind operation,
                                          std::string_view fullPath,
                                          std::optional<NodeData> const& valueAfter,
                                          std::optional<NodeData> const& inverseValue,
                                          bool barrier) -> Expected<void> {
    auto encodePayload = [&](std::optional<NodeData> const& node) -> Expected<UndoJournal::SerializedPayload> {
        if (!node.has_value()) {
            UndoJournal::SerializedPayload payload;
            payload.present = false;
            return payload;
        }
        return UndoJournal::encodeNodeDataPayload(node.value());
    };

    auto valuePayloadExpected = encodePayload(valueAfter);
    if (!valuePayloadExpected) {
        auto reason = valuePayloadExpected.error().message.value_or("Unable to encode journal value payload");
        recordJournalUnsupportedPayload(state, std::string(fullPath), reason);
        return std::unexpected(valuePayloadExpected.error());
    }

    auto inversePayloadExpected = encodePayload(inverseValue);
    if (!inversePayloadExpected) {
        auto reason =
            inversePayloadExpected.error().message.value_or("Unable to encode journal inverse payload");
        recordJournalUnsupportedPayload(state, std::string(fullPath), reason);
        return std::unexpected(inversePayloadExpected.error());
    }

    UndoJournal::JournalEntry entry;
    entry.operation = operation;
    entry.path      = std::string(fullPath);
    entry.timestampMs = UndoUtilsAlias::toMillis(std::chrono::system_clock::now());
    entry.monotonicNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    entry.barrier = barrier;
    entry.value        = std::move(valuePayloadExpected.value());
    entry.inverseValue = std::move(inversePayloadExpected.value());

    std::scoped_lock lock(state.mutex);
    if (!state.activeTransaction)
        return {};
    state.activeTransaction->pendingEntries.push_back(std::move(entry));
    state.activeTransaction->dirty = true;
    return {};
}

} // namespace SP::History
