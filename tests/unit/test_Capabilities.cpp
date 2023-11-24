#include <catch2/catch_test_macros.hpp>
#include <pathspace/core/Capabilities.hpp>
#include <pathspace/core/SpacePath.hpp>

using namespace SP;

TEST_CASE("Capabilities", "[Capabilities]") {
    Capabilities caps;

    SECTION("Adding and checking a specific capability") {
        caps.addCapability("/path/to/resource", Capabilities::Type::READ);
        REQUIRE(caps.hasCapability("/path/to/resource", Capabilities::Type::READ));
    }

    SECTION("Checking a capability that does not exist returns false") {
        REQUIRE_FALSE(caps.hasCapability("/path/to/resource", Capabilities::Type::WRITE));
    }

    SECTION("Wildcard capability matches any path") {
        caps.addCapability("/*/to/resource", Capabilities::Type::EXECUTE);
        REQUIRE(caps.hasCapability("/any_path/to/resource", Capabilities::Type::EXECUTE));
        REQUIRE_FALSE(caps.hasCapability("/path/not/matching/resource", Capabilities::Type::EXECUTE));
    }

    SECTION("Adding capability with wildcard for any type and checking") {
        caps.addCapability("*", "/path/to/anywhere");
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::READ));
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::WRITE));
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::EXECUTE));
    }

    SECTION("Checking a capability with both action and path as wildcards") {
        caps.addCapability("*", SpacePath("*"));
        REQUIRE(caps.hasCapability("/any/path", Capabilities::Type::WRITE));
        REQUIRE(caps.hasCapability("write", SpacePath("/different/path")));
    }
}

// ... Additional test cases to cover more functionality and edge cases
