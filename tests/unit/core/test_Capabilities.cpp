#include "ext/doctest.h"
#include <pathspace/core/Capabilities.hpp>

using namespace SP;

TEST_CASE("Capabilities") {
    Capabilities caps;

    SUBCASE("Adding and checking a specific capability") {
        caps.addCapability("/path/to/resource", Capabilities::Type::READ);
        REQUIRE(caps.hasCapability("/path/to/resource", Capabilities::Type::READ));
    }

    SUBCASE("Checking a capability that does not exist returns false") {
        REQUIRE_FALSE(caps.hasCapability("/path/to/resource", Capabilities::Type::WRITE));
    }

    SUBCASE("Wildcard capability matches any path") {
        caps.addCapability("/*/to/resource", Capabilities::Type::EXECUTE);
        REQUIRE(caps.hasCapability("/any_path/to/resource", Capabilities::Type::EXECUTE));
        REQUIRE_FALSE(caps.hasCapability("/path/not/matching/resource", Capabilities::Type::EXECUTE));
    }

    SUBCASE("Adding capability with wildcard for any type and checking") {
        /*caps.addCapability("*", "/path/to/anywhere");
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::READ));
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::WRITE));
        REQUIRE(caps.hasCapability("/path/to/anywhere", Capabilities::Type::EXECUTE));*/
    }

    SUBCASE("Checking a capability with both action and path as wildcards") {
        /*caps.addCapability("*", Path("*"));
        REQUIRE(caps.hasCapability("/any/path", Capabilities::Type::WRITE));
        REQUIRE(caps.hasCapability("write", Path("/different/path")));*/
    }
}

// ... Additional test cases to cover more functionality and edge cases
