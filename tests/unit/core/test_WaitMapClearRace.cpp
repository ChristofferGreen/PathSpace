#include "core/WaitMap.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

namespace {

// State shared with the hook, which is a plain function pointer.
struct LateWaiter {
    WaitMap*                 map = nullptr;
    std::atomic<bool>        used{false};
    std::thread              thread;
    std::promise<void>       registered;
    std::promise<std::cv_status> finished;
};

std::atomic<LateWaiter*> lateWaiter{nullptr};

// Runs on the thread inside WaitMap::clear(), after the waiters drained: start a waiter now.
void registerLateWaiter() {
    auto* late = lateWaiter.load(std::memory_order_acquire);
    if (!late || late->used.exchange(true)) {
        return;
    }
    late->thread = std::thread([late] {
        auto guard = late->map->wait("/late");
        late->registered.set_value();
        auto status = guard.wait_until(std::chrono::system_clock::now() + 2s);
        late->finished.set_value(status);
    });
    late->registered.get_future().wait();
}

} // namespace

TEST_SUITE("core.waitmap.clear") {

TEST_CASE("clear does not destroy the registry under a waiter that registered while it drained") {
    WaitMap    map;
    LateWaiter late;
    late.map = &map;
    lateWaiter.store(&late, std::memory_order_release);
    testing::SetWaitMapClearDrainedHook(&registerLateWaiter);

    auto finished = late.finished.get_future();
    map.clear();

    testing::SetWaitMapClearDrainedHook(nullptr);
    lateWaiter.store(nullptr, std::memory_order_release);
    REQUIRE(late.used.load());

    // The late waiter registered after clear() had drained the old ones. Its entry has to survive
    // the clear: a notification must still reach it. Destroying the entry under it orphans the
    // waiter (nothing can wake it any more) and leaves it waiting on freed memory.
    map.notify("/late");
    REQUIRE(finished.wait_for(10s) == std::future_status::ready);
    auto const status = finished.get();
    late.thread.join();
    CHECK(status == std::cv_status::no_timeout);

    // With no waiter left, clear() empties the registry as before.
    map.clear();
    CHECK_FALSE(map.hasWaiters());
}

}
