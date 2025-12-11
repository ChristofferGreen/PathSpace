#include "history/UndoJournalEntry.hpp"

#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"

#include "third_party/doctest.h"

#include <span>
#include <string>
#include <vector>
#include <cstring>

using namespace SP;
using namespace SP::History::UndoJournal;

namespace {
[[nodiscard]] auto metadataForInt() -> InputMetadata {
    return InputMetadata{InputMetadataT<int>{}};
}
} // namespace

TEST_SUITE("UndoJournalEntry") {
    TEST_CASE("NodeData payload round-trips through journal helpers") {
        NodeData node;
        int      value = 123;

        REQUIRE_FALSE(node.serialize(InputData(value)).has_value());

        auto payloadExpected = encodeNodeDataPayload(node);
        REQUIRE(payloadExpected.has_value());

        auto restoredExpected = decodeNodeDataPayload(payloadExpected.value());
        REQUIRE(restoredExpected.has_value());

        int restored = 0;
        REQUIRE_FALSE(restoredExpected->deserialize(&restored, metadataForInt()).has_value());
        CHECK(restored == value);
    }

    TEST_CASE("Journal entry binary encoding round-trips all fields") {
        NodeData insertedNode;
        NodeData previousNode;

        int insertedValue = 7;
        int previousValue = 5;
        REQUIRE_FALSE(insertedNode.serialize(InputData(insertedValue)).has_value());
        REQUIRE_FALSE(previousNode.serialize(InputData(previousValue)).has_value());

        auto insertedPayloadExpected = encodeNodeDataPayload(insertedNode);
        auto previousPayloadExpected = encodeNodeDataPayload(previousNode);
        REQUIRE(insertedPayloadExpected.has_value());
        REQUIRE(previousPayloadExpected.has_value());

        JournalEntry entry;
        entry.operation    = OperationKind::Insert;
        entry.path         = "/doc/value";
        entry.tag          = "stroke";
        entry.value        = insertedPayloadExpected.value();
        entry.inverseValue = previousPayloadExpected.value();
        entry.timestampMs  = 123456789u;
        entry.monotonicNs  = 555u;
        entry.sequence     = 42u;
        entry.barrier      = true;

        auto encodedExpected = serializeEntry(entry);
        REQUIRE(encodedExpected.has_value());
        auto encodedBytes = std::move(encodedExpected.value());

        auto decodedExpected = deserializeEntry(std::span<const std::byte>{encodedBytes.data(),
                                                                           encodedBytes.size()});
        REQUIRE(decodedExpected.has_value());
        auto decoded = std::move(decodedExpected.value());

        CHECK(decoded.operation == entry.operation);
        CHECK(decoded.path == entry.path);
        CHECK(decoded.tag == entry.tag);
        CHECK(decoded.timestampMs == entry.timestampMs);
        CHECK(decoded.monotonicNs == entry.monotonicNs);
        CHECK(decoded.sequence == entry.sequence);
        CHECK(decoded.barrier == entry.barrier);
        CHECK(decoded.value.present == entry.value.present);
        CHECK(decoded.value.bytes == entry.value.bytes);
        CHECK(decoded.inverseValue.present == entry.inverseValue.present);
        CHECK(decoded.inverseValue.bytes == entry.inverseValue.bytes);

        auto payloadDecodedNode = decodeNodeDataPayload(decoded.value);
        REQUIRE(payloadDecodedNode.has_value());

        int storedInserted = 0;
        REQUIRE_FALSE(payloadDecodedNode->deserialize(&storedInserted, metadataForInt()).has_value());
        CHECK(storedInserted == insertedValue);
    }

    TEST_CASE("Journal entry decoder accepts legacy v1 payloads without tags") {
        JournalEntry entry;
        entry.operation   = OperationKind::Insert;
        entry.path        = "/legacy";
        entry.timestampMs = 1u;
        entry.sequence    = 2u;
        entry.barrier     = false;

        auto encodedExpected = serializeEntry(entry);
        REQUIRE(encodedExpected);
        auto encoded = std::move(encodedExpected.value());

        // Strip the tag footer and downgrade the version to mimic a v1 payload.
        if (encoded.size() >= sizeof(std::uint32_t)) {
            encoded.resize(encoded.size() - sizeof(std::uint32_t));
        }
        std::uint16_t legacyVersion = 1;
        std::memcpy(encoded.data() + sizeof(std::uint32_t), &legacyVersion, sizeof(legacyVersion));

        auto decodedExpected = deserializeEntry(std::span<const std::byte>{encoded.data(), encoded.size()});
        REQUIRE(decodedExpected);
        auto decoded = std::move(decodedExpected.value());

        CHECK(decoded.tag.empty());
        CHECK(decoded.path == entry.path);
        CHECK(decoded.operation == entry.operation);
    }
}
