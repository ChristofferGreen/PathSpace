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

UndoableSpace::JournalTransactionGuard::JournalTransactionGuard(
    UndoableSpace& owner,
    std::shared_ptr<UndoJournalRootState> state,
    bool active)
    : owner_(&owner)
    , state_(std::move(state))
    , active_(active) {}

void UndoableSpace::JournalTransactionGuard::markDirty() {
    if (active_ && state_) {
        owner_->markJournalTransactionDirty(*state_);
        dirty_ = true;
    }
}

auto UndoableSpace::JournalTransactionGuard::commit() -> Expected<void> {
    if (!active_ || !owner_ || !state_) {
        active_ = false;
        return {};
    }
    auto result = owner_->commitJournalTransaction(*state_);
    active_ = false;
    state_.reset();
    owner_ = nullptr;
    return result;
}

void UndoableSpace::JournalTransactionGuard::deactivate() {
    active_ = false;
    state_.reset();
    owner_ = nullptr;
}

UndoableSpace::HistoryTransaction::HistoryTransaction(JournalTransactionGuard&& guard)
    : guard_(std::move(guard)) {}

auto UndoableSpace::HistoryTransaction::commit() -> Expected<void> {
    if (!guard_) {
        return {};
    }
    auto result = guard_->commit();
    guard_.reset();
    return result;
}

auto UndoableSpace::beginJournalTransactionInternal(
    std::shared_ptr<UndoJournalRootState> const& state)
    -> Expected<JournalTransactionGuard> {
    if (!state) {
        return std::unexpected(Error{Error::Code::UnknownError, "History root missing"});
    }

    std::unique_lock lock(state->mutex);
    auto const currentThread = std::this_thread::get_id();
    while (state->activeTransaction.has_value()
           && state->activeTransaction->owner != currentThread) {
        state->transactionCv.wait(lock);
    }

    if (state->activeTransaction.has_value()) {
        auto& tx = *state->activeTransaction;
        tx.depth += 1;
    } else {
        state->activeTransaction = UndoJournalRootState::TransactionState{
            .owner          = currentThread,
            .depth          = 1,
            .dirty          = false,
            .pendingEntries = {}
        };
    }

    return JournalTransactionGuard(*this, state, true);
}

void UndoableSpace::markJournalTransactionDirty(UndoJournalRootState& state) {
    std::scoped_lock lock(state.mutex);
    if (state.activeTransaction) {
        state.activeTransaction->dirty = true;
    }
}

auto UndoableSpace::commitJournalTransaction(UndoJournalRootState& state) -> Expected<void> {
    std::unique_lock lock(state.mutex);
    if (!state.activeTransaction) {
        return {};
    }
    auto const currentThread = std::this_thread::get_id();
    auto&       tx           = *state.activeTransaction;
    if (tx.owner != currentThread) {
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "History transaction owned by another thread"});
    }

    if (tx.depth == 0) {
        state.activeTransaction.reset();
        state.transactionCv.notify_all();
        return {};
    }

    tx.depth -= 1;
    if (tx.depth > 0) {
        return {};
    }

    bool dirty = tx.dirty;
    auto pendingEntries = std::move(tx.pendingEntries);
    state.activeTransaction.reset();
    state.transactionCv.notify_all();

    if (!dirty || pendingEntries.empty()) {
        return {};
    }

    JournalOperationScope scope(*this, state, "commit");
    auto beforeStats = state.journal.stats();

    if (state.persistenceEnabled && !state.persistenceWriter) {
        state.persistenceWriter = std::make_unique<UndoJournal::JournalFileWriter>(state.journalPath);
        if (auto open = state.persistenceWriter->open(true); !open) {
            scope.setResult(false, open.error().message.value_or("open_failed"));
            return open;
        }
    }

    auto const monotonicBase = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
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

    auto trimmedEntriesDelta = afterStats.trimmedEntries >= beforeStats.trimmedEntries
                                   ? afterStats.trimmedEntries - beforeStats.trimmedEntries
                                   : std::size_t{0};
    auto trimmedBytesDelta = afterStats.trimmedBytes >= beforeStats.trimmedBytes
                                 ? afterStats.trimmedBytes - beforeStats.trimmedBytes
                                 : std::size_t{0};

    if (trimmedEntriesDelta > 0) {
        state.telemetry.trimOperations += 1;
        state.telemetry.trimmedEntries += trimmedEntriesDelta;
        state.telemetry.trimmedBytes += trimmedBytesDelta;
        state.telemetry.lastTrimTimestamp = std::chrono::system_clock::now();
    }

    if (state.persistenceEnabled) {
        if (trimmedEntriesDelta > 0) {
            auto compact = compactJournalPersistence(state, !state.options.manualGarbageCollect);
            if (!compact) {
                scope.setResult(false, compact.error().message.value_or("compact_failed"));
                return compact;
            }
        }
        updateJournalDiskTelemetry(state);
    }

    state.stateDirty       = true;
    if (state.persistenceEnabled) {
        state.persistenceDirty           = state.options.manualGarbageCollect;
        state.telemetry.persistenceDirty = state.persistenceDirty;
    }

    scope.setResult(true);
    return {};
}

void UndoableSpace::recordJournalOperation(UndoJournalRootState& state,
                                           std::string_view type,
                                           std::chrono::steady_clock::duration duration,
                                           bool success,
                                           UndoJournal::JournalState::Stats const& beforeStats,
                                           std::size_t beforeLiveBytes,
                                           HistoryTelemetry const& beforeTelemetry,
                                           HistoryTelemetry const& afterTelemetry,
                                           std::string message) {
    auto afterStats = state.journal.stats();

    struct ByteTotals {
        std::size_t undo = 0;
        std::size_t redo = 0;
        std::size_t live = 0;
    };

    ByteTotals beforeTotals{
        .undo = beforeStats.undoBytes,
        .redo = beforeStats.redoBytes,
        .live = beforeLiveBytes,
    };
    ByteTotals afterTotals{
        .undo = afterStats.undoBytes,
        .redo = afterStats.redoBytes,
        .live = state.liveBytes,
    };

    HistoryOperationRecord record;
    record.type            = std::string(type);
    record.timestamp       = std::chrono::system_clock::now();
    record.duration        = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    record.success         = success;
    record.undoCountBefore = beforeStats.undoCount;
    record.undoCountAfter  = afterStats.undoCount;
    record.redoCountBefore = beforeStats.redoCount;
    record.redoCountAfter  = afterStats.redoCount;
    record.bytesBefore     = beforeTotals.undo + beforeTotals.redo + beforeTotals.live;
    record.bytesAfter      = afterTotals.undo + afterTotals.redo + afterTotals.live;
    record.message         = std::move(message);
    state.telemetry.lastOperation = std::move(record);

    state.telemetry.undoBytes = afterStats.undoBytes;
    state.telemetry.redoBytes = afterStats.redoBytes;
    state.telemetry.cachedUndo = afterStats.undoCount;
    state.telemetry.cachedRedo = afterStats.redoCount;
    state.telemetry.trimmedEntries = afterStats.trimmedEntries;
    state.telemetry.trimmedBytes   = afterStats.trimmedBytes;
}

auto UndoableSpace::recordJournalMutation(UndoJournalRootState& state,
                                          UndoJournal::OperationKind operation,
                                          std::string_view fullPath,
                                          std::optional<NodeData> const& valueAfter,
                                          std::optional<NodeData> const& inverseValue,
                                          bool barrier) -> Expected<void> {
    auto encodePayload = [&](std::optional<NodeData> const& node)
        -> Expected<UndoJournal::SerializedPayload> {
        if (!node.has_value()) {
            UndoJournal::SerializedPayload payload;
            payload.present = false;
            return payload;
        }
        return UndoJournal::encodeNodeDataPayload(node.value());
    };

    auto valuePayloadExpected = encodePayload(valueAfter);
    if (!valuePayloadExpected) {
        auto reason = valuePayloadExpected.error().message.value_or(
            "Unable to encode journal value payload");
        recordJournalUnsupportedPayload(state, std::string(fullPath), reason);
        return std::unexpected(valuePayloadExpected.error());
    }

    auto inversePayloadExpected = encodePayload(inverseValue);
    if (!inversePayloadExpected) {
        auto reason = inversePayloadExpected.error().message.value_or(
            "Unable to encode journal inverse payload");
        recordJournalUnsupportedPayload(state, std::string(fullPath), reason);
        return std::unexpected(inversePayloadExpected.error());
    }

    UndoJournal::JournalEntry entry;
    entry.operation   = operation;
    entry.path        = std::string(fullPath);
    entry.timestampMs = UndoUtilsAlias::toMillis(std::chrono::system_clock::now());
    entry.monotonicNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    entry.barrier     = barrier;
    entry.value        = std::move(valuePayloadExpected.value());
    entry.inverseValue = std::move(inversePayloadExpected.value());

    auto beforeBytes = payloadBytes(inverseValue);
    auto afterBytes  = payloadBytes(valueAfter);

    std::scoped_lock lock(state.mutex);
    if (!state.activeTransaction) {
        return {};
    }

    adjustLiveBytes(state.liveBytes, beforeBytes, afterBytes);
    state.activeTransaction->pendingEntries.push_back(std::move(entry));
    state.activeTransaction->dirty = true;
    return {};
}

} // namespace SP::History
