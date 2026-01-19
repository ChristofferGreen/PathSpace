#include "core/Error.hpp"
#include "history/UndoJournalEntry.hpp"
#include "history/UndoSavefileCodec.hpp"

#include "third_party/doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::History;
namespace Savefile = SP::History::UndoSavefile;
namespace Journal  = SP::History::UndoJournal;

namespace {

auto bytesFromString(std::string const& text) -> std::vector<std::byte> {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

auto makeEntry(std::string path, std::string tag, Journal::OperationKind op, std::string payload)
    -> Journal::JournalEntry {
    Journal::JournalEntry entry{};
    entry.operation = op;
    entry.path      = std::move(path);
    entry.tag       = std::move(tag);
    entry.value.present = true;
    entry.value.bytes   = bytesFromString(payload);
    entry.inverseValue.present = false;
    entry.timestampMs = 1'234;
    entry.monotonicNs = 5'678;
    entry.sequence    = 0;
    entry.barrier     = (op == Journal::OperationKind::Insert);
    return entry;
}

} // namespace

TEST_SUITE_BEGIN("history.undo.savefile");

TEST_CASE("encode/decode round-trip preserves document fields") {
    Savefile::Document document{};
    document.rootPath                     = "/history/root";
    document.options.maxEntries           = 8;
    document.options.maxBytesRetained     = 4'096;
    document.options.maxDiskBytes         = 8'192;
    document.options.keepLatestForMs      = 333;
    document.options.manualGarbageCollect = true;
    document.nextSequence                 = 99;
    document.undoCount                    = 1;

    document.entries.push_back(makeEntry("/history/root/alpha", "t1", Journal::OperationKind::Insert, "payload-one"));
    document.entries.push_back(makeEntry("/history/root/beta", "t2", Journal::OperationKind::Take, "payload-two"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    REQUIRE(decoded);

    CHECK(decoded->rootPath == document.rootPath);
    CHECK(decoded->options.maxEntries == document.options.maxEntries);
    CHECK(decoded->options.maxBytesRetained == document.options.maxBytesRetained);
    CHECK(decoded->options.maxDiskBytes == document.options.maxDiskBytes);
    CHECK(decoded->options.keepLatestForMs == document.options.keepLatestForMs);
    CHECK(decoded->options.manualGarbageCollect == document.options.manualGarbageCollect);
    CHECK(decoded->nextSequence == document.nextSequence);
    CHECK(decoded->undoCount == document.undoCount);
    REQUIRE(decoded->entries.size() == document.entries.size());

    for (std::size_t idx = 0; idx < document.entries.size(); ++idx) {
        auto const& expected = document.entries[idx];
        auto const& actual   = decoded->entries[idx];
        CHECK(actual.operation == expected.operation);
        CHECK(actual.path == expected.path);
        CHECK(actual.tag == expected.tag);
        CHECK(actual.barrier == expected.barrier);
        CHECK(actual.value.present == expected.value.present);
        CHECK(actual.value.bytes == expected.value.bytes);
        CHECK(actual.inverseValue.present == expected.inverseValue.present);
        CHECK(actual.inverseValue.bytes == expected.inverseValue.bytes);
        CHECK(actual.timestampMs == expected.timestampMs);
        CHECK(actual.monotonicNs == expected.monotonicNs);
    }
}

TEST_CASE("decode rejects unexpected magic header") {
    Savefile::Document document{};
    document.rootPath         = "/root";
    document.options.maxBytesRetained = 16;
    document.entries.push_back(makeEntry("/root/value", "", Journal::OperationKind::Insert, "x"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);
    REQUIRE(!encoded->empty());
    encoded->front() = std::byte{0x00};

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when undoCount exceeds recorded entries") {
    Savefile::Document document{};
    document.rootPath                     = "/root";
    document.options.maxEntries           = 2;
    document.undoCount                    = 5; // intentionally inconsistent
    document.entries.push_back(makeEntry("/root/value", "tag", Journal::OperationKind::Insert, "abc"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::UnknownError);
}

TEST_CASE("decode detects truncated savefile payloads") {
    Savefile::Document document{};
    document.rootPath = "/root";
    document.entries.push_back(makeEntry("/root/value", "tag", Journal::OperationKind::Insert, "abc"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);
    REQUIRE(encoded->size() > 8);
    encoded->resize(encoded->size() - 4); // drop part of the last entry

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_SUITE_END();
