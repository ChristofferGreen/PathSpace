#pragma once
#include <expected>
#include <optional>
#include <string>

namespace SP {

struct Error {
    enum class Code {
        InvalidError = 0,
        UnknownError,
        NoSuchPath,
        InvalidPath,
        InvalidPathSubcomponent,
        InvalidType,
        Timeout,
        MalformedInput,
        SerializationFunctionMissing,
        UnserializableType,
        NoObjectFound
    };

    Error(Code c, std::string m) : code(c), message(std::move(m)) {}

    Code                       code;
    std::optional<std::string> message;
};

template <typename T>
using Expected = std::expected<T, Error>;

} // namespace SP