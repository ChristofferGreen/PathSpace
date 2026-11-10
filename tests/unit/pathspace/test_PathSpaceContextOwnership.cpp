#include "PathSpaceNestingTestSupport.hpp"

namespace {

// Nested space that reports its destruction.
struct TrackedSpace final : PathSpace {
    explicit TrackedSpace(std::atomic<int>& counter) : destroyed(counter) {}
    ~TrackedSpace() override { destroyed.fetch_add(1); }
    std::atomic<int>& destroyed;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.context.ownership");

TEST_CASE("a mounted nested space is destroyed when its parent is cleared") {
    std::atomic<int> destroyed{0};
    PathSpace        root;
    REQUIRE(root.insert("/mount", std::make_unique<TrackedSpace>(destroyed)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/inner", 1).errors.empty());

    root.clear();
    // The nested space is owned by the tree; wiping the tree has to run its destructor. A space
    // that is never destroyed keeps its values, tasks and (if it owns one) its thread pool.
    CHECK(destroyed.load() == 1);
}

TEST_CASE("a mounted nested space is destroyed with its parent") {
    std::atomic<int> destroyed{0};
    {
        PathSpace root;
        REQUIRE(root.insert("/mount", std::make_unique<TrackedSpace>(destroyed)).nbrSpacesInserted == 1);
        REQUIRE(root.insert("/mount", std::make_unique<TrackedSpace>(destroyed)).nbrSpacesInserted == 1);
    }
    CHECK(destroyed.load() == 2);
}

TEST_CASE("a nested space replaced at its node is destroyed") {
    std::atomic<int> destroyed{0};
    PathSpace        root;
    REQUIRE(root.insert("/mount", std::make_unique<TrackedSpace>(destroyed)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount", 5, In{.replaceExistingPayload = true}).errors.empty());
    CHECK(destroyed.load() == 1);
}

TEST_CASE("destroying a nested space that was taken out leaves the parent's context running") {
    IntrospectablePathSpace root;
    REQUIRE(root.insert("/mount", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    {
        auto taken = root.take<std::unique_ptr<PathSpace>>("/mount");
        REQUIRE(taken.has_value());
        // The nested space still shares the root's context when it is destroyed here.
    }

    auto ctx = root.contextPtr();
    REQUIRE(ctx);
    CHECK_FALSE(ctx->isShuttingDown());

    // A blocking read has to wait for its timeout, not bail out with "shutting down".
    auto missing = root.read<int>("/missing", Block{20ms});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.value_or("").find("Shutting down") == std::string::npos);
}

TEST_CASE("clearing a space that contains a nested space leaves its own context running") {
    IntrospectablePathSpace root;
    REQUIRE(root.insert("/mount", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/inner", 3).errors.empty());

    root.clear();

    auto ctx = root.contextPtr();
    REQUIRE(ctx);
    CHECK_FALSE(ctx->isShuttingDown());

    // The space is empty but alive: a blocking read is satisfied by a later insert.
    std::promise<void> registered;
    std::thread        writer([&] {
        registered.get_future().wait();
        auto const waiterDeadline = std::chrono::steady_clock::now() + 5s;
        while (!ctx->hasWaiters() && std::chrono::steady_clock::now() < waiterDeadline) {
            std::this_thread::yield();
        }
        root.insert("/value", 9);
    });
    registered.set_value();
    auto value = root.read<int>("/value", Block{5s});
    writer.join();
    REQUIRE(value.has_value());
    CHECK(*value == 9);
}

TEST_CASE("replacing a mounted nested space leaves the parent's context running") {
    IntrospectablePathSpace root;
    REQUIRE(root.insert("/mount", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    // Replace the node's payload: the old nested space is destroyed while mounted.
    REQUIRE(root.insert("/mount", 5, In{.replaceExistingPayload = true}).errors.empty());

    auto ctx = root.contextPtr();
    REQUIRE(ctx);
    CHECK_FALSE(ctx->isShuttingDown());
}

TEST_SUITE_END();
