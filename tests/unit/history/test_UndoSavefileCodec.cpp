#include "core/Error.hpp"
#include "history/UndoJournalEntry.hpp"
#include "history/UndoSavefileCodec.hpp"

#include "third_party/doctest.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
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

auto makeMinimalDocument() -> Savefile::Document {
    Savefile::Document document{};
    document.rootPath                 = "/history/root";
    document.options.maxEntries       = 8;
    document.options.maxBytesRetained = 128;
    document.options.maxDiskBytes     = 256;
    document.options.keepLatestForMs  = 10;
    document.options.manualGarbageCollect = false;
    document.nextSequence                 = 1;
    document.undoCount                    = 0;
    return document;
}

template <typename T>
void overwriteScalar(std::vector<std::byte>& buffer, std::size_t offset, T value) {
    REQUIRE(offset + sizeof(T) <= buffer.size());
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

template <typename T>
void appendScalar(std::vector<std::byte>& buffer, T value) {
    static_assert(std::is_trivially_copyable_v<T>, "appendScalar requires trivially copyable type");
    std::uint8_t local[sizeof(T)];
    std::memcpy(local, &value, sizeof(T));
    auto const* bytes = reinterpret_cast<const std::byte*>(local);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

void appendStringPayload(std::vector<std::byte>& buffer, std::string const& value) {
    appendScalar<std::uint32_t>(buffer, static_cast<std::uint32_t>(value.size()));
    auto const* bytes = reinterpret_cast<const std::byte*>(value.data());
    buffer.insert(buffer.end(), bytes, bytes + value.size());
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

TEST_CASE("decode rejects buffers smaller than magic header") {
    std::array<std::byte, 2> tiny{};
    auto                     decoded = Savefile::decode(std::span<const std::byte>(tiny));
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

TEST_CASE("decode rejects unsupported version values") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    // Version is stored immediately after the 4-byte magic.
    overwriteScalar<std::uint32_t>(*encoded, sizeof(std::uint32_t),
                                   Savefile::SavefileVersion + 1);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode detects truncated root path string") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    // Root path length is at offset 8 (after magic + version).
    auto lengthOffset = sizeof(std::uint32_t) * 2;
    std::uint32_t originalLen{};
    std::memcpy(&originalLen, encoded->data() + lengthOffset, sizeof(originalLen));
    overwriteScalar<std::uint32_t>(*encoded, lengthOffset, originalLen + 5);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when declared root path length exceeds available bytes") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    auto lengthOffset = sizeof(std::uint32_t) * 2;
    std::uint32_t rootLength{};
    std::memcpy(&rootLength, encoded->data() + lengthOffset, sizeof(rootLength));

    // Keep only the length prefix and part of the path bytes to force readBytes to fail.
    auto truncatedSize = lengthOffset + sizeof(std::uint32_t) + (rootLength / 2);
    encoded->resize(truncatedSize);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode reports truncated options block") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    // Drop the tail of the options section so manualGcFlag is missing.
    encoded->resize(encoded->size() - 2);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when entry size overstates available bytes") {
    Savefile::Document document = makeMinimalDocument();
    document.entries.push_back(makeEntry("/history/root/item", "tag", Journal::OperationKind::Insert, "payload"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);

    auto serializedEntry = Journal::serializeEntry(document.entries.front());
    REQUIRE(serializedEntry);
    auto entrySize       = static_cast<std::uint32_t>(serializedEntry->size());
    auto entrySizeOffset = encoded->size() - serializedEntry->size() - sizeof(std::uint32_t);
    overwriteScalar<std::uint32_t>(*encoded, entrySizeOffset, entrySize + 16);

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode errors when manualGcFlag byte is missing") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    auto rootLength      = static_cast<std::size_t>(document.rootPath.size());
    auto manualFlagIndex = (sizeof(std::uint32_t) * 3) // magic + version + root length
                         + rootLength
                         + (sizeof(std::uint64_t) * 4); // maxEntries, maxBytesRetained, maxDiskBytes, keepLatestForMs
    REQUIRE(encoded->size() > manualFlagIndex);
    encoded->resize(manualFlagIndex); // drop the manualGcFlag and the remainder

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode reports truncated nextSequence and undoCount fields") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    auto rootLength      = static_cast<std::size_t>(document.rootPath.size());
    auto manualFlagIndex = (sizeof(std::uint32_t) * 3) + rootLength + (sizeof(std::uint64_t) * 4);

    SUBCASE("nextSequence truncated") {
        REQUIRE(encoded->size() > manualFlagIndex + 1);
        encoded->resize(manualFlagIndex + 1); // keep manualGcFlag but truncate nextSequence

        auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
        CHECK_FALSE(decoded);
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("undoCount truncated") {
        auto nextSeqIndex = manualFlagIndex + 1;
        REQUIRE(encoded->size() > nextSeqIndex + sizeof(std::uint64_t));
        encoded->resize(nextSeqIndex + sizeof(std::uint64_t) + 2); // partial undoCount bytes

        auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
        CHECK_FALSE(decoded);
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }
}

TEST_CASE("decode fails when serialized entry payload is corrupted") {
    Savefile::Document document = makeMinimalDocument();
    document.entries.push_back(makeEntry("/history/root/item", "tag", Journal::OperationKind::Insert, "payload"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);

    auto serializedEntry = Journal::serializeEntry(document.entries.front());
    REQUIRE(serializedEntry);

    auto entryOffset = encoded->size() - serializedEntry->size();
    // Flip the entry magic to force deserializeEntry to error.
    encoded->at(entryOffset) = std::byte{0x00};

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when buffer is shorter than the magic header") {
    std::vector<std::byte> buffer;
    auto decoded = Savefile::decode(std::span<const std::byte>(buffer));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when the version header is missing") {
    std::vector<std::byte> buffer;
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileMagic);

    auto decoded = Savefile::decode(std::span<const std::byte>(buffer));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when the root path length header is truncated") {
    std::vector<std::byte> buffer;
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileMagic);
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileVersion);

    // Only two bytes of the length field are present.
    buffer.push_back(std::byte{0x01});
    buffer.push_back(std::byte{0x00});

    auto decoded = Savefile::decode(std::span<const std::byte>(buffer));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when root path bytes are missing") {
    std::vector<std::byte> buffer;
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileMagic);
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileVersion);
    appendScalar<std::uint32_t>(buffer, 5U); // claims five bytes, but none follow

    auto decoded = Savefile::decode(std::span<const std::byte>(buffer));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when options block is incomplete") {
    std::vector<std::byte> buffer;
    std::string            root = "/root";

    appendScalar<std::uint32_t>(buffer, Savefile::SavefileMagic);
    appendScalar<std::uint32_t>(buffer, Savefile::SavefileVersion);
    appendStringPayload(buffer, root);

    appendScalar<std::uint64_t>(buffer, 1);
    appendScalar<std::uint64_t>(buffer, 2);
    appendScalar<std::uint64_t>(buffer, 3);
    appendScalar<std::uint64_t>(buffer, 4);
    // manualGarbageCollect flag intentionally omitted

    auto decoded = Savefile::decode(std::span<const std::byte>(buffer));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when nextSequence field is truncated") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    std::uint32_t rootLen{};
    std::memcpy(&rootLen, encoded->data() + sizeof(std::uint32_t) * 2, sizeof(rootLen));
    auto nextSequenceOffset =
        static_cast<std::size_t>(sizeof(std::uint32_t) * 3 + rootLen + sizeof(std::uint64_t) * 4
                                 + sizeof(std::uint8_t));
    REQUIRE(encoded->size() > nextSequenceOffset);
    encoded->resize(nextSequenceOffset + 4); // drop half of nextSequence

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when undoCount field is truncated") {
    auto document = makeMinimalDocument();
    auto encoded  = Savefile::encode(document);
    REQUIRE(encoded);

    std::uint32_t rootLen{};
    std::memcpy(&rootLen, encoded->data() + sizeof(std::uint32_t) * 2, sizeof(rootLen));
    auto nextSequenceOffset =
        static_cast<std::size_t>(sizeof(std::uint32_t) * 3 + rootLen + sizeof(std::uint64_t) * 4
                                 + sizeof(std::uint8_t));
    auto undoCountOffset = nextSequenceOffset + sizeof(std::uint64_t);
    REQUIRE(encoded->size() > undoCountOffset);
    encoded->resize(undoCountOffset + 4); // partial undoCount value

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("decode fails when first entry size header is truncated") {
    Savefile::Document document = makeMinimalDocument();
    document.entries.push_back(makeEntry("/history/root/item", "tag", Journal::OperationKind::Insert, "payload"));

    auto encoded = Savefile::encode(document);
    REQUIRE(encoded);

    std::uint32_t rootLen{};
    std::memcpy(&rootLen, encoded->data() + sizeof(std::uint32_t) * 2, sizeof(rootLen));
    auto entrySizeOffset =
        static_cast<std::size_t>(sizeof(std::uint32_t) * 3 + rootLen + sizeof(std::uint64_t) * 4
                                 + sizeof(std::uint8_t) + sizeof(std::uint64_t) * 3);
    REQUIRE(encoded->size() > entrySizeOffset);
    encoded->resize(entrySizeOffset + 2); // incomplete entry size field

    auto decoded = Savefile::decode(std::span<const std::byte>(*encoded));
    CHECK_FALSE(decoded);
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_SUITE_END();
