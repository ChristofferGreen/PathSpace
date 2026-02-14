#define private public
#include "core/PathSpaceContext.hpp"
#undef private
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
TEST_CASE("guards reentrant sink notifications and exposes executor accessors") {
    struct DummyExecutor : Executor {
        auto submit(std::weak_ptr<Task>&&) -> std::optional<Error> override { return std::nullopt; }
        void shutdown() override {}
        auto size() const -> size_t override { return 1; }
    };

    PathSpaceContext ctx;
    DummyExecutor exec;
    ctx.setExecutor(&exec);
    CHECK(ctx.executor() == &exec);

    struct ReentrantSink : NotificationSink {
        explicit ReentrantSink(PathSpaceContext& c) : ctx(c) {}
        void notify(std::string const& notificationPath) override {
            calls.push_back(notificationPath);
            ctx.notify(notificationPath + "/again"); // should be ignored due to notifyingSink guard
        }
        PathSpaceContext&       ctx;
        std::vector<std::string> calls;
    };

    auto sink = std::make_shared<ReentrantSink>(ctx);
    ctx.setSink(sink);

    ctx.notify("/root");
    REQUIRE(sink->calls.size() == 1);
    CHECK(sink->calls.front() == "/root");
}

TEST_CASE("hasWaiters lazily initializes wait registry") {
    PathSpaceContext ctx;

    CHECK_FALSE(ctx.hasWaiters());
    // Second call should reuse the already-initialized registry without crashing.
    CHECK_FALSE(ctx.hasWaiters());
}

TEST_CASE("ensureWait rebuilds registry when cleared") {
    PathSpaceContext ctx;
    ctx.waitRegistry.reset(); // simulate late initialization path

    auto guard  = ctx.wait("/reinit");
    auto status = guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(2));
    CHECK(status == std::cv_status::timeout);
    CHECK(ctx.hasWaiters());
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

TEST_CASE("getSink returns empty when no sink is set") {
    PathSpaceContext ctx;
    CHECK(ctx.getSink().expired());
    // notify should be safe even with no sink installed.
    ctx.notify("/noop");
    CHECK(ctx.getSink().expired());
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

TEST_CASE("clearWaits drops registered waiters and allows reuse") {
    PathSpaceContext ctx;

    {
        auto guard = ctx.wait("/clear");
        auto status = guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(2));
        CHECK(status == std::cv_status::timeout);
    }

    CHECK(ctx.hasWaiters());
    ctx.clearWaits();
    CHECK_FALSE(ctx.hasWaiters());

    // Ensure waits still function after clearing.
    auto guardAfter = ctx.wait("/clear");
    auto statusAfter = guardAfter.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(2));
    CHECK(statusAfter == std::cv_status::timeout);
}

TEST_CASE("notifyAll wakes context waiters") {
    PathSpaceContext ctx;
    std::atomic<bool> waiting{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&] {
        auto guard = ctx.wait("/notify/all");
        waiting.store(true, std::memory_order_release);
        auto status = guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(250));
        woke.store(status == std::cv_status::no_timeout, std::memory_order_release);
    });

    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ctx.notifyAll();

    waiter.join();
    CHECK(woke.load(std::memory_order_acquire));
}
}
