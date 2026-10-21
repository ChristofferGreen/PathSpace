#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.retention");

TEST_CASE("journal telemetry paths expose stats") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", 42).errors.empty());
    REQUIRE(space->insert("/doc/value", 43).errors.empty());

    auto statsExpected = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsExpected.has_value());
    auto const& stats = *statsExpected;
    CHECK(stats.counts.undo >= 2);
    CHECK(stats.counts.redo == 0);
    CHECK_FALSE(stats.counts.manualGarbageCollect);

    auto manualGc = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualGc.has_value());
    CHECK_FALSE(*manualGc);

    auto undoCount = space->read<std::size_t>("/doc/_history/stats/undoCount");
    REQUIRE(undoCount.has_value());
    CHECK(*undoCount >= 2);

    auto redoCount = space->read<std::size_t>("/doc/_history/stats/redoCount");
    REQUIRE(redoCount.has_value());
    CHECK(*redoCount == 0);

    auto headGeneration = space->read<std::size_t>("/doc/_history/head/generation");
    REQUIRE(headGeneration.has_value());
    CHECK(*headGeneration >= 2);
}

TEST_CASE("journal manual garbage collect trims entries when invoked") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.maxEntries           = 1;
    opts.manualGarbageCollect = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", 1).errors.empty());
    REQUIRE(space->insert("/doc/value", 2).errors.empty());

    auto gc = space->insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    auto statsAfterGc = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterGc.has_value());
    CHECK(statsAfterGc->counts.undo == 1);

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto secondUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(secondUndo.has_value());
    CHECK(secondUndo.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("manual garbage collect updates trim telemetry paths") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.maxEntries           = 1;
    opts.manualGarbageCollect = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", 1).errors.empty());
    REQUIRE(space->insert("/doc/value", 2).errors.empty());
    REQUIRE(space->insert("/doc/value", 3).errors.empty());

    auto gc = space->insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->trim.operationCount >= 1);
    CHECK(stats->trim.entries > 0);
    CHECK(stats->trim.bytes > 0);

    auto trimmedEntries = space->read<std::size_t>("/doc/_history/stats/trimmedEntries");
    REQUIRE(trimmedEntries.has_value());
    CHECK(*trimmedEntries == stats->trim.entries);

    auto trimmedBytes = space->read<std::size_t>("/doc/_history/stats/trimmedBytes");
    REQUIRE(trimmedBytes.has_value());
    CHECK(*trimmedBytes == stats->trim.bytes);
}

TEST_CASE("journal history commands toggle manual garbage collect mode") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.maxEntries         = 1;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto manualBefore = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualBefore.has_value());
    CHECK_FALSE(*manualBefore);

    auto enableManual = space->insert("/doc/_history/set_manual_garbage_collect", true);
    CHECK(enableManual.errors.empty());

    auto manualAfterEnable = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualAfterEnable.has_value());
    CHECK(*manualAfterEnable);

    REQUIRE(space->insert("/doc/value", std::string{"one"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"two"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"three"}).errors.empty());
    auto statsBeforeGc = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsBeforeGc.has_value());
    CHECK(statsBeforeGc->counts.undo == 3);

    auto gc = space->insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    auto statsAfterGc = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterGc.has_value());
    CHECK(statsAfterGc->counts.undo == 1);
    CHECK(statsAfterGc->trim.operationCount >= 1);

    auto manualStillEnabled = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualStillEnabled.has_value());
    CHECK(*manualStillEnabled);

    auto disableManual = space->insert("/doc/_history/set_manual_garbage_collect", false);
    CHECK(disableManual.errors.empty());

    auto manualAfterDisable = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualAfterDisable.has_value());
    CHECK_FALSE(*manualAfterDisable);
}

TEST_CASE("journal manual garbage collect defers retention until triggered") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.maxEntries           = 1;
    opts.manualGarbageCollect = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", 1).errors.empty());
    REQUIRE(space->insert("/doc/value", 2).errors.empty());

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto thirdUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(thirdUndo.has_value());
    CHECK(thirdUndo.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("transaction batching produces single history entry") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    REQUIRE(space->enableHistory(ConcretePathStringView{"/items"}).has_value());

    {
        auto txExpected = space->beginTransaction(ConcretePathStringView{"/items"});
        REQUIRE(txExpected.has_value());
        auto tx = std::move(txExpected.value());

        auto firstInsert = space->insert("/items/a", 1);
        REQUIRE(firstInsert.errors.empty());
        auto secondInsert = space->insert("/items/b", 2);
        REQUIRE(secondInsert.errors.empty());

        REQUIRE(tx.commit().has_value());
    }

    auto stats = space->getHistoryStats(ConcretePathStringView{"/items"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 2);
    CHECK(stats->counts.redo == 0);
    CHECK(stats->trim.operationCount == 0);

    REQUIRE(space->undo(ConcretePathStringView{"/items"}, 2).has_value());
    CHECK_FALSE(space->read<int>("/items/a").has_value());
    CHECK_FALSE(space->read<int>("/items/b").has_value());
}

TEST_CASE("journal beginTransaction batches mutations") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/items"}, opts).has_value());

    auto txExpected = space->beginTransaction(ConcretePathStringView{"/items"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto firstInsert = space->insert("/items/a", 1);
    REQUIRE(firstInsert.errors.empty());
    auto secondInsert = space->insert("/items/b", 2);
    REQUIRE(secondInsert.errors.empty());

    REQUIRE(tx.commit().has_value());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/items"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 2);
    CHECK(stats->counts.redo == 0);

    REQUIRE(space->undo(ConcretePathStringView{"/items"}, 2).has_value());
    CHECK_FALSE(space->read<int>("/items/a").has_value());
    CHECK_FALSE(space->read<int>("/items/b").has_value());
}

TEST_CASE("retention trims oldest entries when exceeding maxEntries") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.maxEntries = 2;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", std::string{"one"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"two"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"three"}).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CAPTURE(stats->counts.undo);
    CAPTURE(stats->counts.manualGarbageCollect);
    CHECK(stats->counts.undo >= 1);
    CHECK(stats->trim.operationCount >= 1);

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto thirdUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(thirdUndo.has_value());
    CHECK(thirdUndo.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("retention honors maxBytesRetained budget") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.maxEntries        = 8;
    opts.maxBytesRetained  = 1500;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    std::string blobA(1024, 'a');
    std::string blobB(1024, 'b');

    REQUIRE(space->insert("/doc/value", blobA).errors.empty());
    auto statsAfterFirst = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterFirst.has_value());
    CHECK(statsAfterFirst->counts.undo == 1);
    auto trimsBefore = statsAfterFirst->trim.operationCount;

    REQUIRE(space->insert("/doc/value", blobB).errors.empty());
    auto statsAfterSecond = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterSecond.has_value());
    CHECK(statsAfterSecond->counts.undo <= 1);
    CHECK(statsAfterSecond->trim.operationCount >= trimsBefore + 1);
}

TEST_CASE("journal retention trims oldest entries when exceeding maxEntries") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.maxEntries         = 2;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", std::string{"one"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"two"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"three"}).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 2);
    CHECK(stats->trim.operationCount >= 1);

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto mid = space->read<std::string>("/doc/value");
    REQUIRE(mid.has_value());
    CHECK(*mid == std::string{"one"});

    auto statsAfterFirstUndo = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterFirstUndo.has_value());
    CHECK(statsAfterFirstUndo->counts.undo == 1);

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto first = space->read<std::string>("/doc/value");
    REQUIRE(first.has_value());
    CHECK(*first == std::string{"one"});

    auto statsAfterSecondUndo = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterSecondUndo.has_value());
    CHECK(statsAfterSecondUndo->counts.undo == 0);

    auto thirdUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(thirdUndo.has_value());
    CHECK(thirdUndo.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("manual garbage collect defers retention until invoked") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.maxEntries           = 1;
    opts.manualGarbageCollect = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", 1).errors.empty());
    REQUIRE(space->insert("/doc/value", 2).errors.empty());

    auto statsBefore = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsBefore.has_value());
    CHECK(statsBefore->counts.undo == 2);

    auto gc = space->insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    auto statsAfter = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfter.has_value());
    CAPTURE(statsAfter->counts.undo);
    CAPTURE(statsAfter->counts.manualGarbageCollect);
    CHECK(statsAfter->counts.undo == 1);
    CHECK(statsAfter->trim.operationCount >= statsBefore->trim.operationCount + 1);
    CHECK(statsAfter->trim.entries >= statsBefore->trim.entries + 1);
}

TEST_SUITE_END();
