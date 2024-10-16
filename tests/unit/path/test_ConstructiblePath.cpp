#include "ext/doctest.h"
#include <pathspace/path/ConstructiblePath.hpp>
#include <string>
#include <string_view>

using namespace SP;

TEST_CASE("Path ConstructiblePath") {
    SUBCASE("Default Constructor") {
        ConstructiblePath path;
        CHECK(path.getPath() == "/");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("String Constructor") {
        ConstructiblePath path("/home/user");
        CHECK(path.getPath() == "/home/user");
        CHECK(path.isCompleted());
    }

    SUBCASE("String View Constructor") {
        std::string_view sv = "/var/log";
        ConstructiblePath path(sv);
        CHECK(path.getPath() == "/var/log");
        CHECK(path.isCompleted());
    }

    SUBCASE("C-string Constructor") {
        ConstructiblePath path("/etc/config");
        CHECK(path.getPath() == "/etc/config");
        CHECK(path.isCompleted());
    }

    SUBCASE("Path Constructor") {
        Path<std::string> strPath("/usr/local");
        ConstructiblePath path(strPath);
        CHECK(path.getPath() == "/usr/local");
        CHECK(path.isCompleted());
    }

    SUBCASE("Single Append") {
        ConstructiblePath path;
        path.append("home");
        CHECK(path.getPath() == "/home");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Multiple Appends") {
        ConstructiblePath path;
        path.append("home").append("user").append("documents");
        CHECK(path.getPath() == "/home/user/documents");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Append After Completion") {
        ConstructiblePath path;
        path.append("home");
        path.markComplete();
        path.append("user");
        CHECK(path.getPath() == "/home");
        CHECK(path.isCompleted());
    }

    SUBCASE("Reset") {
        ConstructiblePath path("/home/user");
        path.reset();
        CHECK(path.getPath() == "/");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Mark Complete") {
        ConstructiblePath path;
        path.append("home").append("user");
        CHECK_FALSE(path.isCompleted());
        path.markComplete();
        CHECK(path.isCompleted());
    }

    SUBCASE("Equality and Inequality") {
        ConstructiblePath path1("/home/user");
        ConstructiblePath path2("/home/user");
        ConstructiblePath path3("/var/log");

        CHECK(path1 == path2);
        CHECK(path1 != path3);
        CHECK(path1 == "/home/user");
        CHECK("/home/user" == path1);
        CHECK(path1 != "/var/log");

        Path<std::string> strPath("/home/user");
        CHECK(path1 == strPath);
        CHECK(strPath == path1);
    }

    SUBCASE("Three-way Comparison") {
        ConstructiblePath path1("/home/user1");
        ConstructiblePath path2("/home/user2");
        ConstructiblePath path3("/home/user1");

        CHECK((path1 <=> path2) < 0);
        CHECK((path2 <=> path1) > 0);
        CHECK((path1 <=> path3) == 0);
    }

    SUBCASE("Conversion to string_view") {
        ConstructiblePath path("/usr/local/bin");
        std::string_view sv = static_cast<std::string_view>(path);
        CHECK(sv == "/usr/local/bin");
    }

    SUBCASE("Copy Constructor") {
        ConstructiblePath original("/original/path");
        ConstructiblePath copy(original);
        CHECK(copy == original);
    }

    SUBCASE("Move Constructor") {
        ConstructiblePath original("/original/path");
        ConstructiblePath moved(std::move(original));
        CHECK(moved.getPath() == "/original/path");
        CHECK(moved.isCompleted());
    }

    SUBCASE("Copy Assignment") {
        ConstructiblePath original("/original/path");
        ConstructiblePath copy;
        copy = original;
        CHECK(copy == original);
    }

    SUBCASE("Move Assignment") {
        ConstructiblePath original("/original/path");
        ConstructiblePath moved;
        moved = std::move(original);
        CHECK(moved.getPath() == "/original/path");
        CHECK(moved.isCompleted());
    }

    SUBCASE("Append to Root Path") {
        ConstructiblePath path; // This initializes to "/"
        path.append("home");
        CHECK(path.getPath() == "/home");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Append with Trailing Slash (Incomplete Path)") {
        ConstructiblePath path;
        path.append("usr/");
        path.append("local");
        CHECK(path.getPath() == "/usr/local");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Append to Completed Path") {
        ConstructiblePath path("/usr/");
        path.append("local");
        CHECK(path.getPath() == "/usr/"); // Should not change
        CHECK(path.isCompleted());
    }

    SUBCASE("Append Empty String") {
        ConstructiblePath path;
        path.append("");
        CHECK(path.getPath() == "/");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Append with Multiple Slashes") {
        ConstructiblePath path;
        path.append("home///user//");
        CHECK(path.getPath() == "/home///user//");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Construct with Empty String") {
        ConstructiblePath path("");
        CHECK(path.getPath() == "");
        CHECK(path.isCompleted());
    }

    SUBCASE("Append to Path Without Leading Slash") {
        ConstructiblePath path;
        path.append("home");
        path.append("user");
        CHECK(path.getPath() == "/home/user");
        CHECK(!path.isCompleted());
    }

    SUBCASE("Append Path Starting with Slash") {
        ConstructiblePath path;
        path.append("/home");
        path.append("/user");
        CHECK(path.getPath() == "/home/user");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Reset and Append") {
        ConstructiblePath path("/home/user");
        path.reset();
        path.append("var");
        CHECK(path.getPath() == "/var");
        CHECK_FALSE(path.isCompleted());
    }

    SUBCASE("Compare Paths with Different Completion Status") {
        ConstructiblePath path1("/home/user");
        ConstructiblePath path2;
        path2.append("home").append("user");

        CHECK(path1 == path2);
        CHECK(path1.isCompleted() != path2.isCompleted());
    }

    SUBCASE("Construct from Path with Different String Type") {
        Path<std::string_view> svPath("/usr/bin");
        ConstructiblePath path(svPath);
        CHECK(path.getPath() == "/usr/bin");
        CHECK(path.isCompleted());
    }
}