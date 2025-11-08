#include "history/UndoableSpace.hpp"
#include "history/UndoHistoryMetadata.hpp"

#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

#include <filesystem>
#include <span>

using namespace SP;
using namespace SP::History;
using SP::ConcretePathStringView;

namespace {

auto makeUndoableSpace(HistoryOptions defaults = {}) -> std::unique_ptr<UndoableSpace> {
    auto inner = std::make_unique<PathSpace>();
    return std::make_unique<UndoableSpace>(std::move(inner), defaults);
}

} // namespace

TEST_SUITE("UndoableSpace") {

TEST_CASE("undo metadata encode decode roundtrip") {
    using namespace SP::History;

    UndoMetadata::EntryMetadata entry;
    entry.generation  = 123;
    entry.bytes       = 456;
    entry.timestampMs = 789;

    auto encodedEntry = UndoMetadata::encodeEntryMeta(entry);
    auto parsedEntry =
        UndoMetadata::parseEntryMeta(std::span<const std::byte>(encodedEntry.data(), encodedEntry.size()));
    REQUIRE(parsedEntry.has_value());
    CHECK(parsedEntry->generation == entry.generation);
    CHECK(parsedEntry->bytes == entry.bytes);
    CHECK(parsedEntry->timestampMs == entry.timestampMs);

    UndoMetadata::StateMetadata state;
    state.liveGeneration = 42;
    state.undoGenerations = {1, 2, 3};
    state.redoGenerations = {4, 5};
    state.manualGc        = true;
    state.ramCacheEntries = 8;

    auto encodedState = UndoMetadata::encodeStateMeta(state);
    auto parsedState =
        UndoMetadata::parseStateMeta(std::span<const std::byte>(encodedState.data(), encodedState.size()));
    REQUIRE(parsedState.has_value());
    CHECK(parsedState->liveGeneration == state.liveGeneration);
    CHECK(parsedState->undoGenerations == state.undoGenerations);
    CHECK(parsedState->redoGenerations == state.redoGenerations);
    CHECK(parsedState->manualGc == state.manualGc);
    CHECK(parsedState->ramCacheEntries == state.ramCacheEntries);
}

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

    REQUIRE(space->undo(ConcretePathStringView{"/doc"}).has_value());
    auto secondUndo = space->undo(ConcretePathStringView{"/doc"});
    CHECK_FALSE(secondUndo.has_value());
    CHECK(secondUndo.error().code == Error::Code::NoObjectFound);
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
    CHECK(stats->counts.undo == 1);
    CHECK(stats->counts.redo == 0);
    CHECK(stats->trim.operationCount == 0);

    REQUIRE(space->undo(ConcretePathStringView{"/items"}).has_value());
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
    CHECK(stats->counts.undo == 2);
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

TEST_CASE("history telemetry paths expose stats") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space->insert("/doc/value", 42).errors.empty());

    auto manualGc = space->read<bool>("/doc/_history/stats/manualGcEnabled");
    REQUIRE(manualGc.has_value());
    CHECK_FALSE(*manualGc);

    auto statsUndoCount = space->read<std::size_t>("/doc/_history/stats/undoCount");
    REQUIRE(statsUndoCount.has_value());
    CHECK(*statsUndoCount == 1);

    auto lastOpType = space->read<std::string>("/doc/_history/lastOperation/type");
    REQUIRE(lastOpType.has_value());
    CHECK(*lastOpType == std::string{"commit"});
}

TEST_CASE("history rejects unsupported payloads") {
    SUBCASE("task payloads surface descriptive errors and skip history entries") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        auto task = []() -> int { return 7; };
        auto result = space->insert("/doc/task", task,
                                    In{.executionCategory = ExecutionCategory::Lazy});
        CHECK(result.nbrTasksInserted == 1);
        REQUIRE_FALSE(result.errors.empty());

        auto const& err = result.errors.front();
        CHECK(err.code == Error::Code::UnknownError);
        REQUIRE(err.message.has_value());
        CHECK(err.message->find("tasks or futures") != std::string::npos);

        auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
        REQUIRE(stats.has_value());
        CHECK(stats->counts.undo == 0);
        CHECK(stats->counts.redo == 0);
        CHECK(stats->unsupported.total == 1);
        REQUIRE(stats->unsupported.recent.size() == 1);
        CHECK(stats->unsupported.recent.front().path == "/doc/task");
        CHECK(stats->unsupported.recent.front().reason.find("tasks or futures") != std::string::npos);

        auto totalCount = space->read<std::size_t>("/doc/_history/unsupported/totalCount");
        REQUIRE(totalCount.has_value());
        CHECK(*totalCount == 1);
        auto recentCount = space->read<std::size_t>("/doc/_history/unsupported/recentCount");
        REQUIRE(recentCount.has_value());
        CHECK(*recentCount == 1);
        auto recentReason = space->read<std::string>("/doc/_history/unsupported/recent/0/reason");
        REQUIRE(recentReason.has_value());
        CHECK(recentReason->find("tasks or futures") != std::string::npos);
        auto recentPath = space->read<std::string>("/doc/_history/unsupported/recent/0/path");
        REQUIRE(recentPath.has_value());
        CHECK(*recentPath == std::string{"/doc/task"});
    }

    SUBCASE("nested PathSpaces are rejected with clear messaging") {
        auto space = makeUndoableSpace();
        REQUIRE(space);

        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/value", 1).nbrValuesInserted == 1);

        auto result = space->insert("/doc/nested", std::move(nested));
        CHECK(result.nbrSpacesInserted == 1);
        REQUIRE_FALSE(result.errors.empty());

        auto const& err = result.errors.front();
        CHECK(err.code == Error::Code::UnknownError);
        REQUIRE(err.message.has_value());
        CHECK(err.message->find("nested PathSpaces") != std::string::npos);

        auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
        REQUIRE(stats.has_value());
        CHECK(stats->counts.undo == 0);
        CHECK(stats->counts.redo == 0);
        CHECK(stats->unsupported.total == 1);
        REQUIRE(stats->unsupported.recent.size() == 1);
        CHECK(stats->unsupported.recent.front().path == "/doc/nested");
        CHECK(stats->unsupported.recent.front().reason.find("nested PathSpaces") != std::string::npos);
    }
}

TEST_CASE("shared undo stack keys are rejected across roots") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.sharedStackKey = std::string{"docShared"};

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto second = space->enableHistory(ConcretePathStringView{"/notes"}, opts);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Error::Code::InvalidPermissions);
    REQUIRE(second.error().message.has_value());
    CHECK(second.error().message->find("shared undo stacks") != std::string::npos);
}

TEST_CASE("persistence restores state and undo history") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions defaults;
    defaults.persistHistory      = true;
    defaults.persistenceRoot     = tempRoot.string();
    defaults.persistenceNamespace = "suite";
    defaults.ramCacheEntries     = 2;

    auto space = makeUndoableSpace(defaults);
    REQUIRE(space);
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(space->insert("/doc/title", std::string{"alpha"}).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 1);

    space.reset();

    auto reloaded = makeUndoableSpace(defaults);
    REQUIRE(reloaded);
    REQUIRE(reloaded->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto value = reloaded->read<std::string>("/doc/title");
    REQUIRE(value.has_value());
    CHECK(*value == std::string{"alpha"});

    REQUIRE(reloaded->undo(ConcretePathStringView{"/doc"}).has_value());
    auto missing = reloaded->read<std::string>("/doc/title");
    CHECK_FALSE(missing.has_value());

    REQUIRE(reloaded->redo(ConcretePathStringView{"/doc"}).has_value());
    auto restored = reloaded->read<std::string>("/doc/title");
    REQUIRE(restored.has_value());
    CHECK(*restored == std::string{"alpha"});

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("savefile export import roundtrip retains history") {
    auto savePath = std::filesystem::temp_directory_path() / "undoable_space_savefile.bin";
    std::error_code removeEc;
    std::filesystem::remove(savePath, removeEc);

    auto source = makeUndoableSpace();
    REQUIRE(source);
    REQUIRE(source->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(source->insert("/doc/value", std::string{"alpha"}).errors.empty());
    REQUIRE(source->insert("/doc/value", std::string{"beta"}).errors.empty());

    auto exportResult =
        source->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, true);
    REQUIRE(exportResult.has_value());

    auto destination = makeUndoableSpace();
    REQUIRE(destination);
    REQUIRE(destination->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto importResult =
        destination->importHistorySavefile(ConcretePathStringView{"/doc"}, savePath);
    REQUIRE(importResult.has_value());

    auto statsBefore = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsBefore.has_value());
    CHECK(statsBefore->counts.undo >= 1);

    REQUIRE(destination->undo(ConcretePathStringView{"/doc"}).has_value());
    auto statsAfterUndo = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterUndo.has_value());
    CHECK(statsAfterUndo->counts.undo + 1 == statsBefore->counts.undo);
    CHECK(statsAfterUndo->counts.redo >= 1);

    REQUIRE(destination->redo(ConcretePathStringView{"/doc"}).has_value());
    auto statsAfterRedo = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(statsAfterRedo.has_value());
    CHECK(statsAfterRedo->counts.undo == statsBefore->counts.undo);
    CHECK(statsAfterRedo->counts.redo == 0);

    auto first = destination->take<std::string>("/doc/value");
    REQUIRE(first.has_value());
    CHECK(*first == std::string{"alpha"});
    auto second = destination->take<std::string>("/doc/value");
    REQUIRE(second.has_value());
    CHECK(*second == std::string{"beta"});

    auto stats = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo >= 1);

    std::filesystem::remove(savePath, removeEc);
}

TEST_CASE("mutation journal roots are gated behind feature flag") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;

    REQUIRE(space->enableHistory(ConcretePathStringView{"/journal"}, opts).has_value());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/journal"});
    REQUIRE_FALSE(stats.has_value());
    REQUIRE(stats.error().message.has_value());
    CHECK(stats.error().message->find("Mutation journal") != std::string::npos);

    auto undoResult = space->undo(ConcretePathStringView{"/journal"});
    REQUIRE_FALSE(undoResult.has_value());
    REQUIRE(undoResult.error().message.has_value());
    CHECK(undoResult.error().message->find("Mutation journal") != std::string::npos);

    auto insertResult = space->insert("/journal/value", std::string{"alpha"});
    CHECK(insertResult.errors.empty());

    auto value = space->read<std::string>("/journal/value");
    REQUIRE(value.has_value());
    CHECK(*value == "alpha");

    REQUIRE(space->disableHistory(ConcretePathStringView{"/journal"}).has_value());
}

}
