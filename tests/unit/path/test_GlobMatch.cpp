#include "path/GlobName.hpp"
#include "path/GlobPath.hpp"
#include "path/ConcretePath.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("path.glob.match") {
TEST_CASE("GlobName matches complex patterns") {
    // ** should supermatch anything
    GlobName any{"**"};
    auto [m1, super1] = any.match(std::string_view{"anything"});
    CHECK(m1);
    CHECK(super1);

    // Escaped wildcard
    GlobName esc{"a\\*b"};
    auto [m2, super2] = esc.match(std::string_view{"a*b"});
    CHECK(m2);
    CHECK_FALSE(super2);

    // Character class simple positive
    GlobName cls{"[ab]z"};
    auto [m3, super3] = cls.match(std::string_view{"bz"});
    CHECK(m3);
    CHECK_FALSE(super3);

    // Trailing '*' should match remaining characters without supermatch
    GlobName trailing{"foo*"};
    auto [m4, super4] = trailing.match(std::string_view{"foobar"});
    CHECK(m4);
    CHECK_FALSE(super4);
}

TEST_CASE("GlobName character classes and escapes edge cases") {
    GlobName inverted{"[!a-c]z"};
    auto [m1, super1] = inverted.match(std::string_view{"dz"});
    CHECK(m1);
    CHECK_FALSE(super1);

    auto [m2, super2] = inverted.match(std::string_view{"az"});
    CHECK_FALSE(m2);
    CHECK_FALSE(super2);

    GlobName range{"[a-c]?*"};
    auto [m3, super3] = range.match(std::string_view{"b12"});
    CHECK(m3);
    CHECK_FALSE(super3);

    GlobName unterminated{"[abc"};
    auto [m4, super4] = unterminated.match(std::string_view{"a"});
    CHECK_FALSE(m4);
    CHECK_FALSE(super4);
}

TEST_CASE("GlobName comparisons and negative matches") {
    GlobName exact{"alpha"};
    GlobName same{"alpha"};
    CHECK((exact <=> same) == std::strong_ordering::equal);
    CHECK(exact == same);
    CHECK(exact == "alpha");
    CHECK(exact.isConcrete());
    CHECK_FALSE(GlobName{"*"}.isConcrete());
    CHECK(GlobName{"*"} .isGlob());

    // Star search that never finds the following token should fail.
    auto [miss, super] = GlobName{"*z"}.match(std::string_view{"abc"});
    CHECK_FALSE(miss);
    CHECK_FALSE(super);

    // Character class with empty input should hit early return.
    auto [emptyMatch, emptySuper] = GlobName{"[ab]"}.match(std::string_view{""});
    CHECK_FALSE(emptyMatch);
    CHECK_FALSE(emptySuper);
}

TEST_CASE("GlobPath supermatch with **") {
    GlobPathString glob{"/root/**"};
    ConcretePathString concrete{"/root/a/b/c"};
    CHECK(glob == concrete);

    GlobPathStringView gv{"/root/*/c"};
    ConcretePathStringView cv{"/root/b/c"};
    CHECK(gv == cv);
    CHECK(gv.isGlob());
    CHECK_FALSE(GlobPathString{"/root/a"}.isGlob());

    GlobPathString middleStar{"/root/*/leaf"};
    ConcretePathString deeper{"/root/a/b/leaf"};
    CHECK_FALSE(middleStar == deeper); // single '*' should not cross components

    GlobPathString middleSuper{"/root/**/leaf"};
    ConcretePathString deepMatch{"/root/a/b/leaf"};
    CHECK(middleSuper == deepMatch);
}

TEST_CASE("GlobPath and ConcretePath equality overloads") {
    GlobPathString glob{"/foo/bar"};
    ConcretePathString concrete{"/foo/bar"};
    CHECK(glob == concrete);
    CHECK(concrete == "/foo/bar");
    CHECK(concrete.canonicalized().has_value());

    GlobPathString extraComponent{"/foo/bar/baz"};
    CHECK_FALSE(extraComponent == concrete); // glob longer than concrete without supermatch
}
}
