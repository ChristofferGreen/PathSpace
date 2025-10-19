#include "third_party/doctest.h"
#include "path/Iterator.hpp"
#include "path/utils.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace SP;

TEST_CASE("Path Iterator and Utilities") {
    SUBCASE("Path Iterator Basic Operations") {
        SUBCASE("Root Path") {
            Iterator iter("/");
            CHECK(iter.isAtEnd());
            CHECK(iter.isAtStart());
            CHECK(iter.toStringView() == "/");
        }

        SUBCASE("Simple Path") {
            Iterator iter("/simple/path");
            CHECK(iter.isAtStart());
            CHECK(*iter == "simple");
            ++iter;
            CHECK(*iter == "path");
            ++iter;
            CHECK(iter.isAtEnd());
        }

        SUBCASE("Multiple Consecutive Slashes") {
            Iterator                 iter("///a////b///c//");
            std::vector<std::string> components;
            for (auto it = iter; !it.isAtEnd(); ++it) {
                components.push_back(std::string(*it));
            }

            REQUIRE(components.size() == 3);
            CHECK(components[0] == "a");
            CHECK(components[1] == "b");
            CHECK(components[2] == "c");
        }

        SUBCASE("Path with Single Component") {
            Iterator iter("/component");
            CHECK(*iter == "component");
            ++iter;
            CHECK(iter.isAtEnd());
        }

        SUBCASE("Iterator Increment") {
            Iterator iter("/a/b/c");
            auto     it = iter;
            CHECK(*it == "a");
            ++it;
            CHECK(*it == "b");
            ++it;
            CHECK(*it == "c");
            ++it;
            CHECK(it.isAtEnd());
        }

        SUBCASE("Full Path Access") {
            const std::string path = "/test/path/here";
            Iterator          iter(path);
            CHECK(iter.toStringView() == path);
            ++iter;
            CHECK(iter.toStringView() == path);
        }
    }

    SUBCASE("Path Iterator State Tracking") {
        SUBCASE("Start Position") {
            Iterator iter("/path/to/somewhere");
            CHECK(iter.isAtStart());
            ++iter;
            CHECK_FALSE(iter.isAtStart());
        }

        SUBCASE("End Position") {
            Iterator iter("/path");
            CHECK_FALSE(iter.isAtEnd());
            ++iter;
            CHECK(iter.isAtEnd());
        }

        SUBCASE("State Through Iteration") {
            Iterator iter("/a/b/c");
            CHECK(iter.isAtStart());
            CHECK_FALSE(iter.isAtEnd());

            ++iter;
            CHECK_FALSE(iter.isAtStart());
            CHECK_FALSE(iter.isAtEnd());

            ++iter;
            CHECK_FALSE(iter.isAtStart());
            CHECK_FALSE(iter.isAtEnd());

            ++iter;
            CHECK_FALSE(iter.isAtStart());
            CHECK(iter.isAtEnd());
        }
    }

    SUBCASE("Path Utilities") {
        SUBCASE("match_names Basic") {
            CHECK(match_names("test", "test"));
            CHECK_FALSE(match_names("test", "Test"));
            CHECK_FALSE(match_names("test", "testing"));
            CHECK_FALSE(match_names("testing", "test"));
        }

        SUBCASE("match_names Wildcards") {
            CHECK(match_names("*", "anything"));
            CHECK(match_names("test*", "testing"));
            CHECK(match_names("*test", "mytest"));
            CHECK(match_names("*test*", "mytesting"));
            CHECK_FALSE(match_names("test*", "tost"));
        }

        SUBCASE("match_names Question Mark") {
            CHECK(match_names("t?st", "test"));
            CHECK(match_names("te??", "test"));
            CHECK_FALSE(match_names("te?t", "test!"));
            CHECK_FALSE(match_names("tes?", "te"));
        }

        SUBCASE("match_names Character Classes") {
            CHECK(match_names("[abc]test", "atest"));
            CHECK(match_names("[a-z]test", "xtest"));
            CHECK(match_names("test[0-9]", "test5"));
            CHECK_FALSE(match_names("[a-z]test", "1test"));
            CHECK_FALSE(match_names("test[0-9]", "testa"));
        }

        SUBCASE("match_names Negated Character Classes") {
            CHECK(match_names("[!a]test", "btest"));
            CHECK(match_names("[!0-9]test", "atest"));
            CHECK_FALSE(match_names("[!a]test", "atest"));
            CHECK_FALSE(match_names("[!0-9]test", "1test"));
        }

        SUBCASE("match_names Escaped Characters") {
            CHECK(match_names("\\*test", "*test"));
            CHECK(match_names("test\\?", "test?"));
            CHECK(match_names("\\[test\\]", "[test]"));
            CHECK_FALSE(match_names("\\*test", "atest"));
        }

        SUBCASE("match_paths") {
            CHECK(match_paths("/test/path", "/test/path"));
            CHECK_FALSE(match_paths("/test/path", "/test/other"));
            CHECK(match_paths("/test/*/end", "/test/middle/end"));
            CHECK(match_paths("/test/?/end", "/test/x/end"));
            CHECK_FALSE(match_paths("/test/*/end", "/test/too/many/end"));
        }

        SUBCASE("is_concrete") {
            CHECK(is_concrete("/normal/path"));
            CHECK(is_concrete("/path/with/numbers/123"));
            CHECK_FALSE(is_concrete("/path/*/wildcard"));
            CHECK_FALSE(is_concrete("/path/?/question"));
            CHECK_FALSE(is_concrete("/path/[a-z]/range"));
        }

        SUBCASE("is_glob") {
            CHECK(is_glob("/path/*/wildcard"));
            CHECK(is_glob("/path/?/question"));
            CHECK(is_glob("/path/[a-z]/range"));
            CHECK_FALSE(is_glob("/normal/path"));
            CHECK_FALSE(is_glob("/path/with/escaped\\*"));
        }
    }

    SUBCASE("Edge Cases") {
        SUBCASE("Paths with Special Characters") {
            Iterator                 iter("/path/with spaces/and-dashes/under_scores");
            std::vector<std::string> components;
            for (auto it = iter; !it.isAtEnd(); ++it) {
                components.push_back(std::string(*it));
            }

            REQUIRE(components.size() == 4);
            CHECK(components[0] == "path");
            CHECK(components[1] == "with spaces");
            CHECK(components[2] == "and-dashes");
            CHECK(components[3] == "under_scores");
        }

        SUBCASE("Very Long Path Components") {
            std::string longComponent(1000, 'a');
            std::string path = "/" + longComponent;
            Iterator    iter(path);
            CHECK(std::string(*iter) == longComponent);
        }

        SUBCASE("Pattern Matching Complex Cases") {
            // Multiple wildcards
            CHECK(match_names("*test*ing*", "teststring"));

            // Complex character classes
            CHECK(match_names("[a-zA-Z][0-9]", "A5"));
            CHECK(match_names("[!a-z][!0-9]", "A$"));

            // Mixed patterns
            CHECK(match_names("*st[0-9]*st[a-z]", "test5testt"));
            CHECK(match_names("[a-z]*st[0-9]?", "ttest52"));
        }

        SUBCASE("Empty Components") {
            Iterator                 iter("/a//b///c");
            std::vector<std::string> components;
            for (auto it = iter; !it.isAtEnd(); ++it) {
                components.push_back(std::string(*it));
            }

            REQUIRE(components.size() == 3);
            CHECK(components[0] == "a");
            CHECK(components[1] == "b");
            CHECK(components[2] == "c");
        }
    }

    SUBCASE("Iterator Memory Safety") {
        SUBCASE("Temporary String") {
            auto iter = Iterator(std::string("/temp/path"));
            CHECK(*iter == "temp");
            ++iter;
            CHECK(*iter == "path");
        }

        SUBCASE("String View Lifetime") {
            std::string path = "/test/path";
            auto        iter = Iterator(std::string_view(path));
            CHECK(*iter == "test");
            ++iter;
            CHECK(*iter == "path");
        }
    }

    SUBCASE("Pattern Matching Performance") {
        SUBCASE("Large Number of Matches") {
            std::string              pattern = "[a-z]*st[0-9]*";
            std::vector<std::string> testCases;
            for (int i = 0; i < 1000; ++i) {
                testCases.push_back("test" + std::to_string(i));
            }

            for (const auto& test : testCases) {
                CHECK(match_names(pattern, test));
            }
        }

        SUBCASE("Complex Pattern") {
            std::string pattern = "*st[a-z][0-9][A-Z]*z[!0-9]?";
            CHECK(match_names(pattern, "testa5Bxyzti"));
            CHECK(match_names(pattern, "bsty7Cxzya"));
            CHECK_FALSE(match_names(pattern, "invalid"));
        }
    }
}