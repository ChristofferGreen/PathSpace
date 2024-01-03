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

    SECTION("Simple PathSpace Path Into Data", "[PathSpace]") {
        PathSpace pspace;
        REQUIRE(pspace.insert("/test", 54).value_or(0) == 1);
        REQUIRE(pspace.insert("/test/data", 55).value_or(0) == 0);
    }

    SECTION("PathSpace Multi-Component Path", "[PathSpace]") {
        PathSpace pspace;
        REQUIRE(pspace.insert("/test3/test/data", 56).value_or(0) == 1);
        REQUIRE(pspace.toJSON(false) == R"({"PathSpace": {"value0": {"test3": {"index": 1,"data": {"ptr_wrapper": {"valid": 1,"data": {"value0": {"test": {"index": 1,"data": {"ptr_wrapper": {"valid": 1,"data": {"value0": {"data": {"index": 0,"data": {"value0": {"container": [56,0,0,0]}}}}}}}}}}}}}}})" );
    }
}
