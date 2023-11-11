#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/SpacePath.hpp>

using namespace SP;

TEST_CASE("SpacePath default constructor initializes to empty path", "[SpacePath]") {
    SpacePath sp;
    REQUIRE(sp.toString() == "");
}

TEST_CASE("SpacePath constructor initializes with given path", "[SpacePath]") {
    SpacePath sp("/path/to/node");
    REQUIRE(sp.toString() == "/path/to/node");
}

TEST_CASE("SpacePath does not match different path", "[SpacePath]") {
    SpacePath sp("/path/to/node");
    REQUIRE(sp.toString() != "/path/to/another_node");
}
