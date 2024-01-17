#include <catch2/catch_test_macros.hpp>
#include <pathspace/path/ConcretePath.hpp>

using namespace SP;

TEST_CASE("ConcretePath", "[Path][ConcretePath]") {
    SECTION("Default Construction") {
        ConcretePathString path;
        REQUIRE(!path.isValid());
        ConcretePathStringView path2;
        REQUIRE(!path2.isValid());
    }

    SECTION("Default Construction Invalid", "[Path][ConcretePath]") {
        ConcretePathStringView path;
        REQUIRE(path != "");
        REQUIRE(!path.isValid());
    }

    SECTION("Construction With Empty String Is Invalid", "[Path][ConcretePath]") {
        ConcretePathStringView path{""};
        REQUIRE(path != "");
        REQUIRE(!path.isValid());
    }

    SECTION("Construction Path With Only Slashes", "[Path][ConcretePath]") {
        ConcretePathString slashesPath{"///"};
        REQUIRE(slashesPath.isValid());
        REQUIRE(slashesPath == "/");
    }

    SECTION("Construction With Value", "[Path][ConcretePath]") {
        ConcretePathString path{"/a/b/c"};
        REQUIRE(path == "/a/b/c");
        auto b = path != "/a/b/d";
        REQUIRE(path != "/a/b/d");
        
        ConcretePathStringView path2{"/a/b/c"};
        REQUIRE(path2 == "/a/b/c");
        REQUIRE(path2 != "/a/b/d");
    }

    SECTION("Construction With Root Path", "[Path][ConcretePath]") {
        ConcretePathString path{"/"};
        REQUIRE(path == "/");
    }

    SECTION("Construction Long Path", "[Path][ConcretePath]") {
        std::string const longPath = "/a/" + std::string(1000, 'b') + "/c";
        ConcretePathString path(longPath);
        REQUIRE(path.isValid());
    }

    SECTION("Construction With initial path", "[Path][ConcretePath]") {
        ConcretePathString path{"/root/child"};
        REQUIRE(path == "/root/child");
        REQUIRE(ConcretePathString{"/root/child2"} == "/root/child2");
        REQUIRE(ConcretePathString{"/root/child3"} == ConcretePathString{"/root/child3"});
    }

    SECTION("Match different path", "[Path][ConcretePath]") {
        ConcretePathString sp{"/path/to/node"};
        REQUIRE(sp != "/path/to/another_node");
    }

    SECTION("Copy and Move Semantics", "[Path][ConcretePath]") {
        ConcretePathString originalPath{"/a/b"};
        ConcretePathString const copiedPath = originalPath; // Copy
        REQUIRE(copiedPath == originalPath);

        ConcretePathString const movedPath = std::move(originalPath); // Move
        REQUIRE(movedPath == "/a/b");
        // originalPath state after move is unspecified
    }

    SECTION("Assignment Operations", "[Path][ConcretePath]") {
        ConcretePathString path1{"/a/b"};
        ConcretePathString const path2("/c/d");
        path1 = path2;
        REQUIRE(path1 == path2);

        // Self-assignment
        path1 = path1;
        REQUIRE(path1 == "/c/d");
    }

    SECTION("Relative Paths", "[Path][ConcretePath]") {
        ConcretePathString relativePath{"./a/b"};
        REQUIRE(!relativePath.isValid());
    }

    SECTION("Paths with Special Characters", "[Path][ConcretePath]") {
        ConcretePathString const path{"/path/with special@chars#"};
        REQUIRE(path.isValid());
    }

    SECTION("Mixed Slash Types", "[Path][ConcretePath]") {
        ConcretePathString path{"/path\\with/mixed/slashes\\"};
        REQUIRE(path.isValid());
    }

    SECTION("Multiple Consecutive Slashes", "[Path][ConcretePath]") {
        ConcretePathString path{"/path//with///multiple/slashes"};
        REQUIRE(path.isValid());
    }

    SECTION("Trailing Slashes", "[Path][ConcretePath]") {
        ConcretePathString path{"/path/with/trailing/slash/"};
        REQUIRE(path.isValid());
    }

    SECTION("Unicode Characters in Path", "[Path][ConcretePath]") {
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

    SECTION("Empty Components in Path", "[Path][ConcretePath]") {
        ConcretePathString path{"/a/b//c/d/"};
        REQUIRE(path.isValid());
        REQUIRE(path=="/a/b/c/d/");
        REQUIRE(path=="/a/b/c/d/");
        REQUIRE(path=="/a//b/c////d/");
        REQUIRE(path!="/a//b/c////e/");
    }

    SECTION("Path Normalization", "[Path][ConcretePath]") {
        ConcretePathString path{"/a/./b/../c/"};
        REQUIRE(!path.isValid());
        REQUIRE(path != "/a/c");
        REQUIRE(path != "/a/b/c");
        REQUIRE(path != "/a/./b/../c/");
    }

    SECTION("Path Comparison Case Sensitivity", "[Path][ConcretePath]") {
        ConcretePathString path1{"/Path/To/Node"};
        ConcretePathString path2{"/path/to/node"};
        REQUIRE(path1 != path2);
    }
}
