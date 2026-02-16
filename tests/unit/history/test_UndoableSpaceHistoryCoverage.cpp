#define private public
#include "history/UndoableSpace.hpp"
#undef private

#include "PathSpace.hpp"
#include "core/NodeData.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "history/UndoSavefileCodec.hpp"
#include "third_party/doctest.h"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace SP;
using namespace SP::History;
namespace UndoPaths = SP::History::UndoUtils::Paths;
namespace UndoUtilsAlias = SP::History::UndoUtils;

namespace {

struct EnvVarGuard {
    explicit EnvVarGuard(std::string varName)
        : name(std::move(varName)) {
        if (auto* value = std::getenv(name.c_str())) {
            previous = std::string{value};
        }
    }

    auto set(std::string const& value) const -> bool {
#ifdef _WIN32
        return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
        return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    }

    ~EnvVarGuard() {
        if (previous) {
#ifdef _WIN32
            (void)_putenv_s(name.c_str(), previous->c_str());
#else
            (void)::setenv(name.c_str(), previous->c_str(), 1);
#endif
        } else {
#ifdef _WIN32
            (void)_putenv_s(name.c_str(), "");
#else
            (void)::unsetenv(name.c_str());
#endif
        }
    }

    std::string                    name;
    std::optional<std::string>     previous;
};

auto makeTempJournalPath(std::string_view label) -> std::filesystem::path {
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / (std::string("journal_") + std::string(label) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir / "journal.log";
}

auto makeTempSavefilePath(std::string_view label) -> std::filesystem::path {
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / (std::string("savefile_") + std::string(label) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir / "history.save";
}

auto writeSavefile(std::filesystem::path const& path,
                   UndoSavefile::Document const& document) -> void {
    auto encoded = UndoSavefile::encode(document);
    REQUIRE(encoded.has_value());
    auto span = std::span<const std::byte>(encoded->data(), encoded->size());
    auto write = UndoUtilsAlias::writeFileAtomic(path, span, false, true);
    REQUIRE(write.has_value());
}

auto makeSavefileEntry(std::string path) -> UndoJournal::JournalEntry {
    UndoJournal::JournalEntry entry;
    entry.operation = UndoJournal::OperationKind::Insert;
    entry.path = std::move(path);
    entry.sequence = 1;
    return entry;
}

} // namespace

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

TEST_CASE("UndoableSpace readDiagnosticsHistoryValue reports missing state and null output") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    InputMetadata metaU64{InputMetadataT<std::uint64_t>{}};

    UndoableSpace::MatchedJournalRoot missing{
        std::shared_ptr<UndoJournalRootState>{},
        "",
        "",
        true,
        false};

    std::uint64_t value = 0;
    auto missingErr = space.readDiagnosticsHistoryValue(
        missing,
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        metaU64,
        &value);
    REQUIRE(missingErr.has_value());
    CHECK(missingErr->code == Error::Code::UnknownError);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());

    UndoableSpace::MatchedJournalRoot withState{
        stateIt->second,
        "/doc",
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        true,
        false};

    auto nullOut = space.readDiagnosticsHistoryValue(
        withState,
        std::string(UndoPaths::HistoryDiagnosticsHeadSequence),
        metaU64,
        nullptr);
    REQUIRE(nullOut.has_value());
    CHECK(nullOut->code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace readJournalHistoryValue reports missing state") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    InputMetadata metaSize{InputMetadataT<std::size_t>{}};

    UndoableSpace::MatchedJournalRoot missing{
        std::shared_ptr<UndoJournalRootState>{},
        "",
        "",
        false,
        false};

    std::size_t value = 0;
    auto err = space.readJournalHistoryValue(
        missing,
        std::string(UndoPaths::HistoryStatsUndoCount),
        metaSize,
        &value);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace insert rejects diagnostics paths") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto result = space.insert("/diagnostics/history/_doc/entries/0/path", std::string{"bad"});
    CHECK_FALSE(result.errors.empty());
    CHECK(result.errors.front().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace diagnostics entries expose tag and timing fields") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space.insert("/doc/_history/set_tag", std::string{"paint"}).errors.empty());
    REQUIRE(space.insert("/doc/value", 1).errors.empty());

    auto tag = space.read<std::string>("/diagnostics/history/_doc/entries/0/tag");
    REQUIRE(tag.has_value());
    CHECK(*tag == "paint");

    auto timestamp = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/timestampMs");
    CHECK(timestamp.has_value());

    auto monotonic = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/monotonicNs");
    CHECK(monotonic.has_value());

    auto sequence = space.read<std::uint64_t>("/diagnostics/history/_doc/entries/0/sequence");
    CHECK(sequence.has_value());

    auto barrier = space.read<bool>("/diagnostics/history/_doc/entries/0/barrier");
    CHECK(barrier.has_value());

    auto inverseBytes = space.read<std::size_t>("/diagnostics/history/_doc/entries/0/inverseBytes");
    CHECK(inverseBytes.has_value());

    auto hasInverse = space.read<bool>("/diagnostics/history/_doc/entries/0/hasInverse");
    CHECK(hasInverse.has_value());

    auto badType = space.read<std::string>("/diagnostics/history/_doc/entries/0/timestampMs");
    CHECK_FALSE(badType.has_value());
    CHECK(badType.error().code == Error::Code::InvalidType);
}

TEST_CASE("UndoableSpace history stats surface unsupported recent records") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    space.recordJournalUnsupportedPayload(*stateIt->second, "/doc/bad", "unsupported");

    auto path = space.read<std::string>("/doc/_history/unsupported/recent/0/path");
    REQUIRE(path.has_value());
    CHECK(*path == "/doc/bad");

    auto reason = space.read<std::string>("/doc/_history/unsupported/recent/0/reason");
    REQUIRE(reason.has_value());
    CHECK(*reason == "unsupported");

    auto occurrences = space.read<std::size_t>("/doc/_history/unsupported/recent/0/occurrences");
    REQUIRE(occurrences.has_value());
    CHECK(*occurrences == 1);

    auto timestamp = space.read<std::uint64_t>("/doc/_history/unsupported/recent/0/timestampMs");
    CHECK(timestamp.has_value());
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

    state.journalPath = makeTempJournalPath("replay");

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

TEST_CASE("UndoableSpace loadJournalPersistence surfaces decode failures") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("bad_payload");

    JournalEntry entry;
    entry.path = "/doc/value";
    entry.value.present = true;

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace loadJournalPersistence rejects entries outside root") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("bad_root");

    JournalEntry entry;
    entry.path = "/other/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace loadJournalPersistence reports missing backend") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::unique_ptr<PathSpaceBase>{}, {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("no_backend");

    JournalEntry entry;
    entry.path = "/doc/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
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

TEST_CASE("UndoableSpace persistence helpers no-op when disabled") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = false;

    auto setup = space.ensureJournalPersistenceSetup(state);
    CHECK(setup.has_value());

    auto load = space.loadJournalPersistence(state);
    CHECK(load.has_value());

    auto compact = space.compactJournalPersistence(state, false);
    CHECK(compact.has_value());
}

TEST_CASE("UndoableSpace persistence root helpers honor env override") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    EnvVarGuard envGuard("PATHSPACE_HISTORY_ROOT");

    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto envRoot = std::filesystem::temp_directory_path() / ("history_env_root_" + suffix);
    REQUIRE(envGuard.set(envRoot.string()));

    CHECK(space.defaultPersistenceRoot() == (envRoot / "history"));

    auto explicitRoot = std::filesystem::temp_directory_path() / ("history_explicit_root_" + suffix);
    HistoryOptions opts;
    opts.persistenceRoot = explicitRoot.string();
    CHECK(space.persistenceRootPath(opts) == explicitRoot);
}

TEST_CASE("UndoableSpace encodeRootForPersistence replaces separators") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    CHECK(space.encodeRootForPersistence("") == "__root");
    CHECK(space.encodeRootForPersistence("/") == "__root");
    CHECK(space.encodeRootForPersistence("/doc") == "_doc");
    CHECK(space.encodeRootForPersistence("/doc/child") == "_doc_child");
}

TEST_CASE("UndoableSpace importHistorySavefile applies options and defaults ram cache") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    stateIt->second->options.ramCacheEntries = 0;

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.options.maxEntries = 12;
    document.options.maxBytesRetained = 256;
    document.options.maxDiskBytes = 512;
    document.options.keepLatestForMs = 0;
    document.options.manualGarbageCollect = true;
    document.undoCount = 0;
    document.nextSequence = 0;

    auto savefile = makeTempSavefilePath("apply_options");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, true);
    CHECK(result.has_value());
    CHECK(stateIt->second->options.ramCacheEntries == 8);
}

TEST_CASE("UndoableSpace importHistorySavefile propagates decode undo-count errors") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.entries.push_back(makeSavefileEntry("/doc/value"));
    document.undoCount = 2;

    auto savefile = makeTempSavefilePath("undo_count");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace importHistorySavefile reports missing backend on apply") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.entries.push_back(makeSavefileEntry("/doc/value"));
    document.undoCount = 1;

    auto savefile = makeTempSavefilePath("missing_backend");
    writeSavefile(savefile, document);

    space.inner.reset();
    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace importHistorySavefile propagates persistence failures") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.persistHistory = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    stateIt->second->persistenceEnabled = true;

    auto parentFile = makeTempSavefilePath("persist_parent");
    {
        std::ofstream stream(parentFile);
        REQUIRE(stream.good());
    }
    stateIt->second->journalPath = parentFile / "journal.log";

    UndoSavefile::Document document;
    document.rootPath = "/doc";

    auto savefile = makeTempSavefilePath("persist_error");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_SUITE_END();
