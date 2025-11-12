#pragma once
#include <expected>
#include <optional>
#include <string>
#include <string_view>

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
        InvalidPermissions,
        SerializationFunctionMissing,
        UnserializableType,
        NoObjectFound,
        TypeMismatch,
        NotFound,
        NotSupported,
        CapacityExceeded
    };

    Error(Code c, std::string m)
        : code(c), message(std::move(m)) {}

    Code                       code;
    std::optional<std::string> message;
};

template <typename T>
using Expected = std::expected<T, Error>;

[[nodiscard]] inline auto errorCodeToString(Error::Code code) -> std::string_view {
    switch (code) {
    case Error::Code::InvalidError:
        return "invalid_error";
    case Error::Code::UnknownError:
        return "unknown_error";
    case Error::Code::NoSuchPath:
        return "no_such_path";
    case Error::Code::InvalidPath:
        return "invalid_path";
    case Error::Code::InvalidPathSubcomponent:
        return "invalid_path_subcomponent";
    case Error::Code::InvalidType:
        return "invalid_type";
    case Error::Code::Timeout:
        return "timeout";
    case Error::Code::MalformedInput:
        return "malformed_input";
    case Error::Code::InvalidPermissions:
        return "invalid_permissions";
    case Error::Code::SerializationFunctionMissing:
        return "serialization_function_missing";
    case Error::Code::UnserializableType:
        return "unserializable_type";
    case Error::Code::NoObjectFound:
        return "no_object_found";
    case Error::Code::TypeMismatch:
        return "type_mismatch";
    case Error::Code::NotFound:
        return "not_found";
    case Error::Code::NotSupported:
        return "not_supported";
    case Error::Code::CapacityExceeded:
        return "capacity_exceeded";
    }
    return "unknown_error";
}

[[nodiscard]] inline auto describeError(Error const& error) -> std::string {
    auto const label = errorCodeToString(error.code);
    if (error.message && !error.message->empty()) {
        std::string description;
        description.reserve(label.size() + 1 + error.message->size());
        description.append(label.data(), label.size());
        description.push_back(':');
        description.append(error.message->data(), error.message->size());
        return description;
    }
    return std::string{label};
}

} // namespace SP
