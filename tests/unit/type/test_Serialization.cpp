#include "third_party/doctest.h"

#include "core/Error.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/serialization.hpp"

#include <cstdint>
#include <span>
#include <string>

using namespace SP;

namespace {
struct EmptyStruct {
    auto operator==(EmptyStruct const&) const -> bool = default;
};

struct Sample {
    int    value{0};
    double weight{0.0};
    auto operator==(Sample const&) const -> bool = default;
};
} // namespace

TEST_SUITE_BEGIN("type.serialization");

TEST_CASE("serialize handles empty structs and emits header only") {
    EmptyStruct  value{};
    SlidingBuffer buffer;

    auto err = serialize(value, buffer);
    CHECK_FALSE(err.has_value());
    CHECK(buffer.size() == sizeof(Header));

    auto decoded = deserialize<EmptyStruct>(buffer);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == value);

    auto popBuffer = buffer;
    auto popped    = deserialize_pop<EmptyStruct>(popBuffer);
    REQUIRE(popped.has_value());
    CHECK(popBuffer.size() == 0);
}

TEST_CASE("serialize/deserialize round-trip with populated payload") {
    Sample        sample{42, 3.5};
    SlidingBuffer buffer;

    auto err = serialize(sample, buffer);
    CHECK_FALSE(err.has_value());
    CHECK(buffer.size() > sizeof(Header));

    auto decoded = deserialize<Sample>(buffer);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == sample);

    auto popBuffer = buffer;
    auto popped    = deserialize_pop<Sample>(popBuffer);
    REQUIRE(popped.has_value());
    CHECK(popBuffer.size() == 0);
    CHECK(*popped == sample);
}

TEST_CASE("deserialize rejects insufficient buffers") {
    SlidingBuffer empty;
    auto          missingHeader = deserialize<int>(empty);
    CHECK_FALSE(missingHeader.has_value());
    CHECK(missingHeader.error().code == Error::Code::MalformedInput);

    SlidingBuffer truncated;
    Header        hdr{.size = 4};
    truncated.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    auto truncatedResult = deserialize<int>(truncated);
    CHECK_FALSE(truncatedResult.has_value());
    CHECK(truncatedResult.error().code == Error::Code::MalformedInput);
}

TEST_CASE("deserialize surfaces corrupt payload errors") {
    SlidingBuffer corrupt;
    Header        hdr{.size = 3};
    corrupt.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    uint8_t junk[3] = {0xAA, 0xBB, 0xCC};
    corrupt.append(junk, sizeof(junk));

    auto result = deserialize<int>(corrupt);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnserializableType);
}

TEST_CASE("deserialize accepts zero-length payloads for default-constructible types") {
    struct Zeroable {
        int value{7};
        auto operator==(Zeroable const&) const -> bool = default;
    };

    SlidingBuffer buffer;
    Header        hdr{.size = 0};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    auto decoded = deserialize<Zeroable>(buffer);
    REQUIRE(decoded.has_value());
    CHECK(decoded->value == 7);
}

TEST_CASE("deserialize zero-length payloads default construct non-empty aggregates") {
    struct NonEmpty {
        int first{1};
        int second{2};
        auto operator==(NonEmpty const&) const -> bool = default;
    };

    SlidingBuffer buffer;
    Header        hdr{.size = 0};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    auto decoded = deserialize<NonEmpty>(buffer);
    REQUIRE(decoded.has_value());
    CHECK(decoded->first == 1);
    CHECK(decoded->second == 2);
}

TEST_CASE("deserialize flags payloads shorter than expected") {
    Sample        sample{};
    SlidingBuffer buffer;

    REQUIRE_FALSE(serialize(sample, buffer).has_value());
    auto raw = buffer.rawDataMutable();
    auto* hdr = reinterpret_cast<Header*>(raw.data());
    REQUIRE(hdr->size > 0);

    // Advertise a shorter payload than the bytes we actually have so the
    // decoder rejects the truncated size.
    hdr->size -= 1;
    auto decoded = deserialize<Sample>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
    CHECK(decoded.error().message.has_value());
    CHECK(decoded.error().message->find("shorter") != std::string::npos);
}

TEST_CASE("deserialize detects payload size mismatch after successful decode") {
    Sample        sample{9, 1.5};
    SlidingBuffer buffer;
    REQUIRE_FALSE(serialize(sample, buffer).has_value());

    // Append one byte of padding and bump the advertised payload size so
    // canonical re-serialization detects the mismatch.
    uint8_t padding = 0u;
    buffer.append(&padding, 1);
    auto raw = buffer.rawDataMutable();
    auto* hdr = reinterpret_cast<Header*>(raw.data());
    hdr->size += 1;

    auto decoded = deserialize<Sample>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
    CHECK(decoded.error().message.has_value());
    CHECK(decoded.error().message->find("size mismatch") != std::string::npos);
}

TEST_CASE("deserialize rejects undersized payloads even when header present") {
    struct Compact {
        uint16_t a{1};
        uint16_t b{2};
        auto operator==(Compact const&) const -> bool = default;
    };

    // Produce a header that claims fewer bytes than the canonical encoding.
    SlidingBuffer buffer;
    Header        hdr{.size = 1}; // too small for Compact encoding
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    uint8_t payload = 0xAB;
    buffer.append(&payload, 1);

    auto decoded = deserialize<Compact>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
}

TEST_CASE("deserialize_pop advances buffer across multiple records") {
    SlidingBuffer buffer;
    int           first = 11;
    int           second = 22;

    REQUIRE_FALSE(serialize(first, buffer).has_value());
    REQUIRE_FALSE(serialize(second, buffer).has_value());

    auto firstDecoded = deserialize_pop<int>(buffer);
    REQUIRE(firstDecoded.has_value());
    CHECK(*firstDecoded == first);
    CHECK(buffer.size() > 0);

    auto secondDecoded = deserialize_pop<int>(buffer);
    REQUIRE(secondDecoded.has_value());
    CHECK(*secondDecoded == second);
    CHECK(buffer.size() == 0);
}

TEST_SUITE_END();
