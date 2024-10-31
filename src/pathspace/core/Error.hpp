#pragma once
#include <expected>
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
        TaskFailed,
        SerializationFunctionMissing,
        UnserializableType,
        NoObjectFound,
        PopInRead,
        Shutdown
    };

    Code code;
    std::optional<std::string> message;

    Error(Code c, std::string m) : code(c), message(std::move(m)) {
    }
};

template <typename T>
using Expected = std::expected<T, Error>;

} // namespace SP