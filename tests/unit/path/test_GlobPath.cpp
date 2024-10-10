#include "ext/doctest.h"
#include <pathspace/path/GlobPath.hpp>

#include <set>

using namespace SP;

TEST_CASE("GlobPath") {
    SUBCASE("Basic Iterator Begin") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SUBCASE("Standard Path") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(path == "/a/b/c");
    }

    SUBCASE("Path Foreach") {
        GlobPathStringView path{"/wooo/fooo/dooo"};
        int i{};
        for (auto const p : path) {
            if (i == 0)
                REQUIRE(p == "wooo");
            else if (i == 1)
                REQUIRE(p == "fooo");
            else if (i == 2)
                REQUIRE(p == "dooo");
            else
                REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("Path Foreach Short") {
        GlobPathStringView path{"/a/b/c"};
        int i{};
        for (auto const p : path) {
            if (i == 0)
                REQUIRE(p == "a");
            else if (i == 1)
                REQUIRE(p == "b");
            else if (i == 2)
                REQUIRE(p == "c");
            else
                REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("Path Iterator End") {
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

    SUBCASE("Default construction Invalid") {
        GlobPathStringView path;
        REQUIRE(path != "");
    }

    SUBCASE("Default construction") {
        GlobPathStringView path{"/"};
        bool a = path == "/";
        REQUIRE(path == "/");
    }

    SUBCASE("Construction with initial path") {
        GlobPathStringView path("/root/child");
        REQUIRE(path == "/root/child");
    }

    SUBCASE("Path does not match different path") {
        GlobPathStringView sp("/path/to/node");
        REQUIRE(sp != "/path/to/another_node");
    }

    GlobPathStringView wildcardPath("/root/*");
    ConcretePathStringView exactPath("/root/child");
    ConcretePathStringView differentPath("/root/otherChild");

    SUBCASE("Glob matches exact path") {
        REQUIRE(wildcardPath == exactPath);
    }

    SUBCASE("Glob matches different path") {
        REQUIRE(wildcardPath == differentPath);
    }

    SUBCASE("Exact path does not match different path") {
        REQUIRE(exactPath != differentPath);
    }

    SUBCASE("Path matches itself") {
        REQUIRE(exactPath == exactPath);
    }

    SUBCASE("Single Wildcard Match") {
        GlobPathStringView sp1{"/a/*/c"};
        ConcretePathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 == sp2);
    }

    SUBCASE("Double Wildcard Match") {
        GlobPathStringView sp1{"/a/**"};
        ConcretePathStringView sp2{"/a/b/c"};
        bool b = sp1 == sp2;
        REQUIRE(sp1 == sp2);

        GlobPathStringView sp3{"/a/**/c"};
        ConcretePathStringView sp4{"/a/b/d/c"};
        REQUIRE(sp3 == sp4);
    }

    SUBCASE("Single Wildcard No Match") {
        GlobPathStringView sp1{"/a/*/d"};
        GlobPathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 != sp2);
    }

    SUBCASE("Empty Name") {
        GlobPathStringView sp1{"/a//d"};
        GlobPathStringView sp2{"/a/d"};
        REQUIRE(sp1 == sp2);
    }

    SUBCASE("Glob Match with Special Characters") {
        GlobPathStringView sp1{"/a/*/c?d"};
        ConcretePathStringView sp2{"/a/b/cxd"};
        REQUIRE(sp1 == sp2);
        GlobPathStringView sp3{"/a/b/c"};
        REQUIRE(sp1 != sp3);
    }

    SUBCASE("Name Containing Wildcard") {
        GlobPathStringView sp1{"/a/test*"};
        ConcretePathStringView sp2{"/a/testbaab"};
        ConcretePathStringView sp3{"/a/test*"};
        REQUIRE(sp1 == sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
    }

    SUBCASE("Name Containing Wildcard Exact Match") {
        const GlobPathStringView sp1{"/a/test\\*"};
        const GlobPathStringView sp2{"/a/testbaab"};
        const ConcretePathStringView sp3{"/a/test*"};
        REQUIRE(sp1 != sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
    }

    SUBCASE("Path with No Glob Characters") {
        const GlobPath<std::string> path{"/user/data/file"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Asterisk Glob") {
        const GlobPath<std::string> path{"/user/*/file"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Question Mark Glob") {
        const GlobPath<std::string> path{"/user/data/fil?"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Range Glob") {
        const GlobPath<std::string> path{"/user/data/file[1-3]"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Escaped Glob Characters") {
        const GlobPath<std::string> path{"/user/data/fi\\*le"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Escaped Escape Character") {
        const GlobPath<std::string> path{"/user/data/fi\\\\le"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Mixed Escaped and Unescaped Gobs") {
        const GlobPath<std::string> path{"/user/\\*/fi*le"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Escaped Range Glob") {
        const GlobPath<std::string> path{"/user/data/fi\\[1-3\\]"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Multiple Glob Patterns") {
        const GlobPath<std::string> path{"/us?er/*/file[0-9]"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Empty Path") {
        const GlobPath<std::string> path{""};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Only Glob Characters") {
        const GlobPath<std::string> path{"/*?"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Only Escaped Glob Characters") {
        const GlobPath<std::string> path{"/\\*\\?"};
        REQUIRE_FALSE(path.isGlob());
    }
}

// TEST_CASE("Path Wildcard Maps", "[Path]") {
//     std::map<Path, int> map;
//     map[Path("/a/b/c")] = 1;
//
//     std::unordered_map<Path, int, PathHash, PathEqual> unordered_map;
//     unordered_map[Path("/a/b/c")] = 1;
//
//     SUBCASE("Standard Map Contains With Wildcard") {
//         REQUIRE(Path::containsWithWildcard(map, Path("/a/*/c")));
//     }
//
//     SUBCASE("Standard Map Does Not Contain With Wildcard") {
//         REQUIRE_FALSE(Path::containsWithWildcard(map, Path("/a/c")));
//     }
//
//     SUBCASE("Unordered Map Contains With Wildcard") {
//         REQUIRE(Path::containsWithWildcard(unordered_map, Path("/a/*/c")));
//     }
//
//     SUBCASE("Unordered Map Does Not Contain With Wildcard") {
//         REQUIRE_FALSE(Path::containsWithWildcard(unordered_map, Path("/a/c")));
//     }
// }