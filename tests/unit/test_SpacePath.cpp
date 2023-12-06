#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/SpacePath.hpp>

using namespace SP;


TEST_CASE("SpacePath Construction", "[SpacePath]") {
    SECTION("Default construction") {
        SpacePath path;
        REQUIRE(path.toString() == "");  // Assuming SpacePath has a toString() method
    }

    SECTION("Construction with initial path") {
        SpacePath path("/root/child");
        REQUIRE(path.toString() == "/root/child");
    }
}

TEST_CASE("SpacePath Matching", "[SpacePath]") {
    SECTION("SpacePath does not match different path", "[SpacePath]") {
        SpacePath sp("/path/to/node");
        REQUIRE(sp.toString() != "/path/to/another_node");
    }
}

TEST_CASE("SpacePath Wildcard", "[SpacePath]") {
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

    SECTION("Single Wildcard Match") {
        SpacePath sp1("/a/*/c");
        SpacePath sp2("/a/b/c");
        REQUIRE(sp1.matches(sp2));
    }

    SECTION("Single Wildcard No Match") {
        SpacePath sp1("/a/*/d");
        SpacePath sp2("/a/b/c");
        REQUIRE_FALSE(sp1.matches(sp2));
    }

    SECTION("Multiple Wildcard Match") {
        SpacePath sp1("/a/**/c");
        SpacePath sp2("/a/b/d/c");
        REQUIRE(sp1.matches(sp2));
    }

    SECTION("Wildcard Match with Special Characters") {
        SpacePath sp1("/a/*/c?d");
        SpacePath sp2("/a/b/cxd");
        REQUIRE(sp1.matches(sp2));
    }

   SECTION("Filename Containing Wildcard") {
        SpacePath sp1("/a/test*");
        SpacePath sp2("/a/testbaab");
        SpacePath sp3("/a/test\\*");
        REQUIRE(sp1.matches(sp2));
        REQUIRE(!sp2.matches(sp3));
        REQUIRE(sp3.toString()=="/a/test*");
    }
}

TEST_CASE("SpacePath Wildcard Maps", "[SpacePath]") {
    std::map<SpacePath, int> map;
    map[SpacePath("/a/b/c")] = 1;

    std::unordered_map<SpacePath, int, SpacePathHash, SpacePathEqual> unordered_map;
    unordered_map[SpacePath("/a/b/c")] = 1;

    SECTION("Standard Map Contains With Wildcard") {
        REQUIRE(SpacePath::containsWithWildcard(map, SpacePath("/a/*/c")));
    }

    SECTION("Standard Map Does Not Contain With Wildcard") {
        REQUIRE_FALSE(SpacePath::containsWithWildcard(map, SpacePath("/a/c")));
    }

    SECTION("Unordered Map Contains With Wildcard") {
        REQUIRE(SpacePath::containsWithWildcard(unordered_map, SpacePath("/a/*/c")));
    }

    SECTION("Unordered Map Does Not Contain With Wildcard") {
        REQUIRE_FALSE(SpacePath::containsWithWildcard(unordered_map, SpacePath("/a/c")));
    }
}