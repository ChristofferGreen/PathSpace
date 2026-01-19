#include "third_party/doctest.h"

#include <expected>
#include <string>

namespace SP::testing {
const char* callExpectedShimWhat();
}

// Force-compile and execute the shimmed bad_expected_access<void>::what()
// even on platforms where libc++ provides its own implementation.
TEST_SUITE("core.expected_shim.coverage") {
#ifdef PATHSPACE_NEEDS_EXPECTED_SHIM
TEST_CASE("bad_expected_access<void> shim returns message") {
    struct ShimmedAccess : std::bad_expected_access<void> {
        using std::bad_expected_access<void>::bad_expected_access;
        using std::bad_expected_access<void>::what;
    };

    ShimmedAccess ex;
    auto          msg = std::string{ex.what()};
    CHECK_FALSE(msg.empty());
    CHECK(msg.find("expected") != std::string::npos);
}

TEST_CASE("callExpectedShimWhat invokes shimmed override") {
    auto msg = std::string{SP::testing::callExpectedShimWhat()};
    CHECK_FALSE(msg.empty());
    CHECK(msg.find("expected") != std::string::npos);
}
#else
TEST_CASE("expected shim not required on this platform") {
    CHECK(true);
}
#endif
}
