#include "third_party/doctest.h"

#include <pathspace/path/UnvalidatedPath.hpp>

using namespace SP;

TEST_SUITE("path.unvalidated") {

TEST_CASE("canonicalize_absolute trims trailing slash") {
    UnvalidatedPathView raw{"/system/applications/demo/"};
    auto canonical = raw.canonicalize_absolute();
    REQUIRE(canonical.has_value());
    CHECK(*canonical == "/system/applications/demo");
}

TEST_CASE("contains_relative_tokens detects dot segments") {
    UnvalidatedPathView rel{"scenes/../main"};
    CHECK(rel.contains_relative_tokens());

    UnvalidatedPathView emptyComponent{"double//slash"};
    CHECK(emptyComponent.contains_relative_tokens());
}

TEST_CASE("split_absolute_components rejects non-absolute") {
    UnvalidatedPathView rel{"scenes/main"};
    auto components = rel.split_absolute_components();
    CHECK_FALSE(components.has_value());

    UnvalidatedPathView empty{""};
    auto emptyResult = empty.split_absolute_components();
    CHECK_FALSE(emptyResult.has_value());

    UnvalidatedPathView root{"/"};
    auto rootResult = root.split_absolute_components();
    CHECK_FALSE(rootResult.has_value());

    UnvalidatedPathView doubleSlash{"/widgets//panel"};
    auto doubleSlashResult = doubleSlash.split_absolute_components();
    CHECK_FALSE(doubleSlashResult.has_value());
}

} // TEST_SUITE
