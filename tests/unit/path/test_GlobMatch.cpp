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
}

TEST_CASE("GlobPath and ConcretePath equality overloads") {
    GlobPathString glob{"/foo/bar"};
    ConcretePathString concrete{"/foo/bar"};
    CHECK(glob == concrete);
    CHECK(concrete == "/foo/bar");
    CHECK(concrete.canonicalized().has_value());
}
}
