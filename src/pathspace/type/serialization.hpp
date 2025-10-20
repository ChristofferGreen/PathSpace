#pragma once
#include "core/Error.hpp"
#include "type/SlidingBuffer.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace SP {

template <typename T>
struct Wrapper {
    T obj;
};
struct Header {
    uint32_t size = 0;
};

namespace detail {

template <typename T>
inline auto serialize_with_alpaca(T const& obj, SlidingBuffer& buffer) -> std::optional<Error> {
    try {
        Wrapper<T> wrapper{static_cast<T const&>(obj)};
        std::vector<uint8_t> tempBuffer;
        std::error_code      ec;

        alpaca::serialize<Wrapper<T>, 1>(wrapper, tempBuffer, ec);
        if (ec) {
            return Error{Error::Code::SerializationFunctionMissing, ec.message()};
        }

        Header header{.size = static_cast<uint32_t>(tempBuffer.size())};
        buffer.append(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
        if (!tempBuffer.empty()) {
            buffer.append(tempBuffer.data(), tempBuffer.size());
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        return Error{Error::Code::SerializationFunctionMissing,
                     std::string("Serialization failed: ") + e.what()};
    }
}

template <typename T>
inline auto deserialize_with_alpaca(SlidingBuffer const& buffer)
    -> Expected<std::pair<T, size_t>> {
    try {
        if (buffer.size() < sizeof(Header)) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Buffer too small for header"});
        }

        Header header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        size_t const total_size = sizeof(header) + header.size;
        if (buffer.size() < total_size) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Buffer too small for data"});
        }

        std::vector<uint8_t> tempBuffer(buffer.data() + sizeof(header),
                                        buffer.data() + sizeof(header) + header.size);

        std::error_code ec;
        auto const      wrapper = alpaca::deserialize<Wrapper<T>, 1>(tempBuffer, ec);
        if (ec) {
            return std::unexpected(Error{Error::Code::UnserializableType, ec.message()});
        }

        return std::pair<T, size_t>{wrapper.obj, total_size};
    } catch (const std::exception& e) {
        return std::unexpected(Error{Error::Code::UnserializableType,
                                     std::string("Deserialization failed: ") + e.what()});
    }
}

} // namespace detail

template <typename T>
static auto serialize(T const& obj, SlidingBuffer& buffer) -> std::optional<Error> {
    return detail::serialize_with_alpaca<T>(obj, buffer);
}

template <typename T>
static auto deserialize(SlidingBuffer const& buffer) -> Expected<T> {
    auto decoded = detail::deserialize_with_alpaca<T>(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return std::move(decoded->first);
}

template <typename T>
static auto deserialize_pop(SlidingBuffer& buffer) -> Expected<T> {
    auto decoded = detail::deserialize_with_alpaca<T>(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    buffer.advance(decoded->second);
    return std::move(decoded->first);
}

} // namespace SP
