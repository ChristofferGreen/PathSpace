#include "core/PathSpaceContext.hpp"

#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("core.pathspacecontext") {
TEST_CASE("hasWaiters lazily initializes wait registry") {
    PathSpaceContext ctx;

    CHECK_FALSE(ctx.hasWaiters());
    // Second call should reuse the already-initialized registry without crashing.
    CHECK_FALSE(ctx.hasWaiters());
}
}
