#include "PathSpace.hpp"
#include "third_party/doctest.h"

#include <typeinfo>

using namespace SP;

namespace {
class PathSpaceProbe : public PathSpace {
public:
    using PathSpace::PathSpace;
    using PathSpace::notifyAll;
    using PathSpace::peekFuture;
    using PathSpace::shutdownPublic;
    using PathSpace::typedPeekFuture;
};
} // namespace

TEST_SUITE("pathspace.header") {
TEST_CASE("PathSpace inline helpers are reachable") {
    PathSpaceProbe space;

    // notifyAll/shutdownPublic are thin wrappers; just ensure they can be called.
    space.notifyAll();
    space.shutdownPublic();

    CHECK_FALSE(space.peekFuture("/nothing").has_value());
    CHECK_FALSE(space.typedPeekFuture("/nothing").has_value());
}

TEST_CASE("peekFuture surfaces execution futures and ignores non-exec or glob paths") {
    PathSpaceProbe space;

    auto insertResult = space.insert("/jobs/task", [] { return 5; }, In{.executionCategory = ExecutionCategory::Lazy});
    REQUIRE(insertResult.errors.empty());
    CHECK(insertResult.nbrTasksInserted == 1);

    auto future = space.peekFuture("/jobs/task");
    REQUIRE(future.has_value());
    CHECK(future->valid());

    auto anyFuture = space.typedPeekFuture("/jobs/task");
    REQUIRE(anyFuture.has_value());
    CHECK(anyFuture->valid());
    CHECK(anyFuture->type() == typeid(int));

    REQUIRE(space.insert("/jobs/value", 3).errors.empty());
    CHECK_FALSE(space.peekFuture("/jobs/value").has_value());
    CHECK_FALSE(space.typedPeekFuture("/jobs/value").has_value());

    CHECK_FALSE(space.peekFuture("/jobs/*").has_value());
    CHECK_FALSE(space.typedPeekFuture("/jobs/*").has_value());
}
}
