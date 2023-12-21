#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/Capabilities.hpp>
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathSpace Construction", "[PathSpace]") {
    SECTION("Simple PathSpace Construction", "[PathSpace]") {
        PathSpace pspace;
        pspace.insert("/test", 54);
    }
}