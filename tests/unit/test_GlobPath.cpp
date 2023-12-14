#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/GlobPath.hpp>
#include <pathspace/core/Path.hpp>

using namespace SP;

TEST_CASE("GlobPath") {
    SECTION("Standard Path") {
        GlobPath path{"/a/b/c"};
        REQUIRE(path=="/a/b/c");
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

    SECTION("Path Iterator End") {
        GlobPath path{"/a/b/c"};
        auto iter = path.begin();
        REQUIRE(!iter.isAtEnd());
        ++iter;
        REQUIRE(!iter.isAtEnd());
        ++iter;
        REQUIRE(!iter.isAtEnd());
        ++iter;
        REQUIRE(iter.isAtEnd());
    }

    SECTION("Default construction Invalid") {
        GlobPath path;
        REQUIRE(path != "");
    }

    SECTION("Default construction") {
        GlobPath path{"/"};
        REQUIRE(path == "/");
    }

    SECTION("Construction with initial path") {
        GlobPath path("/root/child");
        REQUIRE(path == "/root/child");
    }

    SECTION("Path does not match different path") {
        GlobPath sp("/path/to/node");
        REQUIRE(sp != "/path/to/another_node");
    }

    GlobPath wildcardPath("/root/*" );
    PathStringView exactPath( "/root/child" );
    PathStringView differentPath( "/root/otherChild" );

    SECTION("Glob matches exact path") {
        auto b = wildcardPath == exactPath;
        REQUIRE(wildcardPath == exactPath);
    }

    SECTION("Glob matches different path") {
        REQUIRE(wildcardPath == differentPath);
    }

    SECTION("Exact path does not match different path") {
        REQUIRE(exactPath != differentPath);
    }

    SECTION("Path matches itself") {
        REQUIRE(exactPath == exactPath);
    }

    SECTION("Single Wildcard Match") {
        GlobPath sp1("/a/*/c");
        PathStringView sp2("/a/b/c");
        REQUIRE(sp1 == sp2);
    }

    SECTION("Double Wildcard Match") {
        GlobPath sp1("/a/**");
        PathStringView sp2("/a/b/c");
        bool b = sp1 == sp2; 
        REQUIRE(sp1 == sp2);

        GlobPath sp3("/a/**/c");
        PathStringView sp4("/a/b/d/c");
        REQUIRE(sp3 == sp4);
    }

    SECTION("Single Wildcard No Match") {
        GlobPath sp1("/a/*/d");
        GlobPath sp2("/a/b/c");
        REQUIRE(sp1 != sp2);
    }

    SECTION("Empty Name") {
        GlobPath sp1("/a//d");
        GlobPath sp2("/a/d");
        REQUIRE(sp1 == sp2);
    }

    SECTION("Glob Match with Special Characters") {
        GlobPath sp1("/a/*/c?d");
        PathStringView sp2("/a/b/cxd");
        REQUIRE(sp1 == sp2);
        GlobPath sp3("/a/b/c");
        REQUIRE(sp1 != sp3);
    }

   SECTION("Name Containing Wildcard") {
        GlobPath sp1("/a/test*");
        PathStringView sp2("/a/testbaab");
        PathStringView sp3("/a/test*");
        REQUIRE(sp1 == sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
   }

   SECTION("Name Containing Wildcard Exact Match") {
        GlobPath sp1("/a/test\\*");
        GlobPath sp2("/a/testbaab");
        PathStringView sp3("/a/test*");
        REQUIRE(sp1 != sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
   }
}

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
