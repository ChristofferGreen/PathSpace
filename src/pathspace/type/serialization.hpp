#pragma once
#include "core/Error.hpp"
#include "layer/PathSpaceTrellisTypes.hpp"
#include "type/SlidingBuffer.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace SP {
struct TrellisTraceSnapshot;

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
        (void)alpaca::serialize<Wrapper<T>, 1>(wrapper, tempBuffer);
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

// ----- TrellisTraceSnapshot specialization -----

template <>
inline auto serialize<TrellisTraceSnapshot>(TrellisTraceSnapshot const& snapshot, SlidingBuffer& buffer)
    -> std::optional<Error> {
    auto append_scalar = [&](auto value) {
        using Scalar = decltype(value);
        uint8_t bytes[sizeof(Scalar)];
        std::memcpy(bytes, &value, sizeof(Scalar));
        buffer.append(bytes, sizeof(Scalar));
    };

    if (snapshot.events.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Error{Error::Code::MalformedInput, "Trace event count exceeds uint32_t capacity"};
    }

    append_scalar(static_cast<std::uint32_t>(snapshot.events.size()));
    for (auto const& event : snapshot.events) {
        append_scalar(static_cast<std::uint64_t>(event.timestampNs));

        if (event.message.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Error{Error::Code::MalformedInput, "Trace message exceeds uint32_t capacity"};
        }
        auto length = static_cast<std::uint32_t>(event.message.size());
        append_scalar(length);
        if (length > 0) {
            buffer.append(reinterpret_cast<uint8_t const*>(event.message.data()), length);
        }
    }
    return std::nullopt;
}

namespace detail {

inline auto deserializeTrellisTraceSnapshot(uint8_t const* data, size_t size)
    -> Expected<std::pair<TrellisTraceSnapshot, size_t>> {
    auto remaining = size;
    auto cursor    = data;

    auto require = [&](size_t needed) -> bool { return remaining >= needed; };
    auto read_scalar = [&](auto& out) -> bool {
        using Scalar = std::remove_reference_t<decltype(out)>;
        if (!require(sizeof(Scalar)))
            return false;
        std::memcpy(&out, cursor, sizeof(Scalar));
        cursor += sizeof(Scalar);
        remaining -= sizeof(Scalar);
        return true;
    };

    std::uint32_t count = 0;
    if (!read_scalar(count)) {
        return std::unexpected(Error{Error::Code::MalformedInput, "Trace snapshot missing event count"});
    }

    TrellisTraceSnapshot snapshot;
    snapshot.events.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t timestamp = 0;
        std::uint32_t length    = 0;
        if (!read_scalar(timestamp)) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Trace snapshot missing timestamp"});
        }
        if (!read_scalar(length)) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Trace snapshot missing message length"});
        }
        if (!require(length)) {
            return std::unexpected(Error{Error::Code::MalformedInput, "Trace snapshot truncated message bytes"});
        }
        TrellisTraceEvent event;
        event.timestampNs = timestamp;
        if (length > 0) {
            event.message.assign(reinterpret_cast<char const*>(cursor), length);
            cursor += length;
            remaining -= length;
        } else {
            event.message.clear();
        }
        snapshot.events.push_back(std::move(event));
    }

    size_t consumed = size - remaining;
    return std::pair<TrellisTraceSnapshot, size_t>{std::move(snapshot), consumed};
}

} // namespace detail

template <>
inline auto deserialize<TrellisTraceSnapshot>(SlidingBuffer const& buffer) -> Expected<TrellisTraceSnapshot> {
    auto decoded =
        detail::deserializeTrellisTraceSnapshot(buffer.data(), buffer.size());
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return std::move(decoded->first);
}

template <>
inline auto deserialize_pop<TrellisTraceSnapshot>(SlidingBuffer& buffer) -> Expected<TrellisTraceSnapshot> {
    auto decoded =
        detail::deserializeTrellisTraceSnapshot(buffer.data(), buffer.size());
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    buffer.advance(decoded->second);
    return std::move(decoded->first);
}

} // namespace SP
