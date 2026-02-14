#include "path/GlobPath.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

#include <compare>
#include <string_view>

using namespace SP;

TEST_SUITE("path.globpath") {
TEST_CASE("glob equality supports supermatch segments") {
    GlobPathString    superGlob{"/root/**"};
    ConcretePathString deepPath{"/root/child/grandchild"};
    CHECK(superGlob == deepPath);
}

TEST_CASE("glob equality respects path length when no supermatch") {
    GlobPathString glob{"/alpha/beta"};
    ConcretePathString shorter{"/alpha"};
    ConcretePathString longer{"/alpha/beta/gamma"};
    ConcretePathString equal{"/alpha/beta"};
    CHECK_FALSE(glob == shorter);
    CHECK_FALSE(glob == longer);
    CHECK(glob == equal);
}

TEST_CASE("invalid glob paths never match") {
    GlobPathString invalid{"relative"};
    ConcretePathString concrete{"/relative"};
    GlobPathString another{"/relative"};
    CHECK_FALSE(invalid == concrete);
    CHECK_FALSE(invalid == another);
    CHECK_FALSE(invalid.isValid());
}

TEST_CASE("isConcrete and isGlob reflect wildcard usage") {
    GlobPathString concrete{"/plain/path"};
    GlobPathString wildcard{"/plain/*"};
    CHECK(concrete.isConcrete());
    CHECK_FALSE(concrete.isGlob());
    CHECK_FALSE(wildcard.isConcrete());
    CHECK(wildcard.isGlob());

    GlobPathStringView viewConcrete{std::string_view{"/plain/path"}};
    GlobPathStringView viewWildcard{std::string_view{"/plain/*"}};
    CHECK(viewConcrete.isConcrete());
    CHECK_FALSE(viewConcrete.isGlob());
    CHECK_FALSE(viewWildcard.isConcrete());
    CHECK(viewWildcard.isGlob());
}

TEST_CASE("string_view equality delegates to path comparison") {
    GlobPathStringView view{std::string_view{"/alpha/*"}};
    CHECK(view == std::string_view{"/alpha/*"});
    CHECK_FALSE(view == std::string_view{"/alpha/beta/gamma"});
}

TEST_CASE("glob equality compares glob-to-glob paths") {
    GlobPathString globA{"/tree/**"};
    GlobPathString globB{"/tree/**"};
    GlobPathString globMismatch{"/tree/*/leaf"};

    CHECK(globA == globB);
    CHECK_FALSE(globA == globMismatch);

    GlobPathStringView viewA{std::string_view{"/tree/**"}};
    GlobPathStringView viewB{std::string_view{"/tree/**"}};
    GlobPathStringView viewMismatch{std::string_view{"/tree/*/leaf"}};
    CHECK(viewA == viewB);
    CHECK_FALSE(viewA == viewMismatch);
}

TEST_CASE("three-way comparison mirrors underlying path ordering") {
    GlobPathString a{"/a"};
    GlobPathString b{"/b"};

    auto cmp = (a <=> b);
    CHECK(std::is_lt(cmp));

    auto cmpEq = (a <=> GlobPathString{"/a"});
    CHECK(std::is_eq(cmpEq));

    GlobPathStringView viewA{std::string_view{"/a"}};
    GlobPathStringView viewB{std::string_view{"/b"}};
    auto viewCmp = (viewA <=> viewB);
    CHECK(std::is_lt(viewCmp));
}

TEST_CASE("empty and root glob paths expose iterator edges") {
    GlobPathString empty{};
    CHECK_FALSE(empty.isValid());
    CHECK(empty.begin() == empty.end());
    CHECK(empty.isConcrete());
    CHECK_FALSE(empty.isGlob());

    GlobPathString root{"/"};
    CHECK(root.isValid());
    CHECK(root.begin() == root.end());
    CHECK(root.isConcrete());
    CHECK_FALSE(root.isGlob());
}
}
