#include "core/Error.hpp"

#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("core.error") {
    TEST_CASE("Error string helpers") {
        // Touch every enum value to cover the switch.
        for (auto code : {
                 Error::Code::InvalidError,
                 Error::Code::UnknownError,
                 Error::Code::NoSuchPath,
                 Error::Code::InvalidPath,
                 Error::Code::InvalidPathSubcomponent,
                 Error::Code::InvalidType,
                 Error::Code::Timeout,
                 Error::Code::MalformedInput,
                 Error::Code::InvalidPermissions,
                 Error::Code::SerializationFunctionMissing,
                 Error::Code::UnserializableType,
                 Error::Code::NoObjectFound,
                 Error::Code::TypeMismatch,
                 Error::Code::NotFound,
                 Error::Code::NotSupported,
                 Error::Code::CapacityExceeded}) {
            auto label = errorCodeToString(code);
            CHECK_FALSE(label.empty());
            // describeError should echo the label when message is absent.
            Error e{code, {}};
            CHECK(describeError(e) == std::string{label});
        }

        Error withMsg{Error::Code::InvalidPath, "bad"};
        CHECK(describeError(withMsg) == "invalid_path:bad");

        Error withoutMsg{Error::Code::NoSuchPath, {}};
        CHECK(describeError(withoutMsg) == "no_such_path");
    }
}
