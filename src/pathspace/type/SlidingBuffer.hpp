#pragma once
#include "core/Error.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <string>
#include <vector>

namespace SP {

class SlidingBuffer {
public:
    std::vector<uint8_t> data;
    size_t virtualFront = 0;

    [[nodiscard]] const uint8_t* current_data() const {
        return data.data() + virtualFront;
    }

    [[nodiscard]] size_t remaining_size() const {
        return data.size() - virtualFront;
    }

    void append(const uint8_t* bytes, size_t count) {
        data.insert(data.end(), bytes, bytes + count);
    }

    void advance(size_t bytes) {
        virtualFront += bytes;
        if (virtualFront > data.size() / 2) {
            compact();
        }
    }

private:
    void compact() {
        if (virtualFront == 0)
            return;

        if (virtualFront < data.size()) {
            std::memmove(data.data(), data.data() + virtualFront, data.size() - virtualFront);
            data.resize(data.size() - virtualFront);
        } else {
            data.clear();
        }
        virtualFront = 0;
    }
};

template <typename T>
class Serializer {
    struct Header {
        uint32_t size = 0;
    };

public:
    static std::optional<Error> serialize(const T& obj, SlidingBuffer& buffer) {
        try {
            // Serialize to temporary buffer
            std::vector<uint8_t> tempBuffer;
            std::error_code ec;
            alpaca::serialize<T>(obj, tempBuffer);
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

    static Expected<T> deserialize(SlidingBuffer& buffer) {
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
            T obj = alpaca::deserialize<T>(tempBuffer, ec);
            if (ec) {
                return std::unexpected(Error{Error::Code::UnserializableType, ec.message()});
            }

            buffer.advance(sizeof(header) + header.size);
            return obj;
        } catch (const std::exception& e) {
            return std::unexpected(Error{Error::Code::UnserializableType, std::string("Deserialization failed: ") + e.what()});
        }
    }
};

} // namespace SP
