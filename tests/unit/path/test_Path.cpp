#include "ext/doctest.h"
#include <pathspace/PathSpace.hpp>

TEST_CASE("Path Validation") {
    SUBCASE("Basic Path Validation") {
        // Valid paths
        CHECK_FALSE(SP::Path<std::string>("/").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/root").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/root/path").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/a/b/c").validate().has_value());

        // Invalid paths
        {
            auto error = SP::Path<std::string>("").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == SP::Error::Code::InvalidPath);
            CHECK(error->message->find("Empty path") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("invalid").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == SP::Error::Code::InvalidPath);
            CHECK(error->message->find("start with '/'") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == SP::Error::Code::InvalidPath);
            CHECK(error->message->find("ends with slash") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("./path").validate();
            REQUIRE(error.has_value());
            CHECK(error->code == SP::Error::Code::InvalidPath);
            CHECK(error->message->find("start with '/'") != std::string::npos);
        }
    }

    SUBCASE("Component Validation") {
        // Invalid components
        {
            auto error = SP::Path<std::string>("//").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Empty path component") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path//other").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Empty path component") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/.").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Relative paths not allowed") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/..").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Relative paths not allowed") != std::string::npos);
        }
    }

    SUBCASE("Glob Pattern Validation") {
        // Valid patterns
        CHECK_FALSE(SP::Path<std::string>("/path/*").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/*/path").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/?/other").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[abc]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[a-z]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[!a-z]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[0-9]/*").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/**").validate().has_value());

        // Invalid patterns
        {
            auto error = SP::Path<std::string>("/path/[").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Unclosed bracket") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/]").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Unmatched closing bracket") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/[a-]").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/[-a]").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
        {
            auto error = SP::Path<std::string>("/path/[z-a]").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Invalid character range") != std::string::npos);
        }
    }

    SUBCASE("Escape Sequence Validation") {
        // Valid escapes
        CHECK_FALSE(SP::Path<std::string>("/path/\\*").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/\\?").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/\\[").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/\\]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/\\\\").validate().has_value());
    }

    SUBCASE("Complex Pattern Combinations") {
        // Multiple patterns
        CHECK_FALSE(SP::Path<std::string>("/path/[a-z]/[0-9]/*").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/*/[a-z]/?/[0-9]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/**/[a-z]/*/[0-9]").validate().has_value());

        // Escaped patterns in brackets
        CHECK(SP::Path<std::string>("/path/[\\[-\\]]").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[\\*\\?]").validate().has_value());

        // Complex combinations
        CHECK_FALSE(SP::Path<std::string>("/[a-z]*/[0-9]?/*").validate().has_value());
        CHECK_FALSE(SP::Path<std::string>("/path/[!a-z][0-9]/*").validate().has_value());

        // Invalid combinations
        {
            auto error = SP::Path<std::string>("/path/[[a-z]]").validate();
            REQUIRE(error.has_value());
            CHECK(error->message->find("Nested brackets") != std::string::npos);
        }
    }

    SUBCASE("Edge Cases") {
        // Maximum nesting
        {
            std::string deep_path;
            for (int i = 0; i < 100; ++i) {
                deep_path += "/valid";
            }
            CHECK_FALSE(SP::Path<std::string>(deep_path).validate().has_value());
        }

        // Long component names
        {
            std::string long_name = "/path/";
            long_name.append(1000, 'a');
            CHECK_FALSE(SP::Path<std::string>(long_name).validate().has_value());
        }

        // Complex pattern combinations
        CHECK_FALSE(SP::Path<std::string>("/[!a-z][0-9]\\*/?/[a-zA-Z0-9]/\\[escaped\\]").validate().has_value());
    }

    /*SUBCASE("Compile Time Validation") {
        static constexpr auto valid1 = "/valid/path";
        static constexpr auto valid2 = "/valid/[a-z]/*";
        static constexpr auto valid3 = "/valid/\\[escaped\\]";

        static_assert(!SP::Path<std::string>(valid1).validate().has_value());
        static_assert(!SP::Path<std::string>(valid2).validate().has_value());
        static_assert(!SP::Path<std::string>(valid3).validate().has_value());*/

    // These would cause compile errors if uncommented
    /*
    static_assert(!SP::Path<std::string>("invalid").validate().has_value());
    static_assert(!SP::Path<std::string>("/invalid/[/").validate().has_value());
    static_assert(!SP::Path<std::string>("/invalid/path/").validate().has_value());
    */
    //}
}

TEST_CASE("PathSpace Integration") {
    SP::PathSpace pspace;

    SUBCASE("Insert Validation") {
        // Valid inserts
        CHECK(pspace.insert("/valid/path", 42).errors.empty());
        CHECK(pspace.insert("/test/[a-z]/*", 42).errors.empty());
        CHECK(pspace.insert("/test/**", 42).errors.empty());

        // Invalid inserts
        {
            auto ret = pspace.insert("invalid", 42);
            CHECK_FALSE(ret.errors.empty());
            CHECK(ret.errors[0].code == SP::Error::Code::InvalidPath);
        }
    }

    SUBCASE("Read Validation") {
        pspace.insert("/test", 42);

        // Valid paths
        CHECK(pspace.read<int>("/test").has_value());

        // Invalid paths
        auto bad_read = pspace.read<int>("invalid");
        REQUIRE_FALSE(bad_read.has_value());
        CHECK(bad_read.error().code == SP::Error::Code::InvalidPath);
    }

    SUBCASE("Extract Validation") {
        pspace.insert("/test", 42);

        // Valid paths
        CHECK(pspace.extract<int>("/test").has_value());

        // Invalid paths
        auto bad_extract = pspace.extract<int>("invalid");
        REQUIRE_FALSE(bad_extract.has_value());
        CHECK(bad_extract.error().code == SP::Error::Code::InvalidPath);
    }
}