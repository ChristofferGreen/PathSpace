#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/BasePath.hpp>

using namespace SP;

TEST_CASE("BasePath") {
    SECTION("BasePath String Construction") {
        BasePathString path;
    }

    SECTION("BasePath StringView Construction") {
        BasePathStringView path;
    }
}
