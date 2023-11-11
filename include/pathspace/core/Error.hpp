#pragma once
#include <string>
#include <optional>

namespace SP {

struct Error {
    enum class Code {
        NoSuchPath,
        InvalidPath,
        InvalidType,
        Timeout,
        CapabilityMismatch,
        CapabilityWriteMissing,
        MemoryAllocationFailed,
        MalformedInput,
        UnmatchedQuotes,
        UnknownError
    };

    Code code;
    std::optional<std::string> message;

    Error(Code c, std::string m) : code(c), message(std::move(m)) {}
};

}