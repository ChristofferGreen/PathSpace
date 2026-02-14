#include "third_party/doctest.h"

#include <pathspace/core/WatchRegistry.hpp>

#include <chrono>
#include <thread>
#include <atomic>

using namespace std::chrono_literals;

TEST_SUITE_BEGIN("core.watchregistry");

TEST_CASE("waiters wake and counts drop after notify") {
    SP::WatchRegistry registry;

    auto guard = registry.wait("/one");
    CHECK(registry.hasWaiters());

    std::thread notifier([&] {
        std::this_thread::sleep_for(5ms);
        registry.notify("/one");
    });

    auto status = guard.wait_until(std::chrono::system_clock::now() + 50ms);
    notifier.join();

    CHECK(status == std::cv_status::no_timeout);
    // guard destruction should drop the count
    guard = SP::WatchRegistry::Guard{};
    CHECK_FALSE(registry.hasWaiters());
}

TEST_CASE("clear resets registered waiters") {
    SP::WatchRegistry registry;
    {
        auto g1 = registry.wait("/a");
        auto g2 = registry.wait("/b");
        CHECK(registry.hasWaiters());
    } // guards out of scope, counts should drop but paths remain allocated

    registry.clear();
    CHECK_FALSE(registry.hasWaiters());
}

TEST_CASE("notifyAll wakes root and nested waiters") {
    SP::WatchRegistry registry;
    std::atomic<bool> go{false};
    std::atomic<bool> rootReady{false};
    std::atomic<bool> nestedReady{false};
    std::atomic<bool> rootWoke{false};
    std::atomic<bool> nestedWoke{false};

    auto waiter = [&](std::string path, std::atomic<bool>& ready, std::atomic<bool>& woke) {
        auto guard = registry.wait(path);
        ready.store(true, std::memory_order_release);
        auto deadline = std::chrono::system_clock::now() + 200ms;
        auto signaled = guard.wait_until(deadline, [&] { return go.load(std::memory_order_acquire); });
        woke.store(signaled, std::memory_order_release);
    };

    std::thread rootThread(waiter, "/", std::ref(rootReady), std::ref(rootWoke));
    std::thread nestedThread(waiter, "/a/b", std::ref(nestedReady), std::ref(nestedWoke));

    while (!rootReady.load(std::memory_order_acquire)
           || !nestedReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    go.store(true, std::memory_order_release);
    registry.notifyAll();

    rootThread.join();
    nestedThread.join();

    CHECK(rootWoke.load(std::memory_order_acquire));
    CHECK(nestedWoke.load(std::memory_order_acquire));
    CHECK_FALSE(registry.hasWaiters());
}

TEST_CASE("predicate wait_until returns true when predicate already satisfied") {
    SP::WatchRegistry registry;
    auto guard = registry.wait("/predicate");
    bool ready = true;

    auto result = guard.wait_until(std::chrono::system_clock::now() + 1ms,
                                   [&] { return ready; });

    CHECK(result);
}

TEST_CASE("notify treats trailing slashes as the same path") {
    SP::WatchRegistry registry;
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&] {
        auto guard = registry.wait("/a/b");
        ready.store(true, std::memory_order_release);
        auto deadline = std::chrono::system_clock::now() + 200ms;
        auto signaled = guard.wait_until(deadline, [&] { return go.load(std::memory_order_acquire); });
        woke.store(signaled, std::memory_order_release);
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    go.store(true, std::memory_order_release);
    registry.notify("/a/b/");

    waiter.join();
    CHECK(woke.load(std::memory_order_acquire));
    CHECK_FALSE(registry.hasWaiters());
}

TEST_SUITE_END();
