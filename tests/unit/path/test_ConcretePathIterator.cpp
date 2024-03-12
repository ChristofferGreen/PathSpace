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
        for(auto const p : path) {
            if(i==0) REQUIRE(p=="a");
            else if(i==1) REQUIRE(p=="b");
            else if(i==2) REQUIRE(p=="c");
            else REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("ForEach Name Iteration Long") {
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
}
