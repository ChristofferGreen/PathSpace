#include "core/PathSpaceContext.hpp"

#include "CountingMemoryResource.hpp"
#include "third_party/doctest.h"

#include <memory_resource>

using namespace SP;

TEST_SUITE("core.pathspace_context.memory_resource") {
TEST_CASE("defaults to the process default resource") {
    PathSpaceContext ctx;
    CHECK(ctx.getMemoryResource() == std::pmr::get_default_resource());
}

TEST_CASE("setMemoryResource installs the resource and getMemoryResource returns it") {
    PathSpaceContext                ctx;
    testing::CountingMemoryResource counting;
    auto const                      result = ctx.setMemoryResource(&counting);
    CHECK(result.has_value());
    CHECK(ctx.getMemoryResource() == &counting);
}

TEST_CASE("setMemoryResource rejects null and keeps the current resource") {
    PathSpaceContext                ctx;
    testing::CountingMemoryResource counting;
    REQUIRE(ctx.setMemoryResource(&counting).has_value());

    auto const result = ctx.setMemoryResource(nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidError);
    CHECK(ctx.getMemoryResource() == &counting);
}

TEST_CASE("the context never allocates from or owns the resource") {
    testing::CountingMemoryResource counting;
    {
        PathSpaceContext ctx;
        REQUIRE(ctx.setMemoryResource(&counting).has_value());
    }
    CHECK(counting.allocations() == 0);
}
}
