#include <catch2/catch_test_macros.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/path/ConcretePathIterator.hpp>

#include <set>

using namespace SP;

TEST_CASE("ConcretePathIterator") {
    SECTION("Basic Iterator Begin", "[Path][ConcretePathIterator][ConcreteName]") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SECTION("ForEach Name Iteration Short", "[Path][ConcretePathIterator][ConcreteName]") {
        ConcretePathStringView path{"/a/b/c"};
        REQUIRE(path.isValid());
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="a");
            else if(i==1) REQUIRE(p=="b");
            else if(i==2) REQUIRE(p=="c");
            else REQUIRE(false);
            ++i;
        }
    }

    SECTION("ForEach Name Iteration Long", "[Path][ConcretePathIterator][ConcreteName]") {
        ConcretePathStringView const path{"/woo/Foo/dOoO"};
        REQUIRE(path.isValid());
        int i{};
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="woo");
            else if(i==1) REQUIRE(p=="Foo");
            else if(i==2) REQUIRE(p=="dOoO");
            else REQUIRE(false);
            ++i;
        }
    }

    SECTION("Iterator End", "[Path][ConcretePathIterator][ConcreteName]") {
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

    SECTION("Match Skipped Name", "[Path][ConcretePath][ConcretePathIterator]") {
        ConcretePathString const sp1{"/a//d"};
        ConcretePathString const sp2{"/a/d"};
        REQUIRE(sp1 == sp2);
        ConcretePathString sp3("//a/////d");
        REQUIRE(sp1 == sp3);
        ConcretePathString sp4("//a/////e");
        REQUIRE(sp1 != sp4);
    }

    SECTION("Path With Trailing Slash", "[Path][ConcretePath][ConcretePathIterator]") {
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
}
