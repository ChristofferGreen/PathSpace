#include "history/UndoJournalEntry.hpp"
#include "third_party/doctest.h"

#include <span>
#include <vector>

using namespace SP;
using namespace SP::History::UndoJournal;

TEST_SUITE_BEGIN("history.undojournal.entry");

TEST_CASE("serializeEntry round-trips journal fields including inverse payload and barrier") {
    JournalEntry entry{};
    entry.operation    = OperationKind::Take;
    entry.path         = "/alpha/beta";
    entry.tag          = "tagged";
    entry.timestampMs  = 123;
    entry.monotonicNs  = 456;
    entry.sequence     = 789;
    entry.barrier      = true;
    entry.value.present       = true;
    entry.value.bytes         = {std::byte{0x01}, std::byte{0x02}};
    entry.inverseValue.present = true;
    entry.inverseValue.bytes   = {std::byte{0x0A}, std::byte{0x0B}};

    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());

    auto decoded = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    REQUIRE(decoded.has_value());

    CHECK(decoded->operation == entry.operation);
    CHECK(decoded->path == entry.path);
    CHECK(decoded->tag == entry.tag);
    CHECK(decoded->timestampMs == entry.timestampMs);
    CHECK(decoded->monotonicNs == entry.monotonicNs);
    CHECK(decoded->sequence == entry.sequence);
    CHECK(decoded->barrier == entry.barrier);
    CHECK(decoded->value.present);
    CHECK(decoded->inverseValue.present);
    CHECK(decoded->value.bytes == entry.value.bytes);
    CHECK(decoded->inverseValue.bytes == entry.inverseValue.bytes);
}

TEST_CASE("deserializeEntry rejects payload flagged absent with bytes present") {
    JournalEntry entry{};
    entry.path                   = "/absent";
    entry.value.present          = false;
    entry.value.bytes            = {std::byte{0xFF}}; // length > 0 while present == false
    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());

    auto decoded = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("deserializeEntry rejects unknown operation kind") {
    JournalEntry entry{};
    entry.operation = OperationKind::Insert;
    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());

    // operation byte sits after magic/version: 4 + 2 bytes -> offset 6
    // followed by op (1 byte). Set to invalid > Take.
    constexpr std::size_t opOffset = 6;
    REQUIRE(opOffset < encoded->size());
    (*encoded)[opOffset] = std::byte{0xFF};

    auto decoded = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("deserializeEntry rejects bad magic, version, and truncated fields") {
    JournalEntry entry{};
    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());

    // Overwrite magic
    (*encoded)[0] = std::byte{0x00};
    auto badMagic = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    CHECK_FALSE(badMagic.has_value());
    CHECK(badMagic.error().code == Error::Code::MalformedInput);

    // Restore magic and bump version beyond supported.
    (*encoded)[0] = std::byte{0x4C}; // 'L' from PSJL little-endian stays ok for magic
    constexpr std::size_t versionOffset = 4; // after magic (uint32)
    REQUIRE(versionOffset + 1 < encoded->size());
    (*encoded)[versionOffset] = std::byte{0xFF};
    auto badVersion = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    CHECK_FALSE(badVersion.has_value());
    CHECK(badVersion.error().code == Error::Code::MalformedInput);

    // Restore header and force truncated path bytes.
    encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());
    constexpr std::size_t pathLenOffset = 4 /*magic*/ + 2 /*version*/ + 1 /*op*/ + 1 /*flags*/ + 2 /*reserved*/
                                         + 8 /*ts*/ + 8 /*mono*/ + 8 /*seq*/;
    REQUIRE(pathLenOffset + 3 < encoded->size());
    (*encoded)[pathLenOffset] = std::byte{0x05};
    (*encoded)[pathLenOffset + 1] = std::byte{0x00};
    (*encoded)[pathLenOffset + 2] = std::byte{0x00};
    (*encoded)[pathLenOffset + 3] = std::byte{0x00};
    // Drop bytes so declared length exceeds buffer
    encoded->resize(pathLenOffset + 4);
    auto truncated = deserializeEntry(std::span<const std::byte>{encoded->data(), encoded->size()});
    CHECK_FALSE(truncated.has_value());
    CHECK(truncated.error().code == Error::Code::MalformedInput);
}

TEST_CASE("encode/decode NodeData payload round-trips value queue") {
    NodeData node;
    int      value = 3;
    REQUIRE_FALSE(node.serialize(InputData{value}).has_value());
    auto payload = encodeNodeDataPayload(node);
    REQUIRE(payload.has_value());
    REQUIRE(payload->present);
    REQUIRE(!payload->bytes.empty());

    auto decoded = decodeNodeDataPayload(payload.value());
    REQUIRE(decoded.has_value());
    int out = 0;
    InputMetadata meta{InputMetadataT<int>{}};
    CHECK_FALSE(decoded->deserialize(&out, meta));
    CHECK(out == value);
}

TEST_CASE("decodeNodeDataPayload rejects absent payloads") {
    SerializedPayload absent{};
    absent.present = false;
    auto decoded   = decodeNodeDataPayload(absent);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnknownError);
}

TEST_CASE("decodeNodeDataPayload rejects malformed serialized NodeData") {
    SerializedPayload payload{};
    payload.present = true;
    payload.bytes   = {std::byte{0x00}}; // insufficient for version + headers
    auto decoded    = decodeNodeDataPayload(payload);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::MalformedInput);
}

TEST_CASE("encodeNodeDataPayload fails when NodeData is empty") {
    NodeData node; // no serializable entries
    auto encoded = encodeNodeDataPayload(node);
    CHECK_FALSE(encoded.has_value());
    CHECK(encoded.error().code == Error::Code::UnknownError);
}

TEST_SUITE_END();
