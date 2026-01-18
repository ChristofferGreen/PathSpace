#include "third_party/doctest.h"

#include <expected>
#include <string>

using namespace std;

TEST_SUITE("core.expected_shim") {
TEST_CASE("bad_expected_access<E> thrown by expected stores payload and message") {
    std::expected<int, int> value = std::unexpected(42);
    try {
        (void)value.value();
        CHECK_FALSE(true); // should throw
    } catch (bad_expected_access<int>& ex) {
        auto msg = std::string{ex.what()};
        CHECK_FALSE(msg.empty());
        CHECK(ex.error() == 42);
    }
}

TEST_CASE("bad_expected_access<void> shim returns message") {
    // The coverage build defines PATHSPACE_NEEDS_EXPECTED_SHIM which supplies
    // a weakly-linked what() override for bad_expected_access<void>. Construct
    // a derived wrapper so we can call the protected base constructor and hit
    // the shimmed what() implementation.
    struct DerivedBadAccess : std::bad_expected_access<void> {
        DerivedBadAccess() : std::bad_expected_access<void>() {}
    };

    DerivedBadAccess ex;
    auto             message = std::string{ex.what()};
    CHECK_FALSE(message.empty());
    CHECK(message.find("expected") != std::string::npos);
}
}
