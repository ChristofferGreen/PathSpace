#include "PathSpace.hpp"
#include "PathSpaceTestHelper.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"
#include "task/IFutureAny.hpp"
#include "third_party/doctest.h"

#include <chrono>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("core.nodedata.execution") {

TEST_CASE("the typed future at a path follows the front execution after a take") {
    PathSpace space;
    REQUIRE(space.insert("/work", [] { return 1; }).errors.empty());
    REQUIRE(space.insert("/work", [] { return 2; }).errors.empty());

    auto first = space.take<int>("/work", Block{2s});
    REQUIRE(first.has_value());
    CHECK(*first == 1);

    // The node now starts with the second execution; its future must be the one handed out.
    auto future = space.read("/work");
    REQUIRE(future.has_value());
    int out = 0;
    REQUIRE(future->wait_for(2s));
    REQUIRE(future->copy_to(&out));
    CHECK(out == 2);

    auto second = space.take<int>("/work", Block{2s});
    REQUIRE(second.has_value());
    CHECK(*second == 2);
}

TEST_CASE("a node stops reporting an execution payload once its executions were taken") {
    PathSpace space;
    REQUIRE(space.insert("/mixed", [] { return 1; }).errors.empty());
    REQUIRE(space.insert("/mixed", 5).errors.empty());

    auto result = space.take<int>("/mixed", Block{2s});
    REQUIRE(result.has_value());
    CHECK(*result == 1);

    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root);
    auto* node = root->getChild("mixed");
    REQUIRE(node);
    {
        std::lock_guard<std::mutex> guard(node->payloadMutex);
        REQUIRE(node->data);
        // Only the plain value is left. A stale future handle would keep the node flagged as
        // holding an execution, which history capture and diagnostics act on.
        CHECK_FALSE(node->data->hasExecutionPayload());
        CHECK_FALSE(node->data->peekAnyFuture().has_value());
    }
    CHECK_FALSE(space.read("/mixed").has_value());

    auto value = space.take<int>("/mixed");
    REQUIRE(value.has_value());
    CHECK(*value == 5);
}

}
