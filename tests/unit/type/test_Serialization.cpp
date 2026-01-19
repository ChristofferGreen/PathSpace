#include "third_party/doctest.h"

#include "core/Error.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/serialization.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <system_error>
#include <stdexcept>

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

// Non-default-constructible type to exercise zero-length payload handling.
struct NonDefault {
    int value;
    explicit NonDefault(int v) : value(v) {}
    auto operator==(NonDefault const&) const -> bool = default;
};

// Types that trigger the serialize/deserialize exception paths by
// providing alpaca specializations that throw at runtime.
struct ThrowOnSerialize {
    int value{0};
};

struct ThrowOnDeserialize {
    int value{0};
};

// Type that reports Alpaca mismatch when serialized size diverges.
struct ShortSerializeLongDecode {
    int value{0};
};

// Type whose alpaca deserializer returns an error_code without throwing.
struct ErrorCodeDecode {
    int value{0};
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

TEST_CASE("deserialize rejects zero-length payload for non-default-constructible type") {
    SlidingBuffer buffer;
    Header        hdr{.size = 0};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));

    auto decoded = deserialize<NonDefault>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
}

TEST_CASE("deserialize_pop mirrors zero-length rejection and leaves buffer intact") {
    SlidingBuffer buffer;
    Header        hdr{.size = 0};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    auto sizeBefore = buffer.size();

    auto decoded = deserialize_pop<NonDefault>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
    CHECK(buffer.size() == sizeBefore); // buffer should not advance on failure
}

TEST_CASE("deserialize rejects insufficient buffers") {
    SlidingBuffer empty;
    auto          missingHeader = deserialize<int>(empty);
    CHECK_FALSE(missingHeader.has_value());
    CHECK(missingHeader.error().code == Error::Code::MalformedInput);

    // Header present but claims more bytes than available.
    SlidingBuffer hugeClaim;
    Header        hugeHdr{.size = 1024};
    hugeClaim.append(reinterpret_cast<uint8_t*>(&hugeHdr), sizeof(hugeHdr));
    auto hugeResult = deserialize<int>(hugeClaim);
    CHECK_FALSE(hugeResult.has_value());
    CHECK(hugeResult.error().code == Error::Code::MalformedInput);

    SlidingBuffer truncated;
    Header        truncatedHdr{.size = 4};
    truncated.append(reinterpret_cast<uint8_t*>(&truncatedHdr), sizeof(truncatedHdr));
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

    auto popBuffer = buffer;
    auto popped    = deserialize_pop<Zeroable>(popBuffer);
    REQUIRE(popped.has_value());
    CHECK(popBuffer.size() == 0);
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

TEST_CASE("serialize/deserialize handles std::vector payloads") {
    struct VectorHolder {
        std::vector<int> values;
        auto operator==(VectorHolder const&) const -> bool = default;
    };

    VectorHolder holder{{1, 2, 3, 5, 8}};
    SlidingBuffer buffer;
    CHECK_FALSE(serialize(holder, buffer).has_value());
    CHECK(buffer.size() > sizeof(Header));

    auto decoded = deserialize<VectorHolder>(buffer);
    REQUIRE(decoded.has_value());
    CHECK(decoded->values == holder.values);
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

namespace alpaca {
template <>
inline std::size_t serialize<SP::Wrapper<NonDefault>, 1, std::vector<uint8_t>>(
    SP::Wrapper<NonDefault> const& wrapper, std::vector<uint8_t>& bytes) {
    // Minimal implementation to satisfy template instantiation; store one byte.
    bytes.push_back(static_cast<uint8_t>(wrapper.obj.value));
    return bytes.size();
}

template <>
inline auto deserialize<SP::Wrapper<NonDefault>, 1, std::vector<uint8_t>>(
    std::vector<uint8_t>& bytes, std::error_code&) -> SP::Wrapper<NonDefault> {
    int value = bytes.empty() ? 0 : static_cast<int>(bytes.front());
    return SP::Wrapper<NonDefault>{NonDefault{value}};
}

template <>
inline std::size_t serialize<SP::Wrapper<ThrowOnSerialize>, 1, std::vector<uint8_t>>(
    SP::Wrapper<ThrowOnSerialize> const&, std::vector<uint8_t>&) {
    throw std::runtime_error("boom serialize");
}

template <>
inline auto deserialize<SP::Wrapper<ThrowOnDeserialize>, 1, std::vector<uint8_t>>(
    std::vector<uint8_t>&, std::error_code&) -> SP::Wrapper<ThrowOnDeserialize> {
    throw std::runtime_error("boom deserialize");
}

template <>
inline std::size_t serialize<SP::Wrapper<ShortSerializeLongDecode>, 1, std::vector<uint8_t>>(
    SP::Wrapper<ShortSerializeLongDecode> const& wrapper, std::vector<uint8_t>& bytes) {
    // Encode only one byte even though the value is int-sized to provoke mismatch.
    bytes.push_back(static_cast<uint8_t>(wrapper.obj.value));
    return bytes.size();
}

template <>
inline auto deserialize<SP::Wrapper<ShortSerializeLongDecode>, 1, std::vector<uint8_t>>(
    std::vector<uint8_t>& bytes, std::error_code&) -> SP::Wrapper<ShortSerializeLongDecode> {
    // Pretend we consumed two bytes to force size mismatch check.
    if (bytes.size() < 2) {
        throw std::runtime_error("insufficient bytes");
    }
    int value = static_cast<int>(bytes[0]) | (static_cast<int>(bytes[1]) << 8);
    return SP::Wrapper<ShortSerializeLongDecode>{ShortSerializeLongDecode{value}};
}

template <>
inline auto deserialize<SP::Wrapper<ErrorCodeDecode>, 1, std::vector<uint8_t>>(
    std::vector<uint8_t>& bytes, std::error_code& ec) -> SP::Wrapper<ErrorCodeDecode> {
    ec = std::make_error_code(std::errc::invalid_argument);
    int value = bytes.empty() ? 0 : static_cast<int>(bytes.front());
    return SP::Wrapper<ErrorCodeDecode>{ErrorCodeDecode{value}};
}
} // namespace alpaca

TEST_CASE("serialize surfaces exceptions from alpaca") {
    SlidingBuffer buffer;
    ThrowOnSerialize value{};
    auto err = serialize(value, buffer);
    CHECK(err.has_value());
    CHECK(err->code == Error::Code::SerializationFunctionMissing);
    CHECK(err->message.has_value());
}

TEST_CASE("deserialize surfaces exceptions from alpaca") {
    // Build a buffer with a non-zero header so deserializer attempts to parse.
    SlidingBuffer buffer;
    Header        hdr{.size = 1};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    uint8_t dummy = 0u;
    buffer.append(&dummy, 1);

    auto decoded = deserialize<ThrowOnDeserialize>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
    CHECK(decoded.error().message.has_value());
}

TEST_CASE("serialize detects alpaca size mismatch after round-trip") {
    SlidingBuffer buffer;
    ShortSerializeLongDecode value{7};
    auto err = serialize(value, buffer);
    CHECK_FALSE(err.has_value());

    // decode then re-encode inside helper should detect mismatch and fail.
    auto decoded = deserialize<ShortSerializeLongDecode>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
}

TEST_CASE("deserialize propagates alpaca error_code") {
    SlidingBuffer buffer;
    Header        hdr{.size = 1};
    buffer.append(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
    uint8_t payload = 0x99;
    buffer.append(&payload, 1);

    auto decoded = deserialize<ErrorCodeDecode>(buffer);
    CHECK_FALSE(decoded.has_value());
    CHECK(decoded.error().code == Error::Code::UnserializableType);
    REQUIRE(decoded.error().message.has_value());
    CHECK_FALSE(decoded.error().message->empty());
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
