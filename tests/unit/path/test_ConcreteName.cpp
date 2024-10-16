#include "ext/doctest.h"
#include <pathspace/path/ConcreteName.hpp>
#include <string>
#include <string_view>

using namespace SP;

TEST_CASE("Path ConcreteName") {
    SUBCASE("Construction and Basic Functionality") {
        SUBCASE("Default Construction") {
            ConcreteName<std::string> name;
            CHECK(name.getName().empty());
        }

        SUBCASE("Construction from C-string") {
            ConcreteName<std::string> name("test");
            CHECK(name.getName() == "test");
        }

        SUBCASE("Construction from std::string") {
            std::string str = "example";
            ConcreteName<std::string> name(str);
            CHECK(name.getName() == "example");
        }

        SUBCASE("Construction from std::string_view") {
            std::string_view sv = "view_test";
            ConcreteName<std::string_view> name(sv);
            CHECK(name.getName() == "view_test");
        }

        SUBCASE("Construction from iterators") {
            std::string str = "iterator_test";
            ConcreteName<std::string> name(str.begin(), str.end());
            CHECK(name.getName() == "iterator_test");
        }
    }

    SUBCASE("Comparison Operations") {
        SUBCASE("Equality") {
            ConcreteName<std::string> name1("test");
            ConcreteName<std::string> name2("test");
            ConcreteName<std::string> name3("different");

            CHECK(name1 == name2);
            CHECK_FALSE(name1 == name3);
        }

        SUBCASE("Inequality") {
            ConcreteName<std::string> name1("test");
            ConcreteName<std::string> name2("different");

            CHECK(name1 != name2);
            CHECK_FALSE(name1 != name1);
        }

        SUBCASE("Less Than") {
            ConcreteName<std::string> name1("abc");
            ConcreteName<std::string> name2("def");

            CHECK(name1 < name2);
            CHECK_FALSE(name2 < name1);
        }

        SUBCASE("Greater Than") {
            ConcreteName<std::string> name1("xyz");
            ConcreteName<std::string> name2("abc");

            CHECK(name1 > name2);
            CHECK_FALSE(name2 > name1);
        }

        SUBCASE("Less Than or Equal") {
            ConcreteName<std::string> name1("abc");
            ConcreteName<std::string> name2("abc");
            ConcreteName<std::string> name3("def");

            CHECK(name1 <= name2);
            CHECK(name1 <= name3);
            CHECK_FALSE(name3 <= name1);
        }

        SUBCASE("Greater Than or Equal") {
            ConcreteName<std::string> name1("xyz");
            ConcreteName<std::string> name2("xyz");
            ConcreteName<std::string> name3("abc");

            CHECK(name1 >= name2);
            CHECK(name1 >= name3);
            CHECK_FALSE(name3 >= name1);
        }
    }

    SUBCASE("Comparison with C-string") {
        ConcreteName<std::string> name("test");

        CHECK(name == "test");
        CHECK(name != "different");
        CHECK_FALSE(name == "different");
        CHECK_FALSE(name != "test");
    }

    SUBCASE("Type Traits and Concepts") {
        SUBCASE("Standard Layout") {
            CHECK(std::is_standard_layout_v<ConcreteName<std::string>>);
            CHECK(std::is_standard_layout_v<ConcreteName<std::string_view>>);
        }

        SUBCASE("Trivially Copyable") {
            CHECK_FALSE(std::is_trivially_copyable_v<ConcreteName<std::string>>);
            CHECK(std::is_trivially_copyable_v<ConcreteName<std::string_view>>);
        }
    }

    SUBCASE("Edge Cases") {
        SUBCASE("Empty Name") {
            ConcreteName<std::string> name("");
            CHECK(name.getName().empty());
        }

        SUBCASE("Name with Special Characters") {
            ConcreteName<std::string> name("!@#$%^&*()");
            CHECK(name.getName() == "!@#$%^&*()");
        }

        SUBCASE("Name with Spaces") {
            ConcreteName<std::string> name("name with spaces");
            CHECK(name.getName() == "name with spaces");
        }

        SUBCASE("Unicode Characters") {
            ConcreteName<std::string> name("こんにちは");
            CHECK(name.getName() == "こんにちは");
        }
    }

    SUBCASE("Performance") {
        SUBCASE("Large Name") {
            std::string large_name(1000000, 'a'); // 1 million characters
            ConcreteName<std::string> name(large_name);
            CHECK(name.getName().length() == 1000000);
        }
    }

    SUBCASE("Const Correctness") {
        const ConcreteName<std::string> const_name("const_test");
        CHECK(const_name.getName() == "const_test");
    }

    SUBCASE("ConcreteName with std::string_view") {
        SUBCASE("Construction and Basic Operations") {
            std::string backing_string = "test_string";
            std::string_view sv = backing_string;
            ConcreteName<std::string_view> name(sv);

            // Test equality with the original string_view
            CHECK(name == sv);

            // Test comparison with C-string
            CHECK(name == "test_string");

            // Test inequality
            CHECK(name != "different_string");
        }

        SUBCASE("Move Operations") {
            std::string backing_string = "move_test";
            std::string_view sv = backing_string;

            SUBCASE("Move Construction") {
                ConcreteName<std::string_view> name1(std::move(sv));

                // After move, sv should still be valid and unchanged
                CHECK(sv == "move_test");
                CHECK(name1 == "move_test");
                CHECK(sv.data() == backing_string.data());
            }

            SUBCASE("Move Assignment") {
                ConcreteName<std::string_view> name1("first");
                ConcreteName<std::string_view> name2(sv);

                name1 = std::move(name2);

                // name1 should now hold "move_test", name2 is in a valid but unspecified state
                CHECK(name1 == "move_test");

                // We can't make assumptions about the content of name2 after move,
                // but we can check that it's in a valid state by comparing it to itself
                CHECK(name2 == name2);
            }
        }
    }

    SUBCASE("Hash Function") {
        ConcreteName<std::string> name1("test");
        ConcreteName<std::string> name2("test");
        ConcreteName<std::string> name3("different");

        std::hash<ConcreteName<std::string>> hasher;
        CHECK(hasher(name1) == hasher(name2));
        CHECK(hasher(name1) != hasher(name3));
    }
}