#include "history/UndoableSpace.hpp"
#include "history/UndoSavefileCodec.hpp"

#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace SP;
using namespace SP::History;
using SP::ConcretePathStringView;
namespace Savefile = SP::History::UndoSavefile;
namespace Journal  = SP::History::UndoJournal;

namespace {

auto makeUndoableSpace(HistoryOptions opts = {}) -> std::unique_ptr<UndoableSpace> {
    auto inner = std::make_unique<PathSpace>();
    return std::make_unique<UndoableSpace>(std::move(inner), std::move(opts));
}

auto tempFile(std::string_view stem) -> std::filesystem::path {
    auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = std::string(stem) + "_" + std::to_string(::getpid()) + "_" + std::to_string(now);
    return std::filesystem::temp_directory_path() / name;
}

void writeBytes(std::filesystem::path const& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<char const*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

auto makeInvalidPayloadEntry(std::string path) -> Journal::JournalEntry {
    Journal::JournalEntry entry{};
    entry.operation          = Journal::OperationKind::Insert;
    entry.path               = std::move(path);
    entry.value.present      = true;
    entry.value.bytes        = {std::byte{0xBA}, std::byte{0xDD}};
    entry.inverseValue.present = false;
    entry.sequence           = 0;
    return entry;
}

auto makeDocumentWithEntries(std::string const& root,
                             std::vector<Journal::JournalEntry> entries,
                             std::size_t undoCount = 0) -> Savefile::Document {
    Savefile::Document doc;
    doc.rootPath   = root;
    doc.undoCount  = undoCount;
    doc.entries    = std::move(entries);
    return doc;
}

} // namespace

TEST_SUITE_BEGIN("history.undoable.coverage");

TEST_CASE("unsupported history records default fields") {
    HistoryTelemetry::UnsupportedRecord telemetry{};
    CHECK(telemetry.path.empty());
    CHECK(telemetry.reason.empty());
    CHECK(telemetry.occurrences == 0);

    HistoryUnsupportedRecord stats{};
    CHECK(stats.path.empty());
    CHECK(stats.reason.empty());
    CHECK(stats.occurrences == 0);
    CHECK(stats.lastTimestampMs == 0);
}

TEST_CASE("savefile defaults are initialized") {
    Savefile::OptionsBlock opts{};
    CHECK(opts.maxEntries == 0);
    CHECK(opts.maxBytesRetained == 0);
    CHECK(opts.maxDiskBytes == 0);
    CHECK(opts.keepLatestForMs == 0);
    CHECK_FALSE(opts.manualGarbageCollect);

    Savefile::Document doc{};
    CHECK(doc.rootPath.empty());
    CHECK(doc.options.maxEntries == 0);
    CHECK(doc.nextSequence == 0);
    CHECK(doc.undoCount == 0);
    CHECK(doc.entries.empty());
}

TEST_CASE("HistoryOptions defaults are populated") {
    HistoryOptions opts{};
    CHECK(opts.maxEntries == 128);
    CHECK(opts.maxBytesRetained == 0);
    CHECK_FALSE(opts.manualGarbageCollect);
    CHECK_FALSE(opts.allowNestedUndo);
    CHECK_FALSE(opts.useMutationJournal);
    CHECK_FALSE(opts.persistHistory);
    CHECK(opts.persistenceRoot.empty());
    CHECK(opts.persistenceNamespace.empty());
    CHECK(opts.ramCacheEntries == 8);
    CHECK(opts.maxDiskBytes == 0);
    CHECK(opts.keepLatestFor.count() == 0);
    CHECK(opts.restoreFromPersistence);
    CHECK_FALSE(opts.sharedStackKey.has_value());
}

TEST_CASE("exportHistorySavefile reports missing history root") {
    auto space = makeUndoableSpace();
    auto path  = tempFile("export_missing.bin");

    auto result = space->exportHistorySavefile(ConcretePathStringView{"/missing"}, path, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotFound);
}

TEST_CASE("exportHistorySavefile blocks active transaction") {
    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto txExpected = space->beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto path   = tempFile("export_tx.bin");
    auto exportResult = space->exportHistorySavefile(ConcretePathStringView{"/doc"}, path, false);
    CHECK_FALSE(exportResult.has_value());
    CHECK(exportResult.error().code == Error::Code::InvalidPermissions);

    CHECK(tx.commit().has_value());
}

TEST_CASE("importHistorySavefile reports missing history root") {
    Savefile::Document doc;
    doc.rootPath = "/doc";
    auto encoded = Savefile::encode(doc);
    REQUIRE(encoded);

    auto path = tempFile("import_missing.bin");
    writeBytes(path, std::span<const std::byte>(*encoded));

    auto space  = makeUndoableSpace();
    auto result = space->importHistorySavefile(ConcretePathStringView{"/doc"}, path, true);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotFound);
}

TEST_CASE("importHistorySavefile rejects root mismatch") {
    auto source = makeUndoableSpace();
    REQUIRE(source->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(source->insert("/doc/value", 7).errors.empty());

    auto savePath = tempFile("import_root_mismatch.bin");
    REQUIRE(source->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false).has_value());

    auto destination = makeUndoableSpace();
    REQUIRE(destination->enableHistory(ConcretePathStringView{"/other"}).has_value());

    auto importResult =
        destination->importHistorySavefile(ConcretePathStringView{"/other"}, savePath, true);
    CHECK_FALSE(importResult.has_value());
    CHECK(importResult.error().code == Error::Code::InvalidPath);
}

TEST_CASE("importHistorySavefile blocks active transaction") {
    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space->insert("/doc/value", 1).errors.empty());

    auto savePath = tempFile("import_tx_locked.bin");
    REQUIRE(space->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false).has_value());

    auto txExpected = space->beginTransaction(ConcretePathStringView{"/doc"});
    REQUIRE(txExpected.has_value());
    auto tx = std::move(txExpected.value());

    auto importResult = space->importHistorySavefile(ConcretePathStringView{"/doc"}, savePath, true);
    CHECK_FALSE(importResult.has_value());
    CHECK(importResult.error().code == Error::Code::InvalidPermissions);

    CHECK(tx.commit().has_value());
}

TEST_CASE("importHistorySavefile rejects entries outside root") {
    auto entry = Journal::JournalEntry{};
    entry.operation          = Journal::OperationKind::Insert;
    entry.path               = "/other/value";
    entry.value.present      = false;
    entry.inverseValue.present = false;
    entry.sequence           = 0;

    auto doc     = makeDocumentWithEntries("/doc", {entry}, 1);
    auto encoded = Savefile::encode(doc);
    REQUIRE(encoded);

    auto path = tempFile("import_outside_root.bin");
    writeBytes(path, std::span<const std::byte>(*encoded));

    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto result = space->importHistorySavefile(ConcretePathStringView{"/doc"}, path, true);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("importHistorySavefile rejects malformed payloads") {
    auto entry = makeInvalidPayloadEntry("/doc/value");
    auto doc   = makeDocumentWithEntries("/doc", {entry}, 1);
    auto encoded = Savefile::encode(doc);
    REQUIRE(encoded);

    auto path = tempFile("import_bad_payload.bin");
    writeBytes(path, std::span<const std::byte>(*encoded));

    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto result = space->importHistorySavefile(ConcretePathStringView{"/doc"}, path, true);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::MalformedInput);
}

TEST_CASE("importHistorySavefile defaults ram cache when zero") {
    Savefile::Document doc;
    doc.rootPath  = "/doc";
    doc.undoCount = 0;
    auto encoded  = Savefile::encode(doc);
    REQUIRE(encoded);

    auto path = tempFile("import_ram_cache.bin");
    writeBytes(path, std::span<const std::byte>(*encoded));

    HistoryOptions opts;
    opts.ramCacheEntries = 0;
    auto space = makeUndoableSpace(opts);
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto result = space->importHistorySavefile(ConcretePathStringView{"/doc"}, path, true);
    REQUIRE(result.has_value());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->limits.ramCacheEntries == 8);
}

TEST_CASE("disableHistory reports missing history root") {
    auto space = makeUndoableSpace();
    auto result = space->disableHistory(ConcretePathStringView{"/missing"});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotFound);
}

TEST_CASE("disableHistory removes persistence directory when enabled") {
    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.persistHistory       = true;
    opts.persistenceNamespace = "disable_ns";
    auto persistenceRoot      = tempFile("disable_persist_root");
    std::filesystem::create_directories(persistenceRoot);
    opts.persistenceRoot = persistenceRoot.string();

    auto space = makeUndoableSpace(opts);
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto persistencePath = persistenceRoot / "disable_ns" / "_doc";
    CHECK(std::filesystem::exists(persistencePath));

    REQUIRE(space->disableHistory(ConcretePathStringView{"/doc"}).has_value());
    CHECK_FALSE(std::filesystem::exists(persistencePath));

    std::error_code ec;
    std::filesystem::remove_all(persistenceRoot, ec);
}

TEST_CASE("importHistorySavefile preserves options when applyOptions is false") {
    HistoryOptions sourceOpts;
    sourceOpts.useMutationJournal = true;
    auto source = makeUndoableSpace(sourceOpts);
    REQUIRE(source->enableHistory(ConcretePathStringView{"/doc"}, sourceOpts).has_value());
    REQUIRE(source->insert("/doc/value", 101).errors.empty());

    auto savePath = tempFile("import_keep_options.bin");
    REQUIRE(source->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false).has_value());

    HistoryOptions destOpts;
    destOpts.useMutationJournal   = true;
    destOpts.maxEntries           = 5;
    destOpts.maxBytesRetained     = 123;
    destOpts.maxDiskBytes         = 456;
    destOpts.keepLatestFor        = std::chrono::milliseconds{789};
    destOpts.manualGarbageCollect = true;
    destOpts.ramCacheEntries      = 3;

    auto destination = makeUndoableSpace(destOpts);
    REQUIRE(destination->enableHistory(ConcretePathStringView{"/doc"}, destOpts).has_value());

    auto importResult =
        destination->importHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false);
    REQUIRE(importResult.has_value());

    auto stats = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->limits.maxEntries == destOpts.maxEntries);
    CHECK(stats->limits.maxBytesRetained == destOpts.maxBytesRetained);
    CHECK(stats->limits.maxDiskBytes == destOpts.maxDiskBytes);
    CHECK(stats->limits.keepLatestForMs == destOpts.keepLatestFor.count());
    CHECK(stats->limits.ramCacheEntries == destOpts.ramCacheEntries);
    CHECK(stats->counts.manualGarbageCollect == destOpts.manualGarbageCollect);
    CHECK(stats->counts.undo >= 1);
}

TEST_CASE("importHistorySavefile writes persistence journal when enabled") {
    HistoryOptions sourceOpts;
    sourceOpts.useMutationJournal = true;
    auto source = makeUndoableSpace(sourceOpts);
    REQUIRE(source->enableHistory(ConcretePathStringView{"/doc"}, sourceOpts).has_value());
    REQUIRE(source->insert("/doc/value", std::string{"alpha"}).errors.empty());
    REQUIRE(source->insert("/doc/value", std::string{"beta"}).errors.empty());

    auto savePath = tempFile("import_persist.bin");
    REQUIRE(source->exportHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false).has_value());

    auto persistenceRoot = tempFile("history_persist_root");
    std::filesystem::create_directories(persistenceRoot);

    HistoryOptions destOpts;
    destOpts.useMutationJournal   = true;
    destOpts.persistHistory       = true;
    destOpts.persistenceRoot      = persistenceRoot.string();
    destOpts.persistenceNamespace = "persist_test";

    auto destination = makeUndoableSpace(destOpts);
    REQUIRE(destination->enableHistory(ConcretePathStringView{"/doc"}, destOpts).has_value());

    auto importResult =
        destination->importHistorySavefile(ConcretePathStringView{"/doc"}, savePath, false);
    REQUIRE(importResult.has_value());

    auto stats = destination->getHistoryStats(ConcretePathStringView{"/doc"});
    REQUIRE(stats.has_value());
    CHECK(stats->bytes.disk > 0);
    CHECK(stats->counts.diskEntries > 0);

    std::error_code ec;
    std::filesystem::remove_all(persistenceRoot, ec);
}

TEST_CASE("unknown history control command reports error") {
    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());

    auto insertResult = space->insert("/doc/_history/not_a_command", true);
    CHECK_FALSE(insertResult.errors.empty());
    CHECK(insertResult.errors.front().code == Error::Code::UnknownError);
}

TEST_CASE("history telemetry rejects mismatched types and missing indices") {
    auto space = makeUndoableSpace();
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}).has_value());
    REQUIRE(space->insert("/doc/value", std::string{"alpha"}).errors.empty());

    auto wrongType = space->read<std::string>("/doc/_history/stats/undoCount");
    CHECK_FALSE(wrongType.has_value());
    CHECK(wrongType.error().code == Error::Code::InvalidType);

    auto unsupported = space->read<std::size_t>("/doc/_history/unsupported/recent/0/path");
    CHECK_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("diagnostics history entry out of range surfaces error") {
    auto space = makeUndoableSpace();
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());
    REQUIRE(space->insert("/doc/value", std::string{"alpha"}).errors.empty());

    auto missing = space->read<std::string>("/diagnostics/history/_doc/entries/999/path");
    CHECK_FALSE(missing.has_value());
    CHECK(missing.error().code == Error::Code::NoObjectFound);
}

TEST_SUITE_END();
