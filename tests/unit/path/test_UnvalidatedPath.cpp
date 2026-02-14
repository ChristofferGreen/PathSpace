#include "third_party/doctest.h"

#include <pathspace/path/UnvalidatedPath.hpp>

#include <string>

using namespace SP;

TEST_SUITE("path.unvalidated") {

TEST_CASE("inline accessors reflect path properties") {
    UnvalidatedPathView root{"/"};
    CHECK(root.raw() == "/");
    CHECK_FALSE(root.empty());
    CHECK(root.is_absolute());
    CHECK(root.has_trailing_slash());

    UnvalidatedPathView relative{"relative/path"};
    CHECK_FALSE(relative.empty());
    CHECK_FALSE(relative.is_absolute());
    CHECK_FALSE(relative.has_trailing_slash());

    UnvalidatedPathView empty{""};
    CHECK(empty.empty());
    CHECK_FALSE(empty.is_absolute());
    CHECK_FALSE(empty.has_trailing_slash());
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

TEST_CASE("canonicalize_absolute preserves valid absolute paths") {
    UnvalidatedPathView single{"/alpha"};
    auto singleCanonical = single.canonicalize_absolute();
    REQUIRE(singleCanonical.has_value());
    CHECK(*singleCanonical == "/alpha");

    UnvalidatedPathView nested{"/alpha/beta"};
    auto nestedCanonical = nested.canonicalize_absolute();
    REQUIRE(nestedCanonical.has_value());
    CHECK(*nestedCanonical == "/alpha/beta");
}

TEST_CASE("canonicalize_absolute rejects relative and dot segments") {
    UnvalidatedPathView rel{"relative/path"};
    auto relResult = rel.canonicalize_absolute();
    CHECK_FALSE(relResult.has_value());

    UnvalidatedPathView dotted{"/alpha/./beta"};
    auto dottedResult = dotted.canonicalize_absolute();
    CHECK_FALSE(dottedResult.has_value());

    UnvalidatedPathView emptyComponent{"/alpha//beta"};
    auto emptyResult = emptyComponent.canonicalize_absolute();
    CHECK_FALSE(emptyResult.has_value());

    UnvalidatedPathView onlyRoot{"/"};
    auto rootResult = onlyRoot.canonicalize_absolute();
    CHECK_FALSE(rootResult.has_value());
}

TEST_CASE("contains_relative_tokens detects dot segments") {
    UnvalidatedPathView rel{"scenes/../main"};
    CHECK(rel.contains_relative_tokens());

    UnvalidatedPathView emptyComponent{"double//slash"};
    CHECK(emptyComponent.contains_relative_tokens());

    UnvalidatedPathView trailingSlash{"/alpha/beta/"};
    CHECK(trailingSlash.contains_relative_tokens());

    UnvalidatedPathView root{"/"};
    CHECK(root.contains_relative_tokens());
}

TEST_CASE("contains_relative_tokens returns false for clean paths") {
    UnvalidatedPathView clean{"stable/releases/v1"};
    CHECK_FALSE(clean.contains_relative_tokens());

    UnvalidatedPathView single{"component"};
    CHECK_FALSE(single.contains_relative_tokens());

    UnvalidatedPathView empty{""};
    CHECK_FALSE(empty.contains_relative_tokens());
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

TEST_CASE("split_absolute_components rejects dot segments") {
    UnvalidatedPathView dot{"/alpha/./beta"};
    auto dotResult = dot.split_absolute_components();
    CHECK_FALSE(dotResult.has_value());

    UnvalidatedPathView dotdot{"/alpha/../beta"};
    auto dotdotResult = dotdot.split_absolute_components();
    CHECK_FALSE(dotdotResult.has_value());
}

TEST_CASE("split_absolute_components reports error details") {
    UnvalidatedPathView root{"/"};
    auto rootResult = root.split_absolute_components();
    REQUIRE_FALSE(rootResult.has_value());
    CHECK(rootResult.error().code == Error::Code::InvalidPath);
    CHECK(rootResult.error().message == std::optional<std::string>{"path must contain at least one component"});

    UnvalidatedPathView rel{"relative"};
    auto relResult = rel.split_absolute_components();
    REQUIRE_FALSE(relResult.has_value());
    CHECK(relResult.error().code == Error::Code::InvalidPath);
    CHECK(relResult.error().message == std::optional<std::string>{"path must be absolute"});
}

TEST_CASE("split_absolute_components reports subcomponent error details") {
    UnvalidatedPathView emptyComponent{"/alpha//beta"};
    auto emptyResult = emptyComponent.split_absolute_components();
    REQUIRE_FALSE(emptyResult.has_value());
    CHECK(emptyResult.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(emptyResult.error().message == std::optional<std::string>{"empty path component"});

    UnvalidatedPathView dot{"/alpha/./beta"};
    auto dotResult = dot.split_absolute_components();
    REQUIRE_FALSE(dotResult.has_value());
    CHECK(dotResult.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(dotResult.error().message == std::optional<std::string>{"relative path components are not allowed"});

    UnvalidatedPathView dotdot{"/alpha/../beta"};
    auto dotdotResult = dotdot.split_absolute_components();
    REQUIRE_FALSE(dotdotResult.has_value());
    CHECK(dotdotResult.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(dotdotResult.error().message == std::optional<std::string>{"relative path components are not allowed"});
}

TEST_CASE("canonicalize_absolute forwards subcomponent errors") {
    UnvalidatedPathView emptyComponent{"/alpha//beta"};
    auto result = emptyComponent.canonicalize_absolute();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
    CHECK(result.error().message == std::optional<std::string>{"empty path component"});
}

} // TEST_SUITE
