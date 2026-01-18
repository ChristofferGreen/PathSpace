#include "PathSpace.hpp"
#include "third_party/doctest.h"

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
}
