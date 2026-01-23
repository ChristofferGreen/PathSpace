#include "third_party/doctest.h"

#include <pathspace/path/UnvalidatedPath.hpp>

using namespace SP;

TEST_SUITE("path.unvalidated") {

TEST_CASE("inline accessors reflect path properties") {
    UnvalidatedPathView root{"/"};
    CHECK(root.raw() == "/");
    CHECK(root.is_absolute());
    CHECK(root.has_trailing_slash());

    UnvalidatedPathView relative{"relative/path"};
    CHECK_FALSE(relative.is_absolute());
    CHECK_FALSE(relative.has_trailing_slash());
}

TEST_CASE("split_absolute_components returns components and trims trailing slash") {
    UnvalidatedPathView path{"/alpha/beta/gamma/"};
    auto components = path.split_absolute_components();
    REQUIRE(components.has_value());
    CHECK(components->size() == 3);
    CHECK((*components)[0] == "alpha");
    CHECK((*components)[1] == "beta");
    CHECK((*components)[2] == "gamma");
}

TEST_CASE("canonicalize_absolute trims trailing slash") {
    UnvalidatedPathView raw{"/system/applications/demo/"};
    auto canonical = raw.canonicalize_absolute();
    REQUIRE(canonical.has_value());
    CHECK(*canonical == "/system/applications/demo");
}

TEST_CASE("canonicalize_absolute rejects relative and dot segments") {
    UnvalidatedPathView rel{"relative/path"};
    auto relResult = rel.canonicalize_absolute();
    CHECK_FALSE(relResult.has_value());

    UnvalidatedPathView dotted{"/alpha/./beta"};
    auto dottedResult = dotted.canonicalize_absolute();
    CHECK_FALSE(dottedResult.has_value());
}

TEST_CASE("contains_relative_tokens detects dot segments") {
    UnvalidatedPathView rel{"scenes/../main"};
    CHECK(rel.contains_relative_tokens());

    UnvalidatedPathView emptyComponent{"double//slash"};
    CHECK(emptyComponent.contains_relative_tokens());
}

TEST_CASE("contains_relative_tokens returns false for clean paths") {
    UnvalidatedPathView clean{"stable/releases/v1"};
    CHECK_FALSE(clean.contains_relative_tokens());

    UnvalidatedPathView single{"component"};
    CHECK_FALSE(single.contains_relative_tokens());
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
