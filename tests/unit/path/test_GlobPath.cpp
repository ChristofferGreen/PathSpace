#include "ext/doctest.h"
#include "path/GlobName.hpp"
#include <pathspace/path/GlobPath.hpp>

#include <set>

using namespace SP;
using namespace std::literals;

TEST_CASE("Path GlobPath") {
    SUBCASE("Basic Iterator Begin") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(*path.begin() == "a");
    }

    SUBCASE("Standard Path") {
        GlobPathStringView path{"/a/b/c"};
        REQUIRE(path == "/a/b/c");
    }

    SUBCASE("Path Foreach") {
        GlobPathStringView path{"/wooo/fooo/dooo"};
        int                i{};
        for (auto const p : path) {
            if (i == 0)
                REQUIRE(p == "wooo");
            else if (i == 1)
                REQUIRE(p == "fooo");
            else if (i == 2)
                REQUIRE(p == "dooo");
            else
                REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("Path Foreach Short") {
        GlobPathStringView path{"/a/b/c"};
        int                i{};
        for (auto const p : path) {
            if (i == 0)
                REQUIRE(p == "a");
            else if (i == 1)
                REQUIRE(p == "b");
            else if (i == 2)
                REQUIRE(p == "c");
            else
                REQUIRE(false);
            ++i;
        }
    }

    SUBCASE("Path Iterator End") {
        GlobPathStringView path{"/a/b/c"};
        auto               iter = path.begin();
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter != path.end());
        ++iter;
        REQUIRE(iter == path.end());
    }

    SUBCASE("Default construction") {
        GlobPathStringView path{"/"};
        bool               a = path == "/";
        REQUIRE(path == "/");
    }

    SUBCASE("Construction with initial path") {
        GlobPathStringView path("/root/child");
        REQUIRE(path == "/root/child");
    }

    SUBCASE("Path does not match different path") {
        GlobPathStringView sp("/path/to/node");
        REQUIRE(sp != "/path/to/another_node");
    }

    GlobPathStringView     wildcardPath("/root/*");
    ConcretePathStringView exactPath("/root/child");
    ConcretePathStringView differentPath("/root/otherChild");

    SUBCASE("Glob matches exact path") {
        REQUIRE(wildcardPath == exactPath);
    }

    SUBCASE("Glob matches different path") {
        REQUIRE(wildcardPath == differentPath);
    }

    SUBCASE("Exact path does not match different path") {
        REQUIRE(exactPath != differentPath);
    }

    SUBCASE("Path matches itself") {
        REQUIRE(exactPath == exactPath);
    }

    SUBCASE("Single Wildcard Match") {
        GlobPathStringView     sp1{"/a/*/c"};
        ConcretePathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 == sp2);
    }

    SUBCASE("Single Wildcard No Match") {
        GlobPathStringView sp1{"/a/*/d"};
        GlobPathStringView sp2{"/a/b/c"};
        REQUIRE(sp1 != sp2);
    }

    SUBCASE("Empty Name") {
        GlobPathStringView sp1{"/a//d"};
        GlobPathStringView sp2{"/a/d"};
        REQUIRE(sp1 == sp2);
    }

    SUBCASE("Glob Match with Special Characters") {
        GlobPathStringView     sp1{"/a/*/c?d"};
        ConcretePathStringView sp2{"/a/b/cxd"};
        REQUIRE(sp1 == sp2);
        GlobPathStringView sp3{"/a/b/c"};
        REQUIRE(sp1 != sp3);
    }

    SUBCASE("Name Containing Wildcard") {
        GlobPathStringView     sp1{"/a/test*"};
        ConcretePathStringView sp2{"/a/testbaab"};
        ConcretePathStringView sp3{"/a/test*"};
        REQUIRE(sp1 == sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
    }

    SUBCASE("Name Containing Wildcard Exact Match") {
        const GlobPathStringView     sp1{"/a/test\\*"};
        const GlobPathStringView     sp2{"/a/testbaab"};
        const ConcretePathStringView sp3{"/a/test*"};
        REQUIRE(sp1 != sp2);
        REQUIRE(sp2 != sp3);
        REQUIRE(sp3 == "/a/test*");
        REQUIRE(sp3 == sp1);
        REQUIRE(sp3 != sp2);
    }

    SUBCASE("Path with No Glob Characters") {
        const GlobPath<std::string> path{"/user/data/file"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Asterisk Glob") {
        const GlobPath<std::string> path{"/user/*/file"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Question Mark Glob") {
        const GlobPath<std::string> path{"/user/data/fil?"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Range Glob") {
        const GlobPath<std::string> path{"/user/data/file[1-3]"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("GlobName with Numerical Range") {
        GlobName name{"0[1-2]"};
        CHECK(name.match("01"sv));
        CHECK(name.match("02"sv));
        CHECK(!name.match("03"sv));
    }

    SUBCASE("Basic Matching") {
        GlobName glob{"simple"};
        CHECK(glob.match("simple"sv));
        CHECK_FALSE(glob.match("other"sv));
    }

    SUBCASE("Single Character Wildcard") {
        GlobName glob{"t?st"};
        CHECK(glob.match("test"sv));
        CHECK(glob.match("tast"sv));
        CHECK_FALSE(glob.match("tests"sv));
    }

    SUBCASE("Multi Character Wildcard") {
        GlobName glob{"test*"};
        CHECK(glob.match("test"sv));
        CHECK(glob.match("tests"sv));
        CHECK(glob.match("testing"sv));
        CHECK_FALSE(glob.match("tes"sv));
    }

    SUBCASE("Character Range") {
        GlobName glob{"[a-c]at"};
        CHECK(glob.match("bat"sv));
        CHECK(glob.match("cat"sv));
        CHECK_FALSE(glob.match("dat"sv));
    }

    SUBCASE("Numerical Range") {
        GlobName glob{"[0-9]"};
        for (char i = '0'; i <= '9'; ++i) {
            INFO("Testing digit: ", i);
            auto s = std::string(1, i);
            CHECK(glob.match(std::string_view{s}));
        }
        CHECK_FALSE(glob.match("a"sv));
    }

    SUBCASE("Specific Numerical Range") {
        GlobName glob{"[1-3]"};
        CHECK(glob.match("1"sv));
        CHECK(glob.match("2"sv));
        CHECK(glob.match("3"sv));
        CHECK_FALSE(glob.match("0"sv));
        CHECK_FALSE(glob.match("4"sv));
    }

    SUBCASE("Range with Prefix") {
        GlobName glob{"test[1-3]"};
        CHECK(glob.match("test1"sv));
        CHECK(glob.match("test2"sv));
        CHECK(glob.match("test3"sv));
        CHECK_FALSE(glob.match("test4"sv));
    }

    SUBCASE("Multiple Character Classes") {
        GlobName glob{"[a-c][1-3]"};
        CHECK(glob.match("a1"sv));
        CHECK(glob.match("b2"sv));
        CHECK(glob.match("c3"sv));
        CHECK_FALSE(glob.match("d1"sv));
        CHECK_FALSE(glob.match("a4"sv));
    }

    SUBCASE("Negated Character Class") {
        GlobName glob{"[!a-c]at"};
        CHECK(glob.match("dat"sv));
        CHECK(glob.match("eat"sv));
        CHECK_FALSE(glob.match("bat"sv));
    }

    SUBCASE("Escaped Characters") {
        GlobName glob{"test\\*"};
        CHECK(glob.match("test*"sv));
        CHECK_FALSE(glob.match("tests"sv));
    }

    SUBCASE("Complex Pattern") {
        GlobName glob{"[a-z][0-9]?[!0-9]"};
        CHECK(glob.match("a1xt"sv));
        CHECK(glob.match("b2ys"sv));
        CHECK_FALSE(glob.match("a111"sv));
        CHECK_FALSE(glob.match("11x1"sv));
    }

    SUBCASE("Empty Pattern") {
        GlobName glob{""};
        CHECK(glob.match(""sv));
        CHECK_FALSE(glob.match("a"sv));
    }

    SUBCASE("Pattern with Numeric Prefix") {
        GlobName glob{"0[1-2]"};
        CHECK(glob.match("01"sv));
        CHECK(glob.match("02"sv));
        CHECK_FALSE(glob.match("03"sv));
        CHECK_FALSE(glob.match("00"sv));
    }

    SUBCASE("Pattern with Multiple Numeric Ranges") {
        GlobName glob{"[0-1][2-3]"};
        CHECK(glob.match("02"sv));
        CHECK(glob.match("03"sv));
        CHECK(glob.match("12"sv));
        CHECK(glob.match("13"sv));
        CHECK_FALSE(glob.match("01"sv));
        CHECK_FALSE(glob.match("14"sv));
    }

    SUBCASE("Range Validation") {
        CHECK_FALSE(GlobName{"[3-1]"}.match("2"sv)); // Invalid range
        CHECK_FALSE(GlobName{"[a-A]"}.match("b"sv)); // Invalid range
    }

    SUBCASE("Character Set") {
        GlobName glob{"[abc]"};
        CHECK(glob.match("a"sv));
        CHECK(glob.match("b"sv));
        CHECK(glob.match("c"sv));
        CHECK_FALSE(glob.match("d"sv));
    }

    SUBCASE("Path with Escaped Glob Characters") {
        const GlobPath<std::string> path{"/user/data/fi\\*le"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Escaped Escape Character") {
        const GlobPath<std::string> path{"/user/data/fi\\\\le"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Mixed Escaped and Unescaped Gobs") {
        const GlobPath<std::string> path{"/user/\\*/fi*le"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Escaped Range Glob") {
        const GlobPath<std::string> path{"/user/data/fi\\[1-3\\]"};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Multiple Glob Patterns") {
        const GlobPath<std::string> path{"/us?er/*/file[0-9]"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Empty Path") {
        const GlobPath<std::string> path{""};
        REQUIRE_FALSE(path.isGlob());
    }

    SUBCASE("Path with Only Glob Characters") {
        const GlobPath<std::string> path{"/*?"};
        REQUIRE(path.isGlob());
    }

    SUBCASE("Path with Only Escaped Glob Characters") {
        const GlobPath<std::string> path{"/\\*\\?"};
        REQUIRE_FALSE(path.isGlob());
    }
}