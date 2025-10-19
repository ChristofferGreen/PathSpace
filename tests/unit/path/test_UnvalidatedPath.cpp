#include "third_party/doctest.h"

#include <pathspace/path/UnvalidatedPath.hpp>

using namespace SP;

TEST_SUITE("UnvalidatedPathView") {

TEST_CASE("canonicalize_absolute trims trailing slash") {
    UnvalidatedPathView raw{"/system/applications/demo/"};
    auto canonical = raw.canonicalize_absolute();
    REQUIRE(canonical.has_value());
    CHECK(*canonical == "/system/applications/demo");
}

TEST_CASE("contains_relative_tokens detects dot segments") {
    UnvalidatedPathView rel{"scenes/../main"};
    CHECK(rel.contains_relative_tokens());
}

TEST_CASE("split_absolute_components rejects non-absolute") {
    UnvalidatedPathView rel{"scenes/main"};
    auto components = rel.split_absolute_components();
    CHECK_FALSE(components.has_value());
}

} // TEST_SUITE

