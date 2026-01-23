#include "third_party/doctest.h"

#include <expected>
#include <string>

namespace SP::testing {
const char* callExpectedShimWhat();
}

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

TEST_CASE("expected<void> throws and surfaces shimmed what()") {
    std::expected<void, int> value = std::unexpected(7);
    try {
        value.value(); // force throw bad_expected_access<void>
        CHECK_FALSE(true); // should not reach
    } catch (std::bad_expected_access<void>& ex) {
        auto msg = std::string{ex.what()};
        CHECK_FALSE(msg.empty());
        // Even though error() is not available for void, ensure message is stable.
        CHECK(msg.find("expected") != std::string::npos);
    }
}

TEST_CASE("shim helper exposes weak what() override") {
    // callExpectedShimWhat() is compiled only when PATHSPACE_NEEDS_EXPECTED_SHIM
    // is defined (coverage builds). Ensure it returns a non-empty message so the
    // weak symbol stays linked and covered.
    auto message = std::string{SP::testing::callExpectedShimWhat()};
    CHECK_FALSE(message.empty());
    CHECK(message.find("expected") != std::string::npos);
}
}
