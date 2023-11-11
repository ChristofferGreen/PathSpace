#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/SpacePath.hpp>

using namespace SP;

TEST_CASE("SpacePath construction and basic operations", "[SpacePath]") {
    SECTION("Default construction") {
        SpacePath path;
        REQUIRE(path.toString() == "");  // Assuming SpacePath has a toString() method
    }

    SECTION("Construction with initial path") {
        SpacePath path("/root/child");s
        REQUIRE(path.toString() == "/root/child");
    }
}

TEST_CASE("SpacePath wildcard support", "[SpacePath]") {
    SpacePath wildcardPath("/root/*" );
    SpacePath exactPath( "/root/child" );
    SpacePath differentPath( "/root/otherChild" );

    SECTION("Wildcard matches exact path") {
        REQUIRE(wildcardPath.matches(exactPath) == true);
    }

    SECTION("Wildcard matches different path") {
        REQUIRE(wildcardPath.matches(differentPath) == true);
    }

    SECTION("Exact path does not match different path") {
        REQUIRE(exactPath.matches(differentPath) == false);
    }

    SECTION("Path matches itself") {
        REQUIRE(exactPath.matches(exactPath) == true);
    }

    SECTION("Path does not match wildcard") {
        REQUIRE(exactPath.matches(wildcardPath) == false);
    }
}

// Add more test cases as needed to cover all the functionalities of SpacePath
