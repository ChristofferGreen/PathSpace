#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/GlobPath.hpp>

using namespace SP;

TEST_CASE("GlobPath", "[GlobPath]") {
    SECTION("Standard Path") {
        GlobPath path{"/a/b/c"};
        REQUIRE(path.toString()=="/a/b/c");
    }

    SECTION("Path Foreach") {
        GlobPath path{"/wooo/fooo/dooo"};
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="wooo");
            else if(i==1) REQUIRE(p=="fooo");
            else if(i==2) REQUIRE(p=="dooo");
            else REQUIRE(false);
            ++i;
        }
    }

    SECTION("Path Foreach Short") {
        GlobPath path{"/a/b/c"};
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="a");
            else if(i==1) REQUIRE(p=="b");
            else if(i==2) REQUIRE(p=="c");
            else REQUIRE(false);
            ++i;
        }
    }
}

/*

TEST_CASE("Path Construction", "[Path]") {
    SECTION("Default construction") {
        Path path;
        REQUIRE(path.toString() == "");  // Assuming Path has a toString() method
    }

    SECTION("Construction with initial path") {
        Path path("/root/child");
        REQUIRE(path.toString() == "/root/child");
    }
}

TEST_CASE("Path Matching", "[Path]") {
    SECTION("Path does not match different path", "[Path]") {
        Path sp("/path/to/node");
        REQUIRE(sp.toString() != "/path/to/another_node");
    }
}

TEST_CASE("Path Wildcard", "[Path]") {
    Path wildcardPath("/root/*" );
    Path exactPath( "/root/child" );
    Path differentPath( "/root/otherChild" );

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
*/
//    SECTION("Single Wildcard Match") {
//        Path sp1("/a/*/c");
//        Path sp2("/a/b/c");
//        REQUIRE(sp1.matches(sp2));
//    }
//
//    SECTION("Single Wildcard No Match") {
//        Path sp1("/a/*/d");
//        Path sp2("/a/b/c");
//        REQUIRE_FALSE(sp1.matches(sp2));
//    }
//
//    SECTION("Multiple Wildcard Match") {
//        Path sp1("/a/**/c");
//        Path sp2("/a/b/d/c");
//        REQUIRE(sp1.matches(sp2));
//    }
//
//    SECTION("Wildcard Match with Special Characters") {
//        Path sp1("/a/*/c?d");
//        Path sp2("/a/b/cxd");
//        REQUIRE(sp1.matches(sp2));
//    }
//
//   SECTION("Filename Containing Wildcard") {
//        Path sp1("/a/test*");
//        Path sp2("/a/testbaab");
//        Path sp3("/a/test\\*");
//        REQUIRE(sp1.matches(sp2));
//        REQUIRE(!sp2.matches(sp3));
//        REQUIRE(sp3.toString()=="/a/test*");
//    }
//}
//
//TEST_CASE("Path Wildcard Maps", "[Path]") {
//    std::map<Path, int> map;
//    map[Path("/a/b/c")] = 1;
//
//    std::unordered_map<Path, int, PathHash, PathEqual> unordered_map;
//    unordered_map[Path("/a/b/c")] = 1;
//
//    SECTION("Standard Map Contains With Wildcard") {
//        REQUIRE(Path::containsWithWildcard(map, Path("/a/*/c")));
//    }
//
//    SECTION("Standard Map Does Not Contain With Wildcard") {
//        REQUIRE_FALSE(Path::containsWithWildcard(map, Path("/a/c")));
//    }
//
//    SECTION("Unordered Map Contains With Wildcard") {
//        REQUIRE(Path::containsWithWildcard(unordered_map, Path("/a/*/c")));
//    }
//
//    SECTION("Unordered Map Does Not Contain With Wildcard") {
//        REQUIRE_FALSE(Path::containsWithWildcard(unordered_map, Path("/a/c")));
//    }
//}
