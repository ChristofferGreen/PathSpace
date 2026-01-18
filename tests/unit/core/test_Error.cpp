#include "core/Error.hpp"

#include "third_party/doctest.h"

#include <vector>

using namespace SP;

TEST_SUITE("core.error") {
    TEST_CASE("Error string helpers") {
        // Touch every enum value using a runtime loop so the compiler cannot
        // constant-fold the switch and skip instrumentation.
        std::vector<Error::Code> codes;
        for (int i = static_cast<int>(Error::Code::InvalidError);
             i <= static_cast<int>(Error::Code::CapacityExceeded);
             ++i) {
            codes.push_back(static_cast<Error::Code>(i));
        }

        for (auto code : codes) {
            auto runtimeCode = code;
            auto label       = errorCodeToString(runtimeCode);
            CHECK_FALSE(label.empty());
            // describeError should echo the label when message is absent.
            Error e{runtimeCode, {}};
            CHECK(describeError(e) == std::string{label});
        }

        Error withMsg{Error::Code::InvalidPath, "bad"};
        CHECK(describeError(withMsg) == "invalid_path:bad");

        Error withoutMsg{Error::Code::NoSuchPath, {}};
        CHECK(describeError(withoutMsg) == "no_such_path");

        // Unknown enum value should still return fallback label.
        auto unknownLabel = errorCodeToString(static_cast<Error::Code>(999));
        CHECK(unknownLabel == "unknown_error");
        Error synthetic{static_cast<Error::Code>(999), {}};
        CHECK(describeError(synthetic) == "unknown_error");
    }
}
