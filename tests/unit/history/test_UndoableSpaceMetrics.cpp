#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.metrics");

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

TEST_CASE("history telemetry exposes retention limits and persistence flags") {
    auto tempRoot = std::filesystem::temp_directory_path() / "history_limits_schema";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.maxEntries            = 256;
    opts.maxBytesRetained      = 8192;
    opts.keepLatestFor         = std::chrono::milliseconds{1500};
    opts.ramCacheEntries       = 32;
    opts.maxDiskBytes          = 65536;
    opts.persistHistory        = true;
    opts.restoreFromPersistence = false;
    opts.persistenceRoot       = tempRoot.string();
    opts.persistenceNamespace  = "limits_schema";
    opts.useMutationJournal    = true;

    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    REQUIRE(space->insert("/doc/value", 7).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->limits.maxEntries == opts.maxEntries);
    CHECK(stats->limits.maxBytesRetained == opts.maxBytesRetained);
    CHECK(stats->limits.keepLatestForMs == static_cast<std::uint64_t>(opts.keepLatestFor.count()));
    CHECK(stats->limits.ramCacheEntries == opts.ramCacheEntries);
    CHECK(stats->limits.maxDiskBytes == opts.maxDiskBytes);
    CHECK(stats->limits.persistHistory);
    CHECK_FALSE(stats->limits.restoreFromPersistence);

    auto maxEntriesPath = space->read<std::size_t>("/doc/_history/stats/limits/maxEntries");
    REQUIRE(maxEntriesPath.has_value());
    CHECK(*maxEntriesPath == opts.maxEntries);

    auto maxBytesPath =
        space->read<std::size_t>("/doc/_history/stats/limits/maxBytesRetained");
    REQUIRE(maxBytesPath.has_value());
    CHECK(*maxBytesPath == opts.maxBytesRetained);

    auto keepLatestPath =
        space->read<std::uint64_t>("/doc/_history/stats/limits/keepLatestForMs");
    REQUIRE(keepLatestPath.has_value());
    CHECK(*keepLatestPath == stats->limits.keepLatestForMs);

    auto ramCachePath =
        space->read<std::size_t>("/doc/_history/stats/limits/ramCacheEntries");
    REQUIRE(ramCachePath.has_value());
    CHECK(*ramCachePath == opts.ramCacheEntries);

    auto maxDiskPath = space->read<std::size_t>("/doc/_history/stats/limits/maxDiskBytes");
    REQUIRE(maxDiskPath.has_value());
    CHECK(*maxDiskPath == opts.maxDiskBytes);

    auto persistFlag = space->read<bool>("/doc/_history/stats/limits/persistHistory");
    REQUIRE(persistFlag.has_value());
    CHECK(*persistFlag);

    auto restoreFlag =
        space->read<bool>("/doc/_history/stats/limits/restoreFromPersistence");
    REQUIRE(restoreFlag.has_value());
    CHECK_FALSE(*restoreFlag);

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("history telemetry compaction alias mirrors trim metrics") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.maxEntries = 1;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space->insert("/doc/value", std::string{"alpha"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"beta"}).errors.empty());
    REQUIRE(space->insert("/doc/value", std::string{"charlie"}).errors.empty());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->trim.operationCount >= 1);

    auto compactionRuns = space->read<std::size_t>("/doc/_history/stats/compaction/runs");
    REQUIRE(compactionRuns.has_value());
    CHECK(*compactionRuns == stats->trim.operationCount);

    auto compactionEntries = space->read<std::size_t>("/doc/_history/stats/compaction/entries");
    REQUIRE(compactionEntries.has_value());
    CHECK(*compactionEntries == stats->trim.entries);

    auto compactionBytes = space->read<std::size_t>("/doc/_history/stats/compaction/bytes");
    REQUIRE(compactionBytes.has_value());
    CHECK(*compactionBytes == stats->trim.bytes);

    auto compactionTimestamp =
        space->read<std::uint64_t>("/doc/_history/stats/compaction/lastTimestampMs");
    REQUIRE(compactionTimestamp.has_value());
    CHECK(*compactionTimestamp == stats->trim.lastTimestampMs);
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
        CHECK(*taken == std::string{"alpha"});
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

    CHECK(snapshotTelemetry.stats.bytes.undo >= journalTelemetry.stats.bytes.undo);
    CHECK(snapshotTelemetry.stats.bytes.redo >= journalTelemetry.stats.bytes.redo);
    CHECK(snapshotTelemetry.stats.bytes.live > 0);
    CHECK(journalTelemetry.stats.bytes.live > 0);
    CHECK(snapshotTelemetry.stats.bytes.live >= journalTelemetry.stats.bytes.live);
    CHECK(snapshotTelemetry.stats.bytes.total >= journalTelemetry.stats.bytes.total);

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

    CHECK(snapshotLast.bytesAfter >= journalLast.bytesAfter);

    CHECK(snapshotTelemetry.undoCountPath == snapshotTelemetry.stats.counts.undo);
    CHECK(journalTelemetry.undoCountPath == journalTelemetry.stats.counts.undo);
    CHECK(snapshotTelemetry.redoCountPath == snapshotTelemetry.stats.counts.redo);
    CHECK(journalTelemetry.redoCountPath == journalTelemetry.stats.counts.redo);
    CHECK(snapshotTelemetry.liveBytesPath == snapshotTelemetry.stats.bytes.live);
    CHECK(journalTelemetry.liveBytesPath == journalTelemetry.stats.bytes.live);
    CHECK(snapshotTelemetry.bytesRetainedPath == snapshotTelemetry.stats.bytes.total);
    CHECK(journalTelemetry.bytesRetainedPath == journalTelemetry.stats.bytes.total);
    CHECK(snapshotTelemetry.manualGcEnabled == journalTelemetry.manualGcEnabled);
    CHECK(snapshotTelemetry.lastBytesAfterPath == snapshotTelemetry.stats.bytes.total);
    CHECK(journalTelemetry.lastBytesAfterPath == journalTelemetry.stats.bytes.total);
}

TEST_SUITE_END();
