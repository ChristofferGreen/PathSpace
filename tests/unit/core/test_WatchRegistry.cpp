#include "third_party/doctest.h"

#include <pathspace/core/WatchRegistry.hpp>

#include <chrono>
#include <thread>

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

TEST_SUITE_END();
