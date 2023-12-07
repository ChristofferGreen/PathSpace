#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/GlobPath.hpp>

using namespace SP;

TEST_CASE("GlobPath", "[GlobPath]") {
    SECTION("Standard Path") {
        GlobPath path{"/a/b/c"};
        REQUIRE(path.toString()=="/a/b/c");
    }

    SECTION("Standard Path Current") {
        GlobPath path{"/a/b/c"};
        auto current = path.currentName();
        REQUIRE(current.has_value());
        REQUIRE(current.value()=="a");

        path.moveToNextName();
        current = path.currentName();
        REQUIRE(current.has_value());
        REQUIRE(current.value()=="b");

        path.moveToNextName();
        current = path.currentName();
        REQUIRE(current.has_value());
        REQUIRE(current.value()=="c");
    }
}