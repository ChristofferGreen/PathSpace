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

    SECTION("Default Construction With Value", "[Path][ConcretePath]") {
        ConcretePathString path{"/a/b/c"};
        REQUIRE(path=="/a/b/c");
        REQUIRE(path!="/a/b/d");
        ConcretePathStringView path2{"/a/b/c"};
        REQUIRE(path2=="/a/b/c");
        REQUIRE(path2!="/a/b/d");
    }
}
