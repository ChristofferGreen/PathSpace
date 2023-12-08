#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/GlobName.hpp>

using namespace SP;

TEST_CASE("GlobName", "[GlobName]") {
    SECTION("Simple Glob Patterns") {
        GlobName name{"testABC"};
        GlobName pattern{"test*"};
        GlobName pattern2{"test1*"};
        GlobName pattern3{"test?BC"};
        GlobName pattern4{"test?BD"};
        REQUIRE(pattern==name);
        REQUIRE(pattern2!=name);
        REQUIRE(pattern3==name);
        REQUIRE(pattern4!=name);
        REQUIRE(name!=pattern4);
    }

    SECTION("Glob Pattern Matches Itself") {
        GlobName pattern{"test*"};
        GlobName pattern2{"test**"};
        REQUIRE(pattern==pattern);
        REQUIRE(pattern2!=pattern);
    }

    SECTION("Empty Glob") {
        GlobName name{"test"};
        GlobName pattern{""};
        REQUIRE(name!=pattern);
    }

    SECTION("Empty Name Against Glob") {
        GlobName name{""};
        GlobName pattern{"*"};
        REQUIRE(name==pattern);
    }
}