#include "core/NodeData.hpp"
#include "core/Error.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"
#include "task/Task.hpp"

#include "third_party/doctest.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace SP;
using doctest::Approx;

namespace {
inline auto metadataForInt() -> InputMetadata {
    return InputMetadata{InputMetadataT<int>{}};
}

inline auto metadataForDouble() -> InputMetadata {
    return InputMetadata{InputMetadataT<double>{}};
}

inline auto metadataForString() -> InputMetadata {
    return InputMetadata{InputMetadataT<std::string>{}};
}
} // namespace

TEST_SUITE("core.nodedata.snapshot") {
    TEST_CASE("round-trips single value") {
        NodeData node;
        int      value = 42;
        CHECK_FALSE(node.serialize(InputData(value)).has_value());

        auto bytes = node.serializeSnapshot();
        REQUIRE(bytes.has_value());

        auto restoredOpt = NodeData::deserializeSnapshot(std::span<const std::byte>{bytes->data(), bytes->size()});
        REQUIRE(restoredOpt.has_value());
        auto restored = std::move(restoredOpt.value());

        int out = 0;
        CHECK_FALSE(restored.deserialize(&out, metadataForInt()).has_value());
        CHECK(out == value);
    }

    TEST_CASE("round-trips multiple queued values") {
        NodeData node;
        int      first = 7;
        double   second = 3.14159;
        std::string third = "snapshot";

        CHECK_FALSE(node.serialize(InputData(first)).has_value());
        CHECK_FALSE(node.serialize(InputData(second)).has_value());
        CHECK_FALSE(node.serialize(InputData(third)).has_value());

        auto bytes = node.serializeSnapshot();
        REQUIRE(bytes.has_value());

        auto restoredOpt = NodeData::deserializeSnapshot(std::span<const std::byte>{bytes->data(), bytes->size()});
        REQUIRE(restoredOpt.has_value());
        auto restored = std::move(restoredOpt.value());

        int storedFirst = 0;
        CHECK_FALSE(restored.deserializePop(&storedFirst, metadataForInt()).has_value());
        CHECK(storedFirst == first);

        double storedSecond = 0.0;
        CHECK_FALSE(restored.deserializePop(&storedSecond, metadataForDouble()).has_value());
        CHECK(storedSecond == Approx(second));

        std::string storedThird;
        CHECK_FALSE(restored.deserializePop(&storedThird, metadataForString()).has_value());
        CHECK(storedThird == third);
    }

    TEST_CASE("deserialize rejects corrupted payload") {
        NodeData node;
        int      value = 13;
        CHECK_FALSE(node.serialize(InputData(value)).has_value());
        auto bytes = node.serializeSnapshot();
        REQUIRE(bytes.has_value());

        auto corrupted = *bytes;
        REQUIRE(corrupted.size() >= sizeof(std::uint32_t));
        // Flip the version field so deserialization fails
        std::uint32_t badVersion = 999u;
        std::memcpy(corrupted.data(), &badVersion, sizeof(std::uint32_t));

        auto restoredOpt = NodeData::deserializeSnapshot(std::span<const std::byte>{corrupted.data(), corrupted.size()});
        CHECK_FALSE(restoredOpt.has_value());
    }

    TEST_CASE("popFrontSerialized extracts front value") {
        NodeData node;
        int      first  = 21;
        std::string second = "next";

        CHECK_FALSE(node.serialize(InputData(first)).has_value());
        CHECK_FALSE(node.serialize(InputData(second)).has_value());

        NodeData extracted;
        auto     error = node.popFrontSerialized(extracted);
        CHECK_FALSE(error.has_value());

        auto snapshot = extracted.serializeSnapshot();
        REQUIRE(snapshot.has_value());
        auto restored = NodeData::deserializeSnapshot(
            std::span<const std::byte>(snapshot->data(), snapshot->size()));
        REQUIRE(restored.has_value());

        int decoded = 0;
        CHECK_FALSE(restored->deserialize(&decoded, metadataForInt()).has_value());
        CHECK(decoded == first);

        std::string remaining;
        CHECK_FALSE(node.deserializePop(&remaining, metadataForString()).has_value());
        CHECK(remaining == second);
    }

    TEST_CASE("popFrontSerialized survives snapshot round trip") {
        NodeData node;
        double   value = 99.5;
        CHECK_FALSE(node.serialize(InputData(value)).has_value());

        auto snapshot = node.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restored = NodeData::deserializeSnapshot(
            std::span<const std::byte>(snapshot->data(), snapshot->size()));
        REQUIRE(restored.has_value());

        NodeData extracted;
        auto     error = restored->popFrontSerialized(extracted);
        CHECK_FALSE(error.has_value());

        auto encoded = extracted.serializeSnapshot();
        REQUIRE(encoded.has_value());
        auto decoded = NodeData::deserializeSnapshot(
            std::span<const std::byte>(encoded->data(), encoded->size()));
        REQUIRE(decoded.has_value());

        double roundtrip = 0;
        CHECK_FALSE(decoded->deserialize(&roundtrip, metadataForDouble()).has_value());
        CHECK(roundtrip == Approx(value));
    }

    TEST_CASE("serializeSnapshot returns nullopt for execution-only payloads") {
        NodeData node;
        auto task = Task::Create([](Task&, bool) {});
        InputData input{task};
        input.task     = task;
        input.executor = nullptr;
        InputMetadata meta{};
        meta.typeInfo     = &typeid(Task);
        meta.serialize    = nullptr;
        meta.deserialize  = nullptr;
        meta.dataCategory = DataCategory::Execution;
        input.metadata    = meta;
        CHECK_FALSE(node.serialize(input).has_value());

        auto snapshot = node.serializeSnapshot();
        CHECK_FALSE(snapshot.has_value());
    }

    TEST_CASE("serializeSnapshot filters execution payloads but keeps values") {
        NodeData node;
        int      value = 7;
        CHECK_FALSE(node.serialize(InputData(value)).has_value());

        auto task = Task::Create([](Task&, bool) {});
        InputData input{task};
        input.task     = task;
        input.executor = nullptr;
        InputMetadata meta{};
        meta.typeInfo     = &typeid(Task);
        meta.serialize    = nullptr;
        meta.deserialize  = nullptr;
        meta.dataCategory = DataCategory::Execution;
        input.metadata    = meta;
        CHECK_FALSE(node.serialize(input).has_value());

        auto snapshot = node.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restoredOpt =
            NodeData::deserializeSnapshot(std::span<const std::byte>{snapshot->data(), snapshot->size()});
        REQUIRE(restoredOpt.has_value());
        auto restored = std::move(*restoredOpt);

        int out = 0;
        CHECK_FALSE(restored.deserialize(&out, metadataForInt()).has_value());
        CHECK(out == value);
    }
}
