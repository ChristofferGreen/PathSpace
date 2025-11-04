#include "core/NodeData.hpp"
#include "core/Error.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"

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

TEST_SUITE("NodeData snapshot serialization") {
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
}
