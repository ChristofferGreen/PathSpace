#pragma once
#include <expected>
#include <string>
#include <optional>
#include <string>

namespace SP {

struct Error {
    enum class Code {
        NoSuchPath,
        InvalidPath,
        InvalidPathSubcomponent,
        InvalidType,
        Timeout,
        CapabilityMismatch,
        CapabilityWriteMissing,
        MemoryAllocationFailed,
        MalformedInput,
        UnmatchedQuotes,
        UnknownError,
        UnserializableType
    };

    Code code;
    std::optional<std::string> message;

    Error(Code c, std::string m) : code(c), message(std::move(m)) {}
};

template<typename T>
using Expected = std::expected<T, Error>;

}