#include <catch2/catch_test_macros.hpp>
#include <pathspace/path/GlobPath.hpp>

#include <set>

using namespace SP;

TEST_CASE("GlobPath") {
    SECTION("Basic Iterator Begin", "[Path][GlobPath]") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SECTION("Standard Path", "[Path][GlobPath]") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(path=="/a/b/c");
    }

    SECTION("Path Foreach", "[Path][GlobPath]") {
        GlobPathStringView path{"/wooo/fooo/dooo"};
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="wooo");
            else if(i==1) REQUIRE(p=="fooo");
            else if(i==2) REQUIRE(p=="dooo");
            else REQUIRE(false);
            ++i;
        }
    }

    SECTION("Path Foreach Short", "[Path][GlobPath]") {
        GlobPathStringView path{"/a/b/c"};
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="a");
            else if(i==1) REQUIRE(p=="b");
            else if(i==2) REQUIRE(p=="c");
            else REQUIRE(false);
            ++i;
        }
    }

    SECTION("Path Iterator End", "[Path][GlobPath]") {
        GlobPathStringView path{"/a/b/c"};
        auto iter = path.begin();
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter == path.end());
    }

    SECTION("Default construction Invalid", "[Path][GlobPath]") {
        GlobPathStringView path;
        REQUIRE(path != "");
    }

    SECTION("Default construction", "[Path][GlobPath]") {
        GlobPathStringView path{"/"};
        REQUIRE(path == "/");
    }

    SECTION("Construction with initial path", "[Path][GlobPath]") {
        GlobPathStringView path("/root/child");
        REQUIRE(path == "/root/child");
    }

    SECTION("Path does not match different path", "[Path][GlobPath]") {
        GlobPathStringView sp("/path/to/node");
        REQUIRE(sp != "/path/to/another_node");
    }

    GlobPathStringView wildcardPath("/root/*" );
    ConcretePathStringView exactPath( "/root/child" );
    ConcretePathStringView differentPath( "/root/otherChild" );

    SECTION("Glob matches exact path", "[Path][GlobPath]") {
        REQUIRE(wildcardPath == exactPath);
    }

    SECTION("Glob matches different path", "[Path][GlobPath]") {
        REQUIRE(wildcardPath == differentPath);
    }

    SECTION("Exact path does not match different path", "[Path][GlobPath]") {
        REQUIRE(exactPath != differentPath);
    }

    SECTION("Path matches itself", "[Path][GlobPath]") {
        REQUIRE(exactPath == exactPath);
    }

    SECTION("Single Wildcard Match", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/*/c"};
        ConcretePathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 == sp2);
    }

    SECTION("Double Wildcard Match", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/**"};
        ConcretePathStringView sp2{"/a/b/c"};
        bool b = sp1 == sp2; 
        REQUIRE(sp1 == sp2);

        GlobPathStringView sp3{"/a/**/c"};
        ConcretePathStringView sp4{"/a/b/d/c"};
        REQUIRE(sp3 == sp4);
    }

    SECTION("Single Wildcard No Match", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/*/d"};
        GlobPathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 != sp2);
    }

    SECTION("Empty Name", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a//d"};
        GlobPathStringView sp2{"/a/d"};
        REQUIRE(sp1 == sp2);
    }

    SECTION("Glob Match with Special Characters", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/*/c?d"};
        ConcretePathStringView sp2{"/a/b/cxd"};
        REQUIRE(sp1 == sp2);
        GlobPathStringView sp3{"/a/b/c"};
        REQUIRE(sp1 != sp3);
    }

   SECTION("Name Containing Wildcard", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/test*"};
        ConcretePathStringView sp2{"/a/testbaab"};
        ConcretePathStringView sp3{"/a/test*"};
        REQUIRE(sp1 == sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
   }

   SECTION("Name Containing Wildcard Exact Match", "[Path][GlobPath]") {
        GlobPathStringView sp1{"/a/test\\*"};
        GlobPathStringView sp2{"/a/testbaab"};
        ConcretePathStringView sp3{"/a/test*"};
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