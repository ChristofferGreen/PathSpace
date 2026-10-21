#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.core");

TEST_CASE("undo/redo round trip") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
    REQUIRE(enable.has_value());

    auto insertResult = space->insert("/doc/title", std::string{"alpha"});
    REQUIRE(insertResult.errors.empty());

    auto statsAfterInsert = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterInsert.has_value());
    CHECK(statsAfterInsert->counts.undo == 1);
    CHECK(statsAfterInsert->counts.redo == 0);
    CHECK(statsAfterInsert->bytes.total > 0);
    CHECK_FALSE(statsAfterInsert->counts.manualGarbageCollect);

    auto undoCountPath = space->read<std::size_t>("/doc/_history/stats/undoCount");
    REQUIRE(undoCountPath.has_value());
    CHECK(*undoCountPath == 1);

    auto value = space->read<std::string>("/doc/title");
    REQUIRE(value.has_value());
    CHECK(value->c_str() == std::string{"alpha"});

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto missing = space->read<std::string>("/doc/title");
    CHECK_FALSE(missing.has_value());

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = space->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(restored->c_str() == std::string{"alpha"});

    auto lastOpType = space->read<std::string>("/doc/_history/lastOperation/type");
    REQUIRE(lastOpType.has_value());
    CHECK(*lastOpType == std::string{"redo"});
}

TEST_CASE("history diagnostics mirror stats and entries") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/title", std::string{"alpha"}).errors.empty());

    auto diagUndoCount = space->read<std::size_t>("/diagnostics/history/_doc/stats/undoCount");
    REQUIRE(diagUndoCount.has_value());
    CHECK(*diagUndoCount == 1);

    auto compatUndoCount = space->read<std::size_t>("/output/v1/diagnostics/history/_doc/stats/undoCount");
    REQUIRE(compatUndoCount.has_value());
    CHECK(*compatUndoCount == 1);

    auto headSequence = space->read<std::uint64_t>("/diagnostics/history/_doc/head/sequence");
    REQUIRE(headSequence.has_value());
    CHECK(*headSequence == 0);

    auto entryOperation = space->read<std::string>("/diagnostics/history/_doc/entries/0/operation");
    REQUIRE(entryOperation.has_value());
    CHECK(*entryOperation == std::string{"insert"});

    auto entryPath = space->read<std::string>("/diagnostics/history/_doc/entries/0/path");
    REQUIRE(entryPath.has_value());
    CHECK(*entryPath == std::string{"/doc/title"});

    auto entryHasValue = space->read<bool>("/diagnostics/history/_doc/entries/0/hasValue");
    REQUIRE(entryHasValue.has_value());
    CHECK(*entryHasValue);

    auto entryValueBytes = space->read<std::size_t>("/diagnostics/history/_doc/entries/0/valueBytes");
    REQUIRE(entryValueBytes.has_value());
    CHECK(*entryValueBytes > 0);

    auto missingEntry = space->read<std::string>("/diagnostics/history/_doc/entries/99/path");
    CHECK_FALSE(missingEntry.has_value());
    CHECK(missingEntry.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("journal undo/redo round trip") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/title", std::string{"alpha"}).errors.empty());
    auto value = space->read<std::string>("/doc/title");
    REQUIRE(value.has_value());
    CHECK(*value == std::string{"alpha"});

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    CHECK_FALSE(space->read<std::string>("/doc/title").has_value());

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = space->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(*restored == std::string{"alpha"});
}

TEST_CASE("UndoableSpace bypasses POD fast path and still records history") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    auto root = ConcretePathStringView{"/doc"};
    REQUIRE(space->enableHistory(root).has_value());

    CHECK(space->insert("/doc/value", 7).errors.empty());
    auto stats = space->getHistoryStats(root);
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 1);

    auto spanRead = space->read("/doc/value", [&](std::span<const int>) {});
    CHECK_FALSE(spanRead.has_value());

    auto value = space->read<int>("/doc/value");
    REQUIRE(value.has_value());
    CHECK(*value == 7);

    REQUIRE(space->undo(root).has_value());
    CHECK_FALSE(space->read<int>("/doc/value").has_value());

    REQUIRE(space->redo(root).has_value());
    auto restored = space->read<int>("/doc/value");
    REQUIRE(restored.has_value());
    CHECK(*restored == 7);
}

TEST_CASE("journal take undo restores value") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/queue"}, opts).has_value());

    REQUIRE(space->insert("/queue/item", 42).errors.empty());

    auto taken = space->take<int>("/queue/item");
    REQUIRE(taken.has_value());
    CHECK(*taken == 42);
    CHECK_FALSE(space->read<int>("/queue/item").has_value());

    REQUIRE(space->undo(ConcretePathStringView{"/queue"}).has_value());
    auto restored = space->read<int>("/queue/item");
    REQUIRE(restored.has_value());
    CHECK(*restored == 42);

    REQUIRE(space->redo(ConcretePathStringView{"/queue"}).has_value());
    CHECK_FALSE(space->read<int>("/queue/item").has_value());
}

TEST_CASE("journal history control commands perform undo and redo") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/title", std::string{"alpha"}).errors.empty());

    auto undoCmd = space->insert("/doc/_history/undo", std::size_t{1});
    CHECK(undoCmd.errors.empty());
    CHECK_FALSE(space->read<std::string>("/doc/title").has_value());

    auto redoCmd = space->insert("/doc/_history/redo", std::size_t{1});
    CHECK(redoCmd.errors.empty());
    auto restored = space->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(*restored == std::string{"alpha"});
}

TEST_CASE("history command step payloads honor signed and unsigned integers") {
    SUBCASE("uint64 step count applies multiple undos") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        HistoryOptions opts;
        opts.useMutationJournal = true;
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

        REQUIRE(space->insert("/doc/a", std::string{"alpha"}).errors.empty());
        REQUIRE(space->insert("/doc/b", std::string{"beta"}).errors.empty());

        auto undoTwo = space->insert("/doc/_history/undo", static_cast<std::uint64_t>(2));
        CHECK(undoTwo.errors.empty());
        CHECK_FALSE(space->read<std::string>("/doc/a").has_value());
        CHECK_FALSE(space->read<std::string>("/doc/b").has_value());
    }

    SUBCASE("negative int64 payload defaults to a single step") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        HistoryOptions opts;
        opts.useMutationJournal = true;
        REQUIRE(space->enableHistory(ConcretePathStringView{"/neg"}, opts).has_value());

        REQUIRE(space->insert("/neg/value", std::string{"gamma"}).errors.empty());
        auto undoOne = space->insert("/neg/_history/undo", static_cast<std::int64_t>(-5));
        CHECK(undoOne.errors.empty());
        CHECK_FALSE(space->read<std::string>("/neg/value").has_value());

        REQUIRE(space->redo(ConcretePathStringView{"/neg"}).has_value());
        auto restored = space->read<std::string>("/neg/value");
        REQUIRE(restored.has_value());
        CHECK(*restored == std::string{"gamma"});
    }
}

TEST_CASE("history tag command annotates last operation and journal steps") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto tagSet = space->insert("/doc/_history/set_tag", std::string{"paint_stroke"});
    CHECK(tagSet.errors.empty());

    REQUIRE(space->insert("/doc/value", 7).errors.empty());

    auto tagValue = space->read<std::string>("/doc/_history/lastOperation/tag");
    REQUIRE(tagValue.has_value());
    CHECK(*tagValue == std::string{"paint_stroke"});

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto tagAfterUndo = space->read<std::string>("/doc/_history/lastOperation/tag");
    REQUIRE(tagAfterUndo.has_value());
    CHECK(*tagAfterUndo == std::string{"paint_stroke"});
}

TEST_CASE("journal multi-step undo redo sequence restores states in order") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/a", std::string{"alpha"}).errors.empty());
    REQUIRE(space->insert("/doc/b", std::string{"beta"}).errors.empty());
    auto removedA = space->take<std::string>("/doc/a");
    REQUIRE(removedA.has_value());
    CHECK(*removedA == std::string{"alpha"});
    REQUIRE(space->insert("/doc/c", std::string{"gamma"}).errors.empty());

    auto statsAfterOps = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterOps.has_value());
    CHECK(statsAfterOps->counts.undo == 4);

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value()); // undo take
    CHECK_FALSE(space->read<std::string>("/doc/c").has_value());

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value()); // undo take on /doc/a
    auto restoredA = space->read<std::string>("/doc/a");
    REQUIRE(restoredA.has_value());
    CHECK(*restoredA == std::string{"alpha"});

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value()); // undo insert /doc/b
    CHECK_FALSE(space->read<std::string>("/doc/b").has_value());

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value()); // undo insert /doc/a
    CHECK_FALSE(space->read<std::string>("/doc/a").has_value());

    auto extraUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(extraUndo.has_value());
    CHECK(extraUndo.error().code == Error::Code::NoObjectFound);

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto redoA = space->read<std::string>("/doc/a");
    REQUIRE(redoA.has_value());
    CHECK(*redoA == std::string{"alpha"});

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto redoB = space->read<std::string>("/doc/b");
    REQUIRE(redoB.has_value());
    CHECK(*redoB == std::string{"beta"});

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    CHECK_FALSE(space->read<std::string>("/doc/a").has_value());

    REQUIRE(space->redo(ConcretePathStringView{"/doc"}).has_value());
    auto redoC = space->read<std::string>("/doc/c");
    REQUIRE(redoC.has_value());
    CHECK(*redoC == std::string{"gamma"});
    auto bFinal = space->read<std::string>("/doc/b");
    REQUIRE(bFinal.has_value());
    CHECK(*bFinal == std::string{"beta"});
}

TEST_SUITE_END();
