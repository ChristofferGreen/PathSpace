#pragma once
#include "core/Error.hpp"
#include "type/SlidingBuffer.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <string>
#include <vector>

namespace SP {

template <typename T>
class Serializer {
    struct Wrapper {
        T obj;
    };
    struct Header {
        uint32_t size = 0;
    };

public:
    static auto serialize(T const& obj, SlidingBuffer& buffer) -> std::optional<Error> {
        try {
            Wrapper wrapper{static_cast<T const&>(obj)};
            // Serialize to temporary buffer
            std::vector<uint8_t> tempBuffer;
            std::error_code ec;
            // alpaca::serialize<T>(obj, tempBuffer);
            size_t const bytesWritten = alpaca::serialize<Wrapper, 1>(wrapper, tempBuffer);
            if (ec)
                return Error{Error::Code::SerializationFunctionMissing, ec.message()};

            // Write header
            Header header{.size = static_cast<uint32_t>(tempBuffer.size())};
            buffer.append(reinterpret_cast<const uint8_t*>(&header), sizeof(header));

            // Write data
            buffer.append(tempBuffer.data(), tempBuffer.size());

            return std::nullopt;
        } catch (const std::exception& e) {
            return Error{Error::Code::SerializationFunctionMissing, std::string("Serialization failed: ") + e.what()};
        }
    }

    static auto deserialize(SlidingBuffer const& buffer) -> Expected<T> {
        try {
            if (buffer.remaining_size() < sizeof(Header)) {
                return std::unexpected(Error{Error::Code::MalformedInput, "Buffer too small for header"});
            }

            // Read header
            Header header;
            std::memcpy(&header, buffer.current_data(), sizeof(header));

            if (buffer.remaining_size() < sizeof(header) + header.size) {
                return std::unexpected(Error{Error::Code::MalformedInput, "Buffer too small for data"});
            }

            // Deserialize data
            std::vector<uint8_t> tempBuffer(buffer.current_data() + sizeof(header), buffer.current_data() + sizeof(header) + header.size);

            std::error_code ec;
            // T obj = alpaca::deserialize<T>(tempBuffer, ec);
            auto const wrapper = alpaca::deserialize<Wrapper, 1>(tempBuffer, ec);
            if (ec)
                return std::unexpected(Error{Error::Code::UnserializableType, ec.message()});

            return wrapper.obj;
        } catch (const std::exception& e) {
            return std::unexpected(Error{Error::Code::UnserializableType, std::string("Deserialization failed: ") + e.what()});
        }
    }

    static auto deserializePop(SlidingBuffer& buffer) -> Expected<T> {
        auto expected = deserialize(buffer);
        Header header;
        buffer.advance(sizeof(header) + header.size);
        return expected;
    }
};

} // namespace SP
