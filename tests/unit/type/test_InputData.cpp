#include <catch2/catch_test_macros.hpp>
#include <pathspace/type/InputData.hpp>

using namespace SP;

TEST_CASE("InputData", "[Type][InputData]") {
    SECTION("Simple Construction", "[Type][InputData]") {
        int a{};
        InputData data{a};
    }
}