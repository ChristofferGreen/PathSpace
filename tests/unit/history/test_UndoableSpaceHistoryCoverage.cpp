#define private public
#include "history/UndoableSpace.hpp"
#undef private

#include "PathSpace.hpp"
#include "core/NodeData.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "third_party/doctest.h"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using namespace SP;
using namespace SP::History;
namespace UndoPaths = SP::History::UndoUtils::Paths;

TEST_SUITE_BEGIN("history.undoable.history.coverage");

TEST_CASE("UndoableSpace payloadBytes clamps empty buffers") {
    NodeData empty;
    CHECK(UndoableSpace::payloadBytes(empty) == 0);

    std::optional<NodeData> none;
    CHECK(UndoableSpace::payloadBytes(none) == 0);

    int value = 7;
    InputMetadata meta{InputMetadataT<int>{}};
    NodeData filled;
    REQUIRE_FALSE(filled.serialize(InputData{&value, meta}).has_value());
    CHECK(UndoableSpace::payloadBytes(filled) > 0);
}

TEST_CASE("UndoableSpace adjustLiveBytes clamps underflow") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    std::size_t live = 3;
    space.adjustLiveBytes(live, 10, 2);
    CHECK(live == 0);

    live = 5;
    space.adjustLiveBytes(live, 2, 9);
    CHECK(live == 12);
}

TEST_CASE("UndoableSpace records and reorders unsupported payload telemetry") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;

    space.recordJournalUnsupportedPayload(state, "/a", "reason");
    space.recordJournalUnsupportedPayload(state, "/b", "reason2");
    REQUIRE(state.telemetry.unsupportedLog.size() == 2);

    space.recordJournalUnsupportedPayload(state, "/a", "reason");
    REQUIRE(state.telemetry.unsupportedLog.size() == 2);
    CHECK(state.telemetry.unsupportedLog.back().path == "/a");
    CHECK(state.telemetry.unsupportedLog.back().reason == "reason");
    CHECK(state.telemetry.unsupportedLog.back().occurrences == 2);
}

TEST_CASE("UndoableSpace parseJournalRelativeComponents rejects invalid roots") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.components = {"root"};

    auto wrongRoot = space.parseJournalRelativeComponents(state, "/other/node");
    CHECK_FALSE(wrongRoot.has_value());
    CHECK(wrongRoot.error().code == Error::Code::InvalidPermissions);

    auto globPath = space.parseJournalRelativeComponents(state, "/root/*");
    CHECK_FALSE(globPath.has_value());
    CHECK(globPath.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("UndoableSpace interpretSteps normalizes step counts") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    int steps = 4;
    InputMetadata metaInt{InputMetadataT<int>{}};
    CHECK(space.interpretSteps(InputData{&steps, metaInt}) == 4);

    int zero = 0;
    CHECK(space.interpretSteps(InputData{&zero, metaInt}) == 1);

    std::int64_t neg = -3;
    InputMetadata metaI64{InputMetadataT<std::int64_t>{}};
    CHECK(space.interpretSteps(InputData{&neg, metaI64}) == 1);

    InputMetadata emptyMeta;
    CHECK(space.interpretSteps(InputData{nullptr, emptyMeta}) == 1);
}

TEST_CASE("UndoableSpace computeJournalLiveBytes reports subtree payloads") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}).has_value());

    REQUIRE(space.insert("/doc/a", 1).errors.empty());
    REQUIRE(space.insert("/doc/b/c", 2).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    auto liveBytes = space.computeJournalLiveBytes(*stateIt->second);
    CHECK(liveBytes > 0);

    UndoJournalRootState missing;
    missing.components = {"missing"};
    CHECK(space.computeJournalLiveBytes(missing) == 0);
}

TEST_CASE("UndoableSpace readHistoryStatsValue handles last operation and unsupported entries") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;

    HistoryLastOperation op;
    op.type        = "insert";
    op.timestampMs = 123;
    op.success     = false;
    stats.lastOperation = op;

    HistoryUnsupportedRecord record;
    record.path            = "/bad";
    record.reason          = "unsupported";
    record.occurrences     = 2;
    record.lastTimestampMs = 99;
    stats.unsupported.recent.push_back(record);
    stats.unsupported.total = 1;

    std::string opType;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{5},
                                           std::string(UndoPaths::HistoryLastOperationType),
                                           metaStr,
                                           &opType);
    CHECK_FALSE(err.has_value());
    CHECK(opType == "insert");

    std::string recPath;
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "0",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    std::size_t totalCount = 0;
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedTotalCount),
                                      metaSize,
                                      &totalCount);
    CHECK_FALSE(err.has_value());
    CHECK(totalCount == 1);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "1/path",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{5},
                                      std::string(UndoPaths::HistoryUnsupportedRecentPrefix) + "bad/path",
                                      metaStr,
                                      &recPath);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);

    std::size_t head = 0;
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{},
                                      std::string(UndoPaths::HistoryHeadGeneration),
                                      metaSize,
                                      &head);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("UndoableSpace readHistoryStatsValue validates metadata and pointers") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;
    stats.counts.undo = 4;

    std::size_t undoCount = 0;
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{0},
                                           std::string(UndoPaths::HistoryStatsUndoCount),
                                           metaSize,
                                           &undoCount);
    CHECK_FALSE(err.has_value());
    CHECK(undoCount == 4);

    std::string wrong;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{0},
                                      std::string(UndoPaths::HistoryStatsUndoCount),
                                      metaStr,
                                      &wrong);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidType);

    err = space.readHistoryStatsValue(stats,
                                      std::optional<std::size_t>{0},
                                      std::string(UndoPaths::HistoryStatsUndoCount),
                                      metaSize,
                                      nullptr);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace readHistoryStatsValue reports missing last operation") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryStats stats;

    std::string opType;
    InputMetadata metaStr{InputMetadataT<std::string>{}};
    auto err = space.readHistoryStatsValue(stats,
                                           std::optional<std::size_t>{0},
                                           std::string(UndoPaths::HistoryLastOperationType),
                                           metaStr,
                                           &opType);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("UndoableSpace persistence setup validates tokens") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";

    state.options.persistenceNamespace = "bad/ns";
    auto badNamespace = space.ensureJournalPersistenceSetup(state);
    CHECK_FALSE(badNamespace.has_value());
    CHECK(badNamespace.error().code == Error::Code::InvalidPermissions);

    state.options.persistenceNamespace = "";
    state.encodedRoot = "bad/ns";
    auto badRoot = space.ensureJournalPersistenceSetup(state);
    CHECK_FALSE(badRoot.has_value());
    CHECK(badRoot.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace loadJournalPersistence resets on missing file") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    state.journalPath = std::filesystem::temp_directory_path()
                        / ("missing_journal_" + suffix + ".log");
    state.liveBytes = 123;
    state.telemetry.trimmedEntries = 7;
    state.telemetry.trimmedBytes = 8;
    state.telemetry.trimOperations = 2;

    auto result = space.loadJournalPersistence(state);
    CHECK(result.has_value());
    CHECK(state.liveBytes == 0);
    CHECK(state.nextSequence == 0);
    CHECK(state.telemetry.trimmedEntries == 0);
    CHECK(state.telemetry.trimmedBytes == 0);
    CHECK(state.telemetry.trimOperations == 0);
    CHECK(state.telemetry.diskEntries == 0);
}

TEST_CASE("UndoableSpace loadJournalPersistence replays valid entries") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};

    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path() / ("journal_bad_root_" + suffix);
    std::filesystem::create_directories(dir);
    state.journalPath = dir / "journal.log";

    JournalEntry entry;
    entry.path = "/doc/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
        REQUIRE(writer.flush().has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK(result.has_value());
    CHECK(state.journal.size() == 1);
    CHECK(state.journal.entryAt(0).path == "/doc/value");
}

TEST_CASE("UndoableSpace updateJournalDiskTelemetry zeros when disabled") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = false;
    state.telemetry.diskBytes = 5;
    state.telemetry.diskEntries = 4;

    space.updateJournalDiskTelemetry(state);
    CHECK(state.telemetry.diskBytes == 0);
    CHECK(state.telemetry.diskEntries == 0);
}

TEST_SUITE_END();
