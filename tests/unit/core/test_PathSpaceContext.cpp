#include "core/PathSpaceContext.hpp"
#include "task/TaskPool.hpp"

#include "third_party/doctest.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace SP;

namespace {

struct RecordingSink : NotificationSink {
    void notify(const std::string& notificationPath) override {
        notifications.push_back(notificationPath);
    }

    std::vector<std::string> notifications;
};

} // namespace

TEST_SUITE("core.pathspacecontext") {
TEST_CASE("hasWaiters lazily initializes wait registry") {
    PathSpaceContext ctx;

    CHECK_FALSE(ctx.hasWaiters());
    // Second call should reuse the already-initialized registry without crashing.
    CHECK_FALSE(ctx.hasWaiters());
}

TEST_CASE("sink lifecycle forwards notifications and can be invalidated") {
    PathSpaceContext ctx;
    auto sink = std::make_shared<RecordingSink>();
    ctx.setSink(sink);

    auto weak = ctx.getSink();
    CHECK_FALSE(weak.expired());

    ctx.notify("/foo");
    REQUIRE(sink->notifications.size() == 1);
    CHECK(sink->notifications.front() == "/foo");

    ctx.invalidateSink();
    CHECK(ctx.getSink().expired());

    // After invalidation, notifications should be dropped.
    ctx.notify("/bar");
    CHECK(sink->notifications.size() == 1);
}

TEST_CASE("shutdown sets flag and wakes waiters") {
    PathSpaceContext ctx(&TaskPool::Instance());

    {
        auto guard = ctx.wait("/wake");
        // Short timeout to exercise wait path without blocking the suite.
        auto status = guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(5));
        CHECK(status == std::cv_status::timeout);
    }

    ctx.shutdown();
    CHECK(ctx.isShuttingDown());

    // New waits should still be serviceable even after shutdown.
    {
        auto guardAfter = ctx.wait("/wake");
        auto status = guardAfter.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(5));
        CHECK(status == std::cv_status::timeout);
    }
}
}
