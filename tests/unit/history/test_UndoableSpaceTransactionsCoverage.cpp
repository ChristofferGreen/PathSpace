#define private public
#include "history/UndoableSpace.hpp"
#undef private

#include "PathSpace.hpp"
#include "core/NodeData.hpp"
#include "third_party/doctest.h"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace SP;
using namespace SP::History;

TEST_SUITE_BEGIN("history.undoable.transactions.coverage");

TEST_CASE("beginJournalTransactionInternal rejects missing state") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    auto result = space.beginJournalTransactionInternal(nullptr);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("commitJournalTransaction handles empty and mismatched ownership") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;

    auto ok = space.commitJournalTransaction(state);
    CHECK(ok.has_value());

    state.activeTransaction = UndoJournalRootState::TransactionState{
        .owner          = std::thread::id{},
        .depth          = 1,
        .dirty          = false,
        .pendingEntries = {},
    };
    auto mismatch = space.commitJournalTransaction(state);
    CHECK_FALSE(mismatch.has_value());
    CHECK(mismatch.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("recordJournalMutation ignores missing transactions and logs unsupported payloads") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    InputMetadata meta{InputMetadataT<int>{}};
    int           value = 5;
    NodeData      after;
    NodeData      before;
    REQUIRE_FALSE(after.serialize(InputData{&value, meta}).has_value());
    REQUIRE_FALSE(before.serialize(InputData{&value, meta}).has_value());

    UndoJournalRootState noTx;
    auto ok = space.recordJournalMutation(noTx,
                                          UndoJournal::OperationKind::Insert,
                                          "/doc/value",
                                          after,
                                          before,
                                          false);
    CHECK(ok.has_value());
    CHECK(noTx.telemetry.unsupportedTotal == 0);

    UndoJournalRootState unsupportedState;
    std::optional<NodeData> emptyPayload{NodeData{}};
    auto bad = space.recordJournalMutation(unsupportedState,
                                           UndoJournal::OperationKind::Insert,
                                           "/doc/empty",
                                           emptyPayload,
                                           emptyPayload,
                                           false);
    CHECK_FALSE(bad.has_value());
    CHECK(unsupportedState.telemetry.unsupportedTotal == 1);
    REQUIRE(unsupportedState.telemetry.unsupportedLog.size() == 1);
    CHECK(unsupportedState.telemetry.unsupportedLog.front().path == "/doc/empty");
    CHECK(unsupportedState.telemetry.unsupportedLog.front().reason.find("Unable to serialize")
          != std::string::npos);
}

TEST_CASE("HistoryTransaction commit is a no-op without a guard") {
    UndoableSpace::HistoryTransaction tx;
    auto result = tx.commit();
    CHECK(result.has_value());
    CHECK_FALSE(static_cast<bool>(tx));
}

TEST_CASE("JournalTransactionGuard commit clears active state when empty") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    auto state = std::make_shared<UndoJournalRootState>();

    auto guardExpected = space.beginJournalTransactionInternal(state);
    REQUIRE(guardExpected.has_value());
    auto guard = std::move(guardExpected.value());
    CHECK(state->activeTransaction.has_value());

    guard.markDirty();
    auto committed = guard.commit();
    CHECK(committed.has_value());
    CHECK_FALSE(state->activeTransaction.has_value());
}

TEST_CASE("recordJournalOperation updates telemetry and last operation fields") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.liveBytes = 7;

    UndoJournal::JournalEntry entry;
    entry.path = "/doc";
    state.journal.append(entry);

    auto beforeStats = state.journal.stats();
    beforeStats.undoCount = 2;
    beforeStats.redoCount = 1;
    beforeStats.undoBytes = 4;
    beforeStats.redoBytes = 3;

    HistoryTelemetry beforeTelemetry = state.telemetry;
    HistoryTelemetry afterTelemetry  = state.telemetry;

    space.recordJournalOperation(state,
                                 "test",
                                 std::chrono::milliseconds(5),
                                 false,
                                 beforeStats,
                                 9,
                                 beforeTelemetry,
                                 afterTelemetry,
                                 "tag",
                                 "message");

    REQUIRE(state.telemetry.lastOperation.has_value());
    auto const& op = *state.telemetry.lastOperation;
    CHECK(op.type == "test");
    CHECK(op.tag == "tag");
    CHECK(op.message == "message");
    CHECK(op.undoCountBefore == 2);
    CHECK(op.redoCountBefore == 1);
    CHECK(op.undoCountAfter == state.journal.stats().undoCount);
    CHECK(op.redoCountAfter == state.journal.stats().redoCount);
    CHECK(op.bytesBefore == (beforeStats.undoBytes + beforeStats.redoBytes + 9));
    CHECK(op.bytesAfter
          == (state.journal.stats().undoBytes + state.journal.stats().redoBytes + state.liveBytes));
    CHECK(state.telemetry.cachedUndo == state.journal.stats().undoCount);
    CHECK(state.telemetry.cachedRedo == state.journal.stats().redoCount);
}

TEST_SUITE_END();
