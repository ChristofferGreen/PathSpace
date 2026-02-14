#include "history/UndoJournalEntry.hpp"
#include "third_party/doctest.h"

#include <span>
#include <cstring>
#include <vector>

using namespace SP;
using namespace SP::History::UndoJournal;

namespace {

struct PayloadLayout {
    std::size_t valueOffset = 0;
    std::size_t inverseOffset = 0;
    std::size_t tagLengthOffset = 0;
    std::uint32_t valueLength = 0;
    std::uint32_t inverseLength = 0;
};

auto readU32(std::vector<std::byte> const& buffer, std::size_t offset) -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

auto computeLayout(std::vector<std::byte> const& buffer) -> PayloadLayout {
    PayloadLayout layout;
    std::size_t offset = 0;
    offset += sizeof(std::uint32_t); // magic
    offset += sizeof(std::uint16_t); // version
    offset += sizeof(std::uint8_t);  // op
    offset += sizeof(std::uint8_t);  // flags
    offset += sizeof(std::uint16_t); // reserved
    offset += sizeof(std::uint64_t) * 3; // timestamps + sequence

    auto pathLength = readU32(buffer, offset);
    offset += sizeof(std::uint32_t);
    offset += pathLength;

    layout.valueOffset = offset;
    layout.valueLength = readU32(buffer, offset + sizeof(std::uint8_t));
    offset += sizeof(std::uint8_t) + sizeof(std::uint32_t) + layout.valueLength;

    layout.inverseOffset = offset;
    layout.inverseLength = readU32(buffer, offset + sizeof(std::uint8_t));
    offset += sizeof(std::uint8_t) + sizeof(std::uint32_t) + layout.inverseLength;

    layout.tagLengthOffset = offset;
    return layout;
}

} // namespace

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

TEST_CASE("deserializeEntry rejects truncated header segments") {
    JournalEntry entry{};
    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());

    SUBCASE("missing version") {
        std::vector<std::byte> buffer;
        buffer.resize(sizeof(std::uint32_t));
        std::memcpy(buffer.data(), encoded->data(), sizeof(std::uint32_t));
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated operation fields") {
        auto buffer = *encoded;
        buffer.resize(sizeof(std::uint32_t) + sizeof(std::uint16_t));
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated metadata") {
        auto buffer = *encoded;
        buffer.resize(sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t)
                      + sizeof(std::uint8_t) + sizeof(std::uint16_t) + 4);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated path length") {
        auto buffer = *encoded;
        buffer.resize(sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t)
                      + sizeof(std::uint8_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t) * 3);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }
}

TEST_CASE("deserializeEntry rejects truncated payload segments") {
    JournalEntry entry{};
    entry.path = "/payload";
    entry.tag = "tag";
    entry.value.present = true;
    entry.value.bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    entry.inverseValue.present = true;
    entry.inverseValue.bytes = {std::byte{0x04}};

    auto encoded = serializeEntry(entry);
    REQUIRE(encoded.has_value());
    auto layout = computeLayout(*encoded);

    SUBCASE("missing payload flag") {
        auto buffer = *encoded;
        buffer.resize(layout.valueOffset);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("missing payload length") {
        auto buffer = *encoded;
        buffer.resize(layout.valueOffset + sizeof(std::uint8_t));
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated payload bytes") {
        auto buffer = *encoded;
        auto truncatedSize = layout.valueOffset + sizeof(std::uint8_t) + sizeof(std::uint32_t)
                             + static_cast<std::size_t>(layout.valueLength - 1);
        buffer.resize(truncatedSize);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated inverse payload") {
        auto buffer = *encoded;
        buffer.resize(layout.inverseOffset);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated tag length") {
        auto buffer = *encoded;
        buffer.resize(layout.tagLengthOffset + 2);
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }

    SUBCASE("truncated tag bytes") {
        auto buffer = *encoded;
        auto tagLength = readU32(buffer, layout.tagLengthOffset);
        if (tagLength > 0) {
            buffer.resize(layout.tagLengthOffset + sizeof(std::uint32_t) + tagLength - 1);
        } else {
            buffer.resize(layout.tagLengthOffset + sizeof(std::uint32_t) - 1);
        }
        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        CHECK_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::MalformedInput);
    }
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
