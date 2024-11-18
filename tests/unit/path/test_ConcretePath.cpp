#include "ext/doctest.h"
#include <pathspace/path/ConcretePath.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace SP;

TEST_CASE("Path ConcretePath") {
    SUBCASE("Default Construction") {
        ConcretePathString path;
        REQUIRE(path.validate().has_value());
        ConcretePathStringView path2;
        REQUIRE(path2.validate().has_value());
    }

    SUBCASE("Construction Path With Only Slashes") {
        ConcretePathString slashesPath{"///"};
        REQUIRE(slashesPath.validate().has_value());
        REQUIRE(slashesPath == "/");
    }

    SUBCASE("Construction With Value") {
        ConcretePathString path{"/a/b/c"};
        REQUIRE(path == "/a/b/c");
        auto b = path != "/a/b/d";
        REQUIRE(path != "/a/b/d");

        ConcretePathStringView path2{"/a/b/c"};
        REQUIRE(path2 == "/a/b/c");
        REQUIRE(path2 != "/a/b/d");
    }

    SUBCASE("Construction With Root Path") {
        ConcretePathString path{"/"};
        REQUIRE(path == "/");
    }

    SUBCASE("Construction Long Path") {
        std::string const  longPath = "/a/" + std::string(1000, 'b') + "/c";
        ConcretePathString path(longPath);
        REQUIRE(!path.validate().has_value());
    }

    SUBCASE("Construction With initial path") {
        ConcretePathString path{"/root/child"};
        REQUIRE(path == "/root/child");
        REQUIRE(ConcretePathString{"/root/child2"} == "/root/child2");
        REQUIRE(ConcretePathString{"/root/child3"} == ConcretePathString{"/root/child3"});
    }

    SUBCASE("Match different path") {
        ConcretePathString sp{"/path/to/node"};
        REQUIRE(sp != "/path/to/another_node");
    }

    SUBCASE("Copy and Move Semantics") {
        ConcretePathString       originalPath{"/a/b"};
        ConcretePathString const copiedPath = originalPath; // Copy
        REQUIRE(copiedPath == originalPath);

        ConcretePathString const movedPath = std::move(originalPath); // Move
        REQUIRE(movedPath == "/a/b");
        // originalPath state after move is unspecified
    }

    SUBCASE("Assignment Operations") {
        ConcretePathString       path1{"/a/b"};
        ConcretePathString const path2("/c/d");
        path1 = path2;
        REQUIRE(path1 == path2);

        // Self-assignment
        path1 = path1;
        REQUIRE(path1 == "/c/d");
    }

    SUBCASE("Relative Paths") {
        ConcretePathString relativePath{"./a/b"};
        REQUIRE(relativePath.validate().has_value());
    }

    SUBCASE("Paths with Special Characters") {
        ConcretePathString const path{"/path/with special@chars#"};
        REQUIRE(!path.validate().has_value());
    }

    SUBCASE("Mixed Slash Types") {
        ConcretePathString path{"/path\\with/mixed/slashes\\"};
        REQUIRE(!path.validate().has_value());
    }

    SUBCASE("Multiple Consecutive Slashes") {
        ConcretePathString path{"/path//with///multiple/slashes"};
        REQUIRE(path.validate().has_value());
    }

    SUBCASE("Trailing Slashes") {
        ConcretePathString path{"/path/with/trailing/slash/"};
        REQUIRE(path.validate().has_value());
    }

    SUBCASE("Unicode Characters in Path") {
        ConcretePathString path{"/路径/含有/非ASCII字符"};
        REQUIRE(!path.validate().has_value());
        REQUIRE(path == "/路径/含有/非ASCII字符");
        auto iter = path.begin();
        REQUIRE(*iter == "路径");
        ++iter;
        REQUIRE(*iter == "含有");
        ++iter;
        REQUIRE(*iter == "非ASCII字符");
        ++iter;
        REQUIRE(iter == path.end());
    }

    SUBCASE("Empty Components in Path") {
        ConcretePathString path{"/a/b//c/d/"};
        REQUIRE(path.validate().has_value());
    }

    SUBCASE("Path Normalization") {
        ConcretePathString path{"/a/./b/../c/"};
        REQUIRE(path.validate().has_value());
        REQUIRE(path != "/a/c");
        REQUIRE(path != "/a/b/c");
    }

    SUBCASE("Path Comparison Case Sensitivity") {
        ConcretePathString path1{"/Path/To/Node"};
        ConcretePathString path2{"/path/to/node"};
        REQUIRE(path1 != path2);
    }

    SUBCASE("Comparison operators") {
        ConcretePathString path1("/foo/bar");
        ConcretePathString path2("/foo/baz");
        ConcretePathString path3("/foo/bar");

        CHECK(path1 < path2);
        CHECK(path2 > path1);
        CHECK(path1 <= path3);
        CHECK(path1 >= path3);
        CHECK(path1 != path2);
    }

    SUBCASE("Comparison with string_view") {
        ConcretePathString path("/foo/bar");
        std::string_view   sv1 = "/foo/bar";
        std::string_view   sv2 = "/foo/baz";

        CHECK(path == sv1);
        CHECK(path != sv2);
        CHECK(path < sv2);
        CHECK(sv1 == path);
        CHECK(sv2 > path);
    }

    SUBCASE("Use in std::set") {
        std::set<ConcretePathString> pathSet;
        pathSet.insert(ConcretePathString("/foo/bar"));
        pathSet.insert(ConcretePathString("/foo/baz"));
        pathSet.insert(ConcretePathString("/foo/bar")); // Duplicate

        CHECK(pathSet.size() == 2);
        CHECK(pathSet.find(ConcretePathString("/foo/bar")) != pathSet.end());
        CHECK(pathSet.find(ConcretePathString("/foo/qux")) == pathSet.end());
    }

    SUBCASE("Sorting") {
        std::vector<ConcretePathString> paths = {ConcretePathString("/c"), ConcretePathString("/a"), ConcretePathString("/b")};
        std::sort(paths.begin(), paths.end());

        CHECK(paths[0] == ConcretePathString("/a"));
        CHECK(paths[1] == ConcretePathString("/b"));
        CHECK(paths[2] == ConcretePathString("/c"));
    }

    SUBCASE("ConcretePathStringView comparisons") {
        ConcretePathStringView path1("/foo/bar");
        ConcretePathStringView path2("/foo/baz");

        CHECK(path1 < path2);
        CHECK(path2 > path1);
        CHECK(path1 != path2);
    }

    SUBCASE("Mixed comparisons") {
        ConcretePathString     pathString("/foo/bar");
        ConcretePathStringView pathView("/foo/bar");

        CHECK(pathString == pathView);
        CHECK(pathView == pathString);
        CHECK(!(pathString < pathView));
        CHECK(!(pathView < pathString));
    }

    SUBCASE("Conversion to string_view") {
        ConcretePathString path("/foo/bar");
        std::string_view   sv = static_cast<std::string_view>(path);

        CHECK(sv == "/foo/bar");
    }

    SUBCASE("Empty and root paths") {
        ConcretePathString emptyPath("");
        ConcretePathString rootPath("/");

        CHECK(emptyPath < rootPath);
        CHECK(rootPath > emptyPath);
        CHECK(emptyPath != rootPath);
    }

    SUBCASE("Paths with different depths") {
        ConcretePathString path1("/foo");
        ConcretePathString path2("/foo/bar");

        CHECK(path1 < path2);
        CHECK(path2 > path1);
    }

    SUBCASE("Case sensitivity") {
        ConcretePathString path1("/foo/bar");
        ConcretePathString path2("/foo/Bar");

        CHECK(path1 != path2);
        CHECK(path1 > path2);
    }

    SUBCASE("Paths with special characters") {
        ConcretePathString path1("/foo/bar-1");
        ConcretePathString path2("/foo/bar_1");

        CHECK(path1 != path2);
        CHECK(path1 < path2); // '-' comes before '_' in ASCII
    }

    SUBCASE("Very long paths") {
        std::string longPathStr(1000, 'a');
        longPathStr[0] = '/'; // Ensure it starts with '/'
        ConcretePathString longPath(longPathStr);
        ConcretePathString normalPath("/b");

        CHECK(longPath < normalPath);
    }

    SUBCASE("Use in std::map") {
        std::map<ConcretePathString, int> pathMap;
        pathMap[ConcretePathString("/foo")] = 1;
        pathMap[ConcretePathString("/bar")] = 2;

        CHECK(pathMap.size() == 2);
        CHECK(pathMap[ConcretePathString("/foo")] == 1);
        CHECK(pathMap[ConcretePathString("/bar")] == 2);

        // Test lookup
        CHECK(pathMap.find(ConcretePathString("/foo")) != pathMap.end());
        CHECK(pathMap.find(ConcretePathString("/bar")) != pathMap.end());
        CHECK(pathMap.find(ConcretePathString("/baz")) == pathMap.end());
    }
}
