#include "ext/doctest.h"
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/path/ConcretePathIterator.hpp>

#include <set>

using namespace SP;

TEST_CASE("ConcretePathIterator") {
    SUBCASE("Basic Iterator Begin") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SUBCASE("ForEach Name Iteration Short") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(path.isValid());
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

    SUBCASE("ForEach Name Iteration Long") {
        ConcretePathStringView const path{"/woo/Foo/dOoO"};
        REQUIRE(path.isValid());
        int i{};
        for (auto const p : path) {
            if (i == 0)
                REQUIRE(p == "woo");
            else if (i == 1)
                REQUIRE(p == "Foo");
            else if (i == 2)
                REQUIRE(p == "dOoO");
            else
                REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("Iterator End") {
        ConcretePathStringView const path{"/a/b/c"};
        auto iter = path.begin();
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter == path.end());
    }

    SUBCASE("Match Skipped Name") {
        ConcretePathString const sp1{"/a//d"};
        ConcretePathString const sp2{"/a/d"};
        REQUIRE(sp1 == sp2);
        ConcretePathString sp3("//a/////d");
        REQUIRE(sp1 == sp3);
        ConcretePathString sp4("//a/////e");
        REQUIRE(sp1 != sp4);
    }

    SUBCASE("Path With Trailing Slash") {
        ConcretePathString path{"/a/b/c/"};
        auto iter = path.begin();
        REQUIRE(iter != path.end());
        REQUIRE(*iter == "a");
        ++iter;
        REQUIRE(iter != path.end());
        REQUIRE(*iter == "b");
        ++iter;
        REQUIRE(iter != path.end());
        REQUIRE(*iter == "c");
        ++iter;
        REQUIRE(iter == path.end());
    }

    SUBCASE("isAtStart() functionality") {
        ConcretePathStringView path{"/a/b/c"};
        auto iter = path.begin();
        REQUIRE(iter.isAtStart());
        ++iter;
        REQUIRE_FALSE(iter.isAtStart());
    }

    SUBCASE("isAtStart() with empty path") {
        ConcretePathStringView emptyPath{""};
        auto iter = emptyPath.begin();
        REQUIRE(iter.isAtStart());
        REQUIRE(iter == emptyPath.end());
    }

    SUBCASE("fullPath() functionality") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(path.begin().fullPath() == "/a/b/c");
        auto iter = path.begin();
        ++iter;
        REQUIRE(iter.fullPath() == "/a/b/c");
    }

    SUBCASE("fullPath() with empty path") {
        ConcretePathStringView emptyPath{""};
        REQUIRE(emptyPath.begin().fullPath().empty());
    }

    SUBCASE("Iterator with only root") {
        ConcretePathStringView rootPath{"/"};
        auto iter = rootPath.begin();
        REQUIRE(iter == rootPath.begin());
        REQUIRE(iter.fullPath() == "/");
    }

    SUBCASE("Iterator with multiple consecutive slashes") {
        ConcretePathStringView path{"///a////b///c//"};
        std::vector<std::string> components;
        for (auto const& component : path) {
            components.push_back(std::string(component.getName()));
        }
        REQUIRE(components == std::vector<std::string>{"a", "b", "c"});
        REQUIRE(path.begin().fullPath() == "///a////b///c//");
    }

    SUBCASE("Iterating and checking isAtStart()") {
        ConcretePathStringView path{"/a/b/c"};
        auto iter = path.begin();
        REQUIRE(iter.isAtStart());
        REQUIRE(*iter == "a");
        ++iter;
        REQUIRE_FALSE(iter.isAtStart());
        REQUIRE(*iter == "b");
        ++iter;
        REQUIRE_FALSE(iter.isAtStart());
        REQUIRE(*iter == "c");
        ++iter;
        REQUIRE_FALSE(iter.isAtStart());
        REQUIRE(iter == path.end());
    }

    SUBCASE("fullPath() consistency across iterations") {
        ConcretePathStringView path{"/a/b/c"};
        auto iter = path.begin();
        std::string fullPath = std::string(iter.fullPath());
        while (iter != path.end()) {
            REQUIRE(iter.fullPath() == fullPath);
            ++iter;
        }
    }
}