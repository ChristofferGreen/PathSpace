#include "history/UndoableSpace.hpp"
#include "history/UndoHistoryMetadata.hpp"

#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

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
    CHECK(stats->counts.undo == 1);
    CHECK(stats->counts.redo == 0);
    CHECK(stats->trim.operationCount == 0);

    REQUIRE(space->undo(ConcretePathStringView{"/items"}).has_value());
    CHECK_FALSE(space->read<int>("/items/a").has_value());
    CHECK_FALSE(space->read<int>("/items/b").has_value());
}

TEST_CASE("journal beginTransaction reports migration not yet complete") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/items"}, opts).has_value());

    auto txExpected = space->beginTransaction(ConcretePathStringView{"/items"});
    CHECK_FALSE(txExpected.has_value());
    auto const& err = txExpected.error();
    CHECK(err.code == Error::Code::UnknownError);
    REQUIRE(err.message.has_value());
    CHECK(err.message->find("Mutation journal history not yet supported") != std::string::npos);
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

TEST_CASE("journal telemetry matches snapshot telemetry outputs") {
    struct TelemetryCapture {
        HistoryStats stats;
        std::size_t undoCountPath      = 0;
        std::size_t redoCountPath      = 0;
        std::size_t liveBytesPath      = 0;
        std::size_t bytesRetainedPath  = 0;
        bool        manualGcEnabled    = false;
        std::size_t lastBytesAfterPath = 0;
    };

    auto captureTelemetry = [](bool useJournal) {
        TelemetryCapture capture;
        auto             space = makeUndoableSpace();
        REQUIRE(space);

        HistoryOptions opts;
        opts.useMutationJournal = useJournal;
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

        REQUIRE(space->insert("/doc/value", std::string{"alpha"}).errors.empty());
        REQUIRE(space->insert("/doc/value", std::string{"bravo"}).errors.empty());
        auto taken = space->take<std::string>("/doc/value");
        REQUIRE(taken.has_value());
        CHECK(*taken == std::string{"bravo"});
        REQUIRE(space->insert("/doc/value", std::string{"charlie"}).errors.empty());

        auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
        REQUIRE(stats.has_value());
        capture.stats = stats.value();

        auto undoCount = space->read<std::size_t>("/doc/_history/stats/undoCount");
        REQUIRE(undoCount.has_value());
        capture.undoCountPath = *undoCount;

        auto redoCount = space->read<std::size_t>("/doc/_history/stats/redoCount");
        REQUIRE(redoCount.has_value());
        capture.redoCountPath = *redoCount;

        auto liveBytes = space->read<std::size_t>("/doc/_history/stats/liveBytes");
        REQUIRE(liveBytes.has_value());
        capture.liveBytesPath = *liveBytes;

        auto bytesRetained = space->read<std::size_t>("/doc/_history/stats/bytesRetained");
        REQUIRE(bytesRetained.has_value());
        capture.bytesRetainedPath = *bytesRetained;

        auto manualGc = space->read<bool>("/doc/_history/stats/manualGcEnabled");
        REQUIRE(manualGc.has_value());
        capture.manualGcEnabled = *manualGc;

        auto lastBytesAfter = space->read<std::size_t>("/doc/_history/lastOperation/bytesAfter");
        REQUIRE(lastBytesAfter.has_value());
        capture.lastBytesAfterPath = *lastBytesAfter;

        return capture;
    };

    auto snapshotTelemetry = captureTelemetry(false);
    auto journalTelemetry  = captureTelemetry(true);

    CHECK(snapshotTelemetry.stats.counts.undo == journalTelemetry.stats.counts.undo);
    CHECK(snapshotTelemetry.stats.counts.redo == journalTelemetry.stats.counts.redo);
    CHECK(snapshotTelemetry.stats.counts.manualGarbageCollect
          == journalTelemetry.stats.counts.manualGarbageCollect);
    CHECK(snapshotTelemetry.stats.counts.diskEntries == journalTelemetry.stats.counts.diskEntries);
    CHECK(snapshotTelemetry.stats.counts.cachedUndo == journalTelemetry.stats.counts.cachedUndo);
    CHECK(snapshotTelemetry.stats.counts.cachedRedo == journalTelemetry.stats.counts.cachedRedo);

    CHECK(snapshotTelemetry.stats.bytes.undo == journalTelemetry.stats.bytes.undo);
    CHECK(snapshotTelemetry.stats.bytes.redo == journalTelemetry.stats.bytes.redo);
    CHECK(snapshotTelemetry.stats.bytes.live == journalTelemetry.stats.bytes.live);
    CHECK(snapshotTelemetry.stats.bytes.total == journalTelemetry.stats.bytes.total);

    CHECK(snapshotTelemetry.stats.trim.operationCount
          == journalTelemetry.stats.trim.operationCount);
    CHECK(snapshotTelemetry.stats.trim.entries == journalTelemetry.stats.trim.entries);
    CHECK(snapshotTelemetry.stats.trim.bytes == journalTelemetry.stats.trim.bytes);

    CHECK(snapshotTelemetry.stats.unsupported.total == journalTelemetry.stats.unsupported.total);
    CHECK(snapshotTelemetry.stats.unsupported.recent.size()
          == journalTelemetry.stats.unsupported.recent.size());

    REQUIRE(snapshotTelemetry.stats.lastOperation.has_value());
    REQUIRE(journalTelemetry.stats.lastOperation.has_value());
    auto const& snapshotLast = *snapshotTelemetry.stats.lastOperation;
    auto const& journalLast  = *journalTelemetry.stats.lastOperation;
    CHECK(snapshotLast.type == journalLast.type);
    CHECK(snapshotLast.success == journalLast.success);
    CHECK(snapshotLast.undoCountAfter == journalLast.undoCountAfter);
    CHECK(snapshotLast.redoCountAfter == journalLast.redoCountAfter);
    CHECK(snapshotLast.bytesAfter == journalLast.bytesAfter);

    CHECK(snapshotTelemetry.undoCountPath == journalTelemetry.undoCountPath);
    CHECK(snapshotTelemetry.redoCountPath == journalTelemetry.redoCountPath);
    CHECK(snapshotTelemetry.liveBytesPath == journalTelemetry.liveBytesPath);
    CHECK(snapshotTelemetry.bytesRetainedPath == journalTelemetry.bytesRetainedPath);
    CHECK(snapshotTelemetry.manualGcEnabled == journalTelemetry.manualGcEnabled);
    CHECK(snapshotTelemetry.lastBytesAfterPath == journalTelemetry.lastBytesAfterPath);
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

TEST_CASE("journal persistence replays entries on enable") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_journal_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions defaults;
    defaults.persistHistory       = true;
    defaults.persistenceRoot      = tempRoot.string();
    defaults.persistenceNamespace = "journal_suite";
    defaults.useMutationJournal   = true;

    {
        auto space = makeUndoableSpace(defaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

        REQUIRE(space->insert("/doc/value_a", std::string{"alpha"}).errors.empty());
        REQUIRE(space->insert("/doc/value_b", std::string{"beta"}).errors.empty());

        auto currentA = space->read<std::string>("/doc/value_a");
        REQUIRE(currentA.has_value());
        CHECK(*currentA == std::string{"alpha"});

        auto currentB = space->read<std::string>("/doc/value_b");
        REQUIRE(currentB.has_value());
        CHECK(*currentB == std::string{"beta"});
    }

    auto reloaded = makeUndoableSpace(defaults);
    REQUIRE(reloaded);
    REQUIRE(reloaded->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto reloadedA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(reloadedA.has_value());
    CHECK(*reloadedA == std::string{"alpha"});

    auto reloadedB = reloaded->read<std::string>("/doc/value_b");
    REQUIRE(reloadedB.has_value());
    CHECK(*reloadedB == std::string{"beta"});

    REQUIRE(reloaded->undo(ConcretePathStringView{"/doc"}).has_value());
    auto afterUndoB = reloaded->read<std::string>("/doc/value_b");
    CHECK_FALSE(afterUndoB.has_value());
    auto afterUndoA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(afterUndoA.has_value());
    CHECK(*afterUndoA == std::string{"alpha"});

    REQUIRE(reloaded->redo(ConcretePathStringView{"/doc"}).has_value());
    auto afterRedoB = reloaded->read<std::string>("/doc/value_b");
    REQUIRE(afterRedoB.has_value());
    CHECK(*afterRedoB == std::string{"beta"});
    auto afterRedoA = reloaded->read<std::string>("/doc/value_a");
    REQUIRE(afterRedoA.has_value());
    CHECK(*afterRedoA == std::string{"alpha"});

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("persistence namespace validation rejects path traversal tokens") {
    auto tempRoot = std::filesystem::temp_directory_path() / "undoable_space_namespace_validation";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    HistoryOptions snapshotDefaults;
    snapshotDefaults.persistHistory       = true;
    snapshotDefaults.persistenceRoot      = tempRoot.string();
    snapshotDefaults.persistenceNamespace = "invalid/namespace";

    {
        auto space = makeUndoableSpace(snapshotDefaults);
        REQUIRE(space);
        auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
        REQUIRE_FALSE(enable.has_value());
        CHECK(enable.error().code == Error::Code::InvalidPermissions);
    }

    snapshotDefaults.persistenceNamespace = "snapshot_ns";
    {
        auto space = makeUndoableSpace(snapshotDefaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    }

    HistoryOptions journalDefaults = snapshotDefaults;
    journalDefaults.useMutationJournal   = true;
    journalDefaults.persistenceNamespace = "bad namespace";
    {
        auto space = makeUndoableSpace(journalDefaults);
        REQUIRE(space);
        auto enable = space->enableHistory(ConcretePathStringView{"/doc"});
        REQUIRE_FALSE(enable.has_value());
        CHECK(enable.error().code == Error::Code::InvalidPermissions);
    }

    journalDefaults.persistenceNamespace = "journal_ns";
    {
        auto space = makeUndoableSpace(journalDefaults);
        REQUIRE(space);
        REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    }

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

TEST_CASE("journal handles concurrent mutation and history operations") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.maxEntries         = 4096;
    opts.maxBytesRetained   = 512 * 1024;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/stress"}, opts).has_value());

    constexpr int kThreadCount = 4;
    constexpr int kIterations  = 80;

    std::atomic<int> undoSuccess{0};
    std::atomic<int> redoSuccess{0};
    std::atomic<int> gcSuccess{0};

    std::mutex errorMutex;
    std::vector<std::string> errors;

    auto recordError = [&](int threadIndex, std::string message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errors.emplace_back("[thread " + std::to_string(threadIndex) + "] " + std::move(message));
    };

    auto const root = ConcretePathStringView{"/stress"};
    auto* const rawSpace = space.get();

    auto worker = [&](int threadIndex) {
        for (int iter = 0; iter < kIterations; ++iter) {
            std::string key = "/stress/thread";
            key += std::to_string(threadIndex);
            key += "/value";
            key += std::to_string(iter);

            auto insertResult = rawSpace->insert(key.c_str(), iter);
            if (!insertResult.errors.empty()) {
                recordError(threadIndex, std::string("insert reported errors for ") + key);
            }

            switch (iter % 3) {
            case 0: {
                auto taken = rawSpace->take<int>(key.c_str());
                if (taken.has_value()) {
                    if (*taken != iter) {
                        recordError(threadIndex, std::string("take returned unexpected value for ") + key);
                    }
                }

                auto undoResult = rawSpace->undo(root);
                if (undoResult.has_value()) {
                    undoSuccess.fetch_add(1, std::memory_order_relaxed);
                    auto redoResult = rawSpace->redo(root);
                    if (redoResult.has_value()) {
                        redoSuccess.fetch_add(1, std::memory_order_relaxed);
                    } else if (redoResult.error().code != Error::Code::NoObjectFound) {
                        recordError(threadIndex, "redo returned unexpected error code");
                    }
                } else if (undoResult.error().code != Error::Code::NoObjectFound) {
                    recordError(threadIndex, "undo returned unexpected error code");
                }
                break;
            }
            case 1: {
                auto undoResult = rawSpace->undo(root);
                if (undoResult.has_value()) {
                    undoSuccess.fetch_add(1, std::memory_order_relaxed);
                    auto redoResult = rawSpace->redo(root);
                    if (redoResult.has_value()) {
                        redoSuccess.fetch_add(1, std::memory_order_relaxed);
                    } else if (redoResult.error().code != Error::Code::NoObjectFound) {
                        recordError(threadIndex, "redo returned unexpected error code");
                    }
                } else if (undoResult.error().code != Error::Code::NoObjectFound) {
                    recordError(threadIndex, "undo returned unexpected error code");
                }
                break;
            }
            default: {
                auto gcResult = rawSpace->insert("/stress/_history/garbage_collect", true);
                if (!gcResult.errors.empty()) {
                    recordError(threadIndex, "garbage_collect insert reported errors");
                } else {
                    gcSuccess.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            }

            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& workerThread : threads) {
        workerThread.join();
    }

    if (!errors.empty()) {
        for (auto const& err : errors) {
            INFO(err);
        }
        FAIL("concurrent journal stress encountered errors");
    }

    CHECK(undoSuccess.load(std::memory_order_relaxed) > 0);
    CHECK(redoSuccess.load(std::memory_order_relaxed) > 0);
    CHECK(gcSuccess.load(std::memory_order_relaxed) > 0);

    auto stats = space->getHistoryStats(root);
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo >= 0);
    CHECK(stats->counts.redo >= 0);

    auto markerInsert = space->insert("/stress/marker", std::string{"marker"});
    CHECK(markerInsert.errors.empty());

    std::size_t undone = 0;
    for (;;) {
        auto undoResult = space->undo(root);
        if (!undoResult.has_value()) {
            CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        ++undone;
    }
    CHECK(undone > 0);

    std::size_t redone = 0;
    for (;;) {
        auto redoResult = space->redo(root);
        if (!redoResult.has_value()) {
            CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        ++redone;
    }
    CHECK(redone == undone);

    auto markerCleanup = space->take<std::string>("/stress/marker");
    REQUIRE(markerCleanup.has_value());
    CHECK(*markerCleanup == "marker");

    auto postInsert = space->insert("/stress/post_check", std::string{"ok"});
    CHECK(postInsert.errors.empty());

    REQUIRE(space->undo(root).has_value());
    REQUIRE(space->redo(root).has_value());

    auto cleanup = space->take<std::string>("/stress/post_check");
    REQUIRE(cleanup.has_value());
    CHECK(*cleanup == "ok");

    auto finalGc = space->insert("/stress/_history/garbage_collect", true);
    CHECK(finalGc.errors.empty());
}

TEST_CASE("journal fuzz sequence maintains parity with reference model") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.manualGarbageCollect = true;
    opts.maxEntries           = 4096;
    opts.maxBytesRetained     = 2 * 1024 * 1024;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/fuzz"}, opts).has_value());

    struct ReferenceModel {
        enum class Kind { InsertReplace, Take };
        struct Entry {
            Kind kind;
            std::string key;
            std::optional<int> prior;
            std::optional<int> value;
        };

        std::unordered_map<std::string, int> values;
        std::vector<Entry> undoStack;
        std::vector<Entry> redoStack;

        void insert(std::string const& key, int value) {
            Entry entry;
            entry.kind  = Kind::InsertReplace;
            entry.key   = key;
            entry.value = value;
            if (auto it = values.find(key); it != values.end()) {
                entry.prior = it->second;
            }
            values[key] = value;
            undoStack.push_back(entry);
            redoStack.clear();
        }

        std::optional<int> take(std::string const& key) {
            auto it = values.find(key);
            if (it == values.end()) {
                return std::nullopt;
            }
            Entry entry;
            entry.kind  = Kind::Take;
            entry.key   = key;
            entry.prior = it->second;
            values.erase(it);
            undoStack.push_back(entry);
            redoStack.clear();
            return entry.prior;
        }

        bool undo() {
            if (undoStack.empty()) {
                return false;
            }
            auto entry = undoStack.back();
            undoStack.pop_back();
            switch (entry.kind) {
            case Kind::InsertReplace:
                if (entry.prior.has_value()) {
                    values[entry.key] = *entry.prior;
                } else {
                    values.erase(entry.key);
                }
                break;
            case Kind::Take:
                if (entry.prior.has_value()) {
                    values[entry.key] = *entry.prior;
                }
                break;
            }
            redoStack.push_back(entry);
            return true;
        }

        bool redo() {
            if (redoStack.empty()) {
                return false;
            }
            auto entry = redoStack.back();
            redoStack.pop_back();
            switch (entry.kind) {
            case Kind::InsertReplace:
                REQUIRE(entry.value.has_value());
                values[entry.key] = *entry.value;
                break;
            case Kind::Take:
                values.erase(entry.key);
                break;
            }
            undoStack.push_back(entry);
            return true;
        }

        std::optional<int> read(std::string const& key) const {
            if (auto it = values.find(key); it != values.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        void alignToStats(std::size_t undoCount, std::size_t redoCount) {
            trimStackTo(undoStack, undoCount);
            trimStackTo(redoStack, redoCount);
        }

        std::size_t undoCount() const { return undoStack.size(); }
        std::size_t redoCount() const { return redoStack.size(); }

    private:
        static void trimStackTo(std::vector<Entry>& stack, std::size_t target) {
            REQUIRE(stack.size() >= target);
            while (stack.size() > target) {
                stack.erase(stack.begin());
            }
        }
    };

    ReferenceModel reference;

    constexpr std::array<std::string_view, 6> kKeySuffixes{
        "/value/a", "/value/b", "/value/c", "/value/d", "/value/e", "/value/f"};
    const ConcretePathStringView root{"/fuzz"};

    auto buildKey = [](std::string_view suffix) {
        std::string key{"/fuzz"};
        key.append(suffix);
        return key;
    };

    std::vector<std::string> keyPaths;
    keyPaths.reserve(kKeySuffixes.size());
    for (auto suffix : kKeySuffixes) {
        keyPaths.push_back(buildKey(suffix));
    }

    auto checkStateMatches = [&] {
        for (auto const& key : keyPaths) {
            auto expected = reference.read(key);
            auto actual   = space->read<int>(key.c_str());
            if (expected.has_value()) {
                REQUIRE(actual.has_value());
                CHECK(*actual == *expected);
            } else {
                CHECK_FALSE(actual.has_value());
            }
        }
    };

    std::mt19937 rng{1337};
    std::uniform_int_distribution<int> opDist(0, 5);
    std::uniform_int_distribution<int> keyDist(0, static_cast<int>(keyPaths.size() - 1));
    std::uniform_int_distribution<int> valueDist(-1000, 1000);

    constexpr int kIterations = 250;

    for (int iter = 0; iter < kIterations; ++iter) {
        int opIndex = opDist(rng);
        auto const& key = keyPaths[static_cast<std::size_t>(keyDist(rng))];

        switch (opIndex) {
        case 0:
        case 1: {
            int value = valueDist(rng);
            auto result = space->insert(key.c_str(), value);
            CHECK(result.errors.empty());
            reference.insert(key, value);
            break;
        }
        case 2: {
            auto taken = space->take<int>(key.c_str());
            auto refTaken = reference.take(key);
            CHECK(taken.has_value() == refTaken.has_value());
            if (taken.has_value()) {
                CHECK(*taken == *refTaken);
            }
            break;
        }
        case 3: {
            auto undoResult = space->undo(root);
            bool refUndid   = reference.undo();
            if (undoResult.has_value()) {
                CHECK(refUndid);
            } else {
                CHECK_FALSE(refUndid);
                CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            }
            break;
        }
        case 4: {
            auto redoResult = space->redo(root);
            bool refRedid   = reference.redo();
            if (redoResult.has_value()) {
                CHECK(refRedid);
            } else {
                CHECK_FALSE(refRedid);
                CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            }
            break;
        }
        default: {
            auto gc = space->insert("/fuzz/_history/garbage_collect", true);
            CHECK(gc.errors.empty());
            auto statsAfterGc = space->getHistoryStats(root);
            REQUIRE(statsAfterGc.has_value());
            reference.alignToStats(statsAfterGc->counts.undo, statsAfterGc->counts.redo);
            CHECK(reference.undoCount() == statsAfterGc->counts.undo);
            CHECK(reference.redoCount() == statsAfterGc->counts.redo);
            checkStateMatches();
            continue;
        }
        }

        auto stats = space->getHistoryStats(root);
        REQUIRE(stats.has_value());
        reference.alignToStats(stats->counts.undo, stats->counts.redo);

        CHECK(reference.undoCount() == stats->counts.undo);
        CHECK(reference.redoCount() == stats->counts.redo);

        checkStateMatches();
    }

    // Drain undo stack and ensure redo parity follows.
    for (;;) {
        auto undoResult = space->undo(root);
        bool refUndid   = reference.undo();
        if (!undoResult.has_value()) {
            CHECK_FALSE(refUndid);
            CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        CHECK(refUndid);
        checkStateMatches();
    }

    for (;;) {
        auto redoResult = space->redo(root);
        bool refRedid   = reference.redo();
        if (!redoResult.has_value()) {
            CHECK_FALSE(refRedid);
            CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        CHECK(refRedid);
        checkStateMatches();
    }
}

TEST_CASE("mutation journal roots require explicit opt-in") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;

    REQUIRE(space->enableHistory(ConcretePathStringView{"/journal"}, opts).has_value());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/journal"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 0);
    CHECK(stats->counts.redo == 0);

    auto insertResult = space->insert("/journal/value", std::string{"alpha"});
    CHECK(insertResult.errors.empty());

    auto value = space->read<std::string>("/journal/value");
    REQUIRE(value.has_value());
    CHECK(*value == "alpha");

    REQUIRE(space->undo(ConcretePathStringView{"/journal"}).has_value());
    CHECK_FALSE(space->read<std::string>("/journal/value").has_value());

    auto redoResult = space->redo(ConcretePathStringView{"/journal"});
    REQUIRE(redoResult.has_value());
    auto restored = space->read<std::string>("/journal/value");
    REQUIRE(restored.has_value());
    CHECK(*restored == "alpha");

    REQUIRE(space->disableHistory(ConcretePathStringView{"/journal"}).has_value());
}

}
