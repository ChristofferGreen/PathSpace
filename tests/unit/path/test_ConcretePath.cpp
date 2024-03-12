#include "ext/doctest.h"
#include <pathspace/path/ConcretePath.hpp>

using namespace SP;

TEST_CASE("ConcretePath") {
    SUBCASE("Default Construction") {
        ConcretePathString path;
        REQUIRE(!path.isValid());
        ConcretePathStringView path2;
        REQUIRE(!path2.isValid());
    }

    SUBCASE("Default Construction Invalid") {
        ConcretePathStringView path;
        REQUIRE(path != "");
        REQUIRE(!path.isValid());
    }

    SUBCASE("Construction With Empty String Is Invalid") {
        ConcretePathStringView path{""};
        REQUIRE(path != "");
        REQUIRE(!path.isValid());
    }

    SUBCASE("Construction Path With Only Slashes") {
        ConcretePathString slashesPath{"///"};
        REQUIRE(slashesPath.isValid());
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
        std::string const longPath = "/a/" + std::string(1000, 'b') + "/c";
        ConcretePathString path(longPath);
        REQUIRE(path.isValid());
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
        ConcretePathString originalPath{"/a/b"};
        ConcretePathString const copiedPath = originalPath; // Copy
        REQUIRE(copiedPath == originalPath);

        ConcretePathString const movedPath = std::move(originalPath); // Move
        REQUIRE(movedPath == "/a/b");
        // originalPath state after move is unspecified
    }

    SUBCASE("Assignment Operations") {
        ConcretePathString path1{"/a/b"};
        ConcretePathString const path2("/c/d");
        path1 = path2;
        REQUIRE(path1 == path2);

        // Self-assignment
        path1 = path1;
        REQUIRE(path1 == "/c/d");
    }

    SUBCASE("Relative Paths") {
        ConcretePathString relativePath{"./a/b"};
        REQUIRE(!relativePath.isValid());
    }

    SUBCASE("Paths with Special Characters") {
        ConcretePathString const path{"/path/with special@chars#"};
        REQUIRE(path.isValid());
    }

    SUBCASE("Mixed Slash Types") {
        ConcretePathString path{"/path\\with/mixed/slashes\\"};
        REQUIRE(path.isValid());
    }

    SUBCASE("Multiple Consecutive Slashes") {
        ConcretePathString path{"/path//with///multiple/slashes"};
        REQUIRE(path.isValid());
    }

    SUBCASE("Trailing Slashes") {
        ConcretePathString path{"/path/with/trailing/slash/"};
        REQUIRE(path.isValid());
    }

    SUBCASE("Unicode Characters in Path") {
        ConcretePathString path{"/路径/含有/非ASCII字符"};
        REQUIRE(path.isValid());
        REQUIRE(path=="/路径/含有/非ASCII字符");
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
        REQUIRE(path.isValid());
        REQUIRE(path=="/a/b/c/d/");
        REQUIRE(path=="/a/b/c/d/");
        REQUIRE(path=="/a//b/c////d/");
        REQUIRE(path!="/a//b/c////e/");
    }

    SUBCASE("Path Normalization") {
        ConcretePathString path{"/a/./b/../c/"};
        REQUIRE(!path.isValid());
        REQUIRE(path != "/a/c");
        REQUIRE(path != "/a/b/c");
        REQUIRE(path != "/a/./b/../c/");
    }

    SUBCASE("Path Comparison Case Sensitivity") {
        ConcretePathString path1{"/Path/To/Node"};
        ConcretePathString path2{"/path/to/node"};
        REQUIRE(path1 != path2);
    }
}
