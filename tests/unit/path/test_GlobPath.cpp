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

    SUBCASE("Double Wildcard Match") {
        GlobPathStringView     sp1{"/a/**"};
        ConcretePathStringView sp2{"/a/b/c"};
        bool                   b = sp1 == sp2;
        REQUIRE(sp1 == sp2);

        GlobPathStringView     sp3{"/a/**/c"};
        ConcretePathStringView sp4{"/a/b/d/c"};
        REQUIRE(sp3 == sp4);
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
        CHECK(std::get<0>(name.match("01"sv)));
        CHECK(std::get<0>(name.match("02"sv)));
        CHECK(!std::get<0>(name.match("03"sv)));
    }

    SUBCASE("Basic Matching") {
        GlobName glob{"simple"};
        CHECK(std::get<0>(glob.match("simple"sv)));
        CHECK_FALSE(std::get<0>(glob.match("other"sv)));
    }

    SUBCASE("Single Character Wildcard") {
        GlobName glob{"t?st"};
        CHECK(std::get<0>(glob.match("test"sv)));
        CHECK(std::get<0>(glob.match("tast"sv)));
        CHECK_FALSE(std::get<0>(glob.match("tests"sv)));
    }

    SUBCASE("Multi Character Wildcard") {
        GlobName glob{"test*"};
        CHECK(std::get<0>(glob.match("test"sv)));
        CHECK(std::get<0>(glob.match("tests"sv)));
        CHECK(std::get<0>(glob.match("testing"sv)));
        CHECK_FALSE(std::get<0>(glob.match("tes"sv)));
    }

    SUBCASE("Character Range") {
        GlobName glob{"[a-c]at"};
        CHECK(std::get<0>(glob.match("bat"sv)));
        CHECK(std::get<0>(glob.match("cat"sv)));
        CHECK_FALSE(std::get<0>(glob.match("dat"sv)));
    }

    SUBCASE("Numerical Range") {
        GlobName glob{"[0-9]"};
        for (char i = '0'; i <= '9'; ++i) {
            INFO("Testing digit: ", i);
            auto s = std::string(1, i);
            CHECK(std::get<0>(glob.match(std::string_view{s})));
        }
        CHECK_FALSE(std::get<0>(glob.match("a"sv)));
    }

    SUBCASE("Specific Numerical Range") {
        GlobName glob{"[1-3]"};
        CHECK(std::get<0>(glob.match("1"sv)));
        CHECK(std::get<0>(glob.match("2"sv)));
        CHECK(std::get<0>(glob.match("3"sv)));
        CHECK_FALSE(std::get<0>(glob.match("0"sv)));
        CHECK_FALSE(std::get<0>(glob.match("4"sv)));
    }

    SUBCASE("Range with Prefix") {
        GlobName glob{"test[1-3]"};
        CHECK(std::get<0>(glob.match("test1"sv)));
        CHECK(std::get<0>(glob.match("test2"sv)));
        CHECK(std::get<0>(glob.match("test3"sv)));
        CHECK_FALSE(std::get<0>(glob.match("test4"sv)));
    }

    SUBCASE("Multiple Character Classes") {
        GlobName glob{"[a-c][1-3]"};
        CHECK(std::get<0>(glob.match("a1"sv)));
        CHECK(std::get<0>(glob.match("b2"sv)));
        CHECK(std::get<0>(glob.match("c3"sv)));
        CHECK_FALSE(std::get<0>(glob.match("d1"sv)));
        CHECK_FALSE(std::get<0>(glob.match("a4"sv)));
    }

    SUBCASE("Negated Character Class") {
        GlobName glob{"[!a-c]at"};
        CHECK(std::get<0>(glob.match("dat"sv)));
        CHECK(std::get<0>(glob.match("eat"sv)));
        CHECK_FALSE(std::get<0>(glob.match("bat"sv)));
    }

    SUBCASE("Escaped Characters") {
        GlobName glob{"test\\*"};
        CHECK(std::get<0>(glob.match("test*"sv)));
        CHECK_FALSE(std::get<0>(glob.match("tests"sv)));
    }

    SUBCASE("Complex Pattern") {
        GlobName glob{"[a-z][0-9]?[!0-9]"};
        CHECK(std::get<0>(glob.match("a1xt"sv)));
        CHECK(std::get<0>(glob.match("b2ys"sv)));
        CHECK_FALSE(std::get<0>(glob.match("a111"sv)));
        CHECK_FALSE(std::get<0>(glob.match("11x1"sv)));
    }

    SUBCASE("Empty Pattern") {
        GlobName glob{""};
        CHECK(std::get<0>(glob.match(""sv)));
        CHECK_FALSE(std::get<0>(glob.match("a"sv)));
    }

    SUBCASE("Pattern with Numeric Prefix") {
        GlobName glob{"0[1-2]"};
        CHECK(std::get<0>(glob.match("01"sv)));
        CHECK(std::get<0>(glob.match("02"sv)));
        CHECK_FALSE(std::get<0>(glob.match("03"sv)));
        CHECK_FALSE(std::get<0>(glob.match("00"sv)));
    }

    SUBCASE("Pattern with Multiple Numeric Ranges") {
        GlobName glob{"[0-1][2-3]"};
        CHECK(std::get<0>(glob.match("02"sv)));
        CHECK(std::get<0>(glob.match("03"sv)));
        CHECK(std::get<0>(glob.match("12"sv)));
        CHECK(std::get<0>(glob.match("13"sv)));
        CHECK_FALSE(std::get<0>(glob.match("01"sv)));
        CHECK_FALSE(std::get<0>(glob.match("14"sv)));
    }

    SUBCASE("Range Validation") {
        CHECK_FALSE(std::get<0>(GlobName{"[3-1]"}.match("2"sv))); // Invalid range
        CHECK_FALSE(std::get<0>(GlobName{"[a-A]"}.match("b"sv))); // Invalid range
    }

    SUBCASE("Character Set") {
        GlobName glob{"[abc]"};
        CHECK(std::get<0>(glob.match("a"sv)));
        CHECK(std::get<0>(glob.match("b"sv)));
        CHECK(std::get<0>(glob.match("c"sv)));
        CHECK_FALSE(std::get<0>(glob.match("d"sv)));
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

    SUBCASE("Basic Supermatcher") {
        GlobName glob{"**"};
        auto     result = glob.match("anything"sv);
        CHECK(std::get<0>(result)); // Should match
        CHECK(std::get<1>(result)); // Should be a supermatch

        // Should match empty string too
        result = glob.match(""sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should match paths of any length
        result = glob.match("very/long/path/with/many/components"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));
    }

    SUBCASE("Supermatcher with Prefix") {
        GlobName glob{"test/**"};

        // Should match any string starting with "test/"
        auto result = glob.match("test/anything"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match strings not starting with "test/"
        result = glob.match("testing/anything"sv);
        CHECK_FALSE(std::get<0>(result));

        // Should match multiple levels
        result = glob.match("test/level1/level2/level3"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));
    }

    SUBCASE("Supermatcher with Suffix") {
        GlobName glob{"**/end"};

        // Should match any string ending with "/end"
        auto result = glob.match("anything/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should match multiple levels ending with "/end"
        result = glob.match("level1/level2/level3/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match strings not ending with "/end"
        result = glob.match("end/something"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Supermatcher Between Components") {
        GlobName glob{"start/**/end"};

        // Should match direct connection
        auto result = glob.match("start/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should match with intermediate components
        result = glob.match("start/middle/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        result = glob.match("start/level1/level2/level3/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match if missing start or end
        result = glob.match("different/middle/end"sv);
        CHECK_FALSE(std::get<0>(result));

        result = glob.match("start/middle/different"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Multiple Supermatchers") {
        GlobName glob{"**/middle/**"};

        // Should match anything containing "middle"
        auto result = glob.match("start/middle/end"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        result = glob.match("level1/level2/middle/level3/level4"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match if "middle" is missing
        result = glob.match("start/different/end"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Supermatcher with Wildcards") {
        GlobName glob{"test/**/[a-z]*"};

        // Should match paths starting with "test/" and ending with a letter-started component
        auto result = glob.match("test/anything/abc"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match if ending component doesn't start with letter
        result = glob.match("test/anything/123"sv);
        CHECK_FALSE(std::get<0>(result));

        // Should match multiple levels
        result = glob.match("test/level1/level2/abc"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));
    }

    SUBCASE("Nested Supermatchers") {
        GlobName glob{"outer/**/inner/**/final"};

        // Should match with varying levels of nesting
        auto result = glob.match("outer/inner/final"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        result = glob.match("outer/a/b/inner/c/d/final"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should maintain order of fixed components
        result = glob.match("outer/inner/inner/final"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        result = glob.match("outer/final/inner/final"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Supermatcher Edge Cases") {
        // Empty supermatcher
        GlobName emptyGlob{""};
        auto     result = emptyGlob.match(""sv);
        CHECK(std::get<0>(result));
        CHECK_FALSE(std::get<1>(result));

        // Single component with supermatcher
        GlobName singleGlob{"**"};
        result = singleGlob.match("any/path/at/all"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Escaped supermatcher
        GlobName escapedGlob{"\\**"};
        result = escapedGlob.match("**"sv);
        CHECK(std::get<0>(result));
        CHECK_FALSE(std::get<1>(result)); // Not a supermatch because it's escaped

        result = escapedGlob.match("anything_else"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Supermatcher with Character Classes") {
        GlobName glob{"[a-z]**[0-9]"};

        // Should match strings starting with letter and ending with number
        auto result = glob.match("a/anything/here/5"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Should not match if pattern start/end don't match
        result = glob.match("1/anything/here/5"sv);
        CHECK_FALSE(std::get<0>(result));

        result = glob.match("a/anything/here/z"sv);
        CHECK_FALSE(std::get<0>(result));
    }

    SUBCASE("Complex Supermatcher Patterns") {
        GlobName glob{"[a-z][0-9]/**/test[0-9]/**/*[!0-9]"};

        // Should match complex pattern with multiple conditions
        auto result = glob.match("a1/middle/test5/more/endA"sv);
        CHECK(std::get<0>(result));
        CHECK(std::get<1>(result));

        // Test various non-matching cases
        result = glob.match("11/middle/test5/more/endA"sv); // Wrong start
        CHECK_FALSE(std::get<0>(result));

        result = glob.match("a1/middle/test/more/endA"sv); // Wrong test number
        CHECK_FALSE(std::get<0>(result));

        result = glob.match("a1/middle/test5/more/end5"sv); // Wrong end
        CHECK_FALSE(std::get<0>(result));
    }
}