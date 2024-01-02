#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/Capabilities.hpp>
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathSpace Construction", "[PathSpace]") {
    SECTION("Simple PathSpace Construction", "[PathSpace]") {
        PathSpace pspace;
        REQUIRE(pspace.insert("/test", 54).value_or(0) == 1);
    }

    SECTION("Simple PathSpace Construction JSON", "[PathSpace]") {
        PathSpace pspace;
        REQUIRE(pspace.insert("/test", 54).value_or(0) == 1);        
        REQUIRE(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test": {"index": 0,"data": {"value0": {"container": [54,0,0,0]}}}}})" );
    }
}
