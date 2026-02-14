#define private public
#include "core/WaitMap.hpp"
#undef private

#include "third_party/doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("core.waitmap") {
TEST_CASE("Guard initializes version and counts lazily") {
    WaitMap waitMap;
    WaitMap::Guard guard(waitMap, "/lazy", 0, false);

    CHECK_FALSE(guard.counted);
    CHECK(guard.versionInitialized);

    guard.versionInitialized = false;
    auto status = guard.wait_until(std::chrono::system_clock::now() + 5ms);
    CHECK(status == std::cv_status::timeout);
    CHECK(guard.versionInitialized);
    CHECK(guard.counted);
}

TEST_CASE("notify waits for registry lock when busy") {
    WaitMap waitMap;
    auto    guard = waitMap.wait("/busy");

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::unique_lock<std::timed_mutex> hold(waitMap.registryMutex);

    std::thread notifier([&] {
        started.store(true, std::memory_order_release);
        waitMap.notify("/busy");
        finished.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(150ms);
    hold.unlock();

    notifier.join();
    CHECK(finished.load(std::memory_order_acquire));
}

TEST_CASE("debug_log no-ops when disabled") {
    testing::waitMapDebugOverride().store(false, std::memory_order_relaxed);
    WaitMap::debug_log("noop", "/debug/noop", std::chrono::milliseconds{0}, std::chrono::milliseconds{0}, 0);
}

TEST_CASE("guard tracks active waiters and clear waits for drain") {
    testing::waitMapDebugOverride().store(true, std::memory_order_relaxed);

    WaitMap waitMap;
    std::atomic<bool> predicateReady{false};
    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> enteredWait{false};

    std::thread waiter([&] {
        auto guard = waitMap.wait("/paths/foo");
        waiterStarted.store(true, std::memory_order_release);
        auto const deadline = std::chrono::system_clock::now() + 250ms;
        auto ok = guard.wait_until(deadline, [&] {
            enteredWait.store(true, std::memory_order_release);
            return predicateReady.load(std::memory_order_acquire);
        });
        CHECK_MESSAGE(ok, "waiter should exit once predicate is ready");
    });

    // Ensure the waiter is blocked inside wait_until before proceeding.
    while (!waiterStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // hasWaiters becomes true once an entry exists in the trie.
    while (!enteredWait.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool observedWaiters = false;
    for (int i = 0; i < 50 && !observedWaiters; ++i) {
        observedWaiters = waitMap.hasWaiters();
        if (!observedWaiters) {
            std::this_thread::sleep_for(2ms);
        }
    }
    CHECK(observedWaiters);

    // Enable predicate and wake the waiter via clear(). clear() should block until the waiter drains.
    predicateReady.store(true, std::memory_order_release);
    waitMap.clear();

    waiter.join();

    // After clear, all waiter structures should be removed.
    CHECK_FALSE(waitMap.hasWaiters());

    // Reset override for other tests.
    testing::waitMapDebugOverride().store(false, std::memory_order_relaxed);
}

TEST_CASE("clear waits for scoped guard destruction") {
    WaitMap waitMap;
    std::atomic<bool> waiterReady{false};
    std::atomic<bool> cleared{false};

    std::thread waiter([&] {
        auto guard = waitMap.wait("/scoped/clear");
        waiterReady.store(true, std::memory_order_release);
        // Wait without predicate so the guard increments activeWaiterCount.
        guard.wait_until(std::chrono::system_clock::now() + 75ms);
    });

    // Ensure the waiter is registered before clearing.
    while (!waiterReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread clearer([&] {
        waitMap.clear();
        cleared = true;
    });

    waiter.join();
    clearer.join();

    CHECK(cleared.load());
    CHECK_FALSE(waitMap.hasWaiters());
}

TEST_CASE("predicate wait increments active waiter count on first wait") {
    WaitMap waitMap;

    WaitMap::Guard guard(waitMap, "/predicate", 0, false);
    bool predicateCalled = false;
    auto deadline = std::chrono::system_clock::now() + 50ms;
    auto ok = guard.wait_until(deadline, [&] {
        predicateCalled = true;
        return true;
    });

    CHECK(predicateCalled);
    CHECK(ok);
}

TEST_CASE("Guard move constructor preserves wait entry and counts") {
    WaitMap waitMap;
    auto original = waitMap.wait("/move");
    WaitMap::Guard moved(std::move(original));

    auto status = moved.wait_until(std::chrono::system_clock::now() + 10ms);
    CHECK(status == std::cv_status::timeout);
}

TEST_CASE("notify wakes both concrete and glob waiters") {
    WaitMap waitMap;

    std::atomic<int> started{0};
    std::atomic<bool> concreteWoke{false};
    std::atomic<bool> globWoke{false};

    auto concrete = std::thread([&] {
        auto guard = waitMap.wait("/foo/bar");
        started.fetch_add(1, std::memory_order_acq_rel);
        auto status = guard.wait_until(std::chrono::system_clock::now() + 250ms);
        concreteWoke.store(status == std::cv_status::no_timeout, std::memory_order_release);
    });

    auto glob = std::thread([&] {
        auto guard = waitMap.wait("/foo/*");
        started.fetch_add(1, std::memory_order_acq_rel);
        auto status = guard.wait_until(std::chrono::system_clock::now() + 250ms);
        globWoke.store(status == std::cv_status::no_timeout, std::memory_order_release);
    });

    // Wait until both waiters are blocked inside wait_until.
    while (started.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    waitMap.notify("/foo/bar");

    concrete.join();
    glob.join();

    CHECK(concreteWoke.load(std::memory_order_acquire));
    CHECK(globWoke.load(std::memory_order_acquire));
}

TEST_CASE("glob notify wakes matching concrete waiters and logs when enabled") {
    testing::waitMapDebugOverride().store(true, std::memory_order_relaxed);

    WaitMap waitMap;
    std::atomic<int> started{0};
    std::atomic<int> waiting{0};
    std::atomic<bool> aWoke{false};
    std::atomic<bool> bWoke{false};

    std::thread a([&] {
        auto guard = waitMap.wait("/root/a");
        started.fetch_add(1, std::memory_order_acq_rel);
        waiting.fetch_add(1, std::memory_order_acq_rel);
        auto status = guard.wait_until(std::chrono::system_clock::now() + 1000ms);
        aWoke.store(status == std::cv_status::no_timeout, std::memory_order_release);
    });

    std::thread b([&] {
        auto guard = waitMap.wait("/root/b");
        started.fetch_add(1, std::memory_order_acq_rel);
        waiting.fetch_add(1, std::memory_order_acq_rel);
        auto status = guard.wait_until(std::chrono::system_clock::now() + 1000ms);
        bWoke.store(status == std::cv_status::no_timeout, std::memory_order_release);
    });

    while (started.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    while (waiting.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(10ms);
    waitMap.notify("/root/*");

    a.join();
    b.join();

    CHECK(aWoke.load(std::memory_order_acquire));
    CHECK(bWoke.load(std::memory_order_acquire));

    testing::waitMapDebugOverride().store(false, std::memory_order_relaxed);
}

TEST_CASE("notifyAll wakes all registered waiters") {
    WaitMap waitMap;

    struct WaiterState {
        std::string path;
        std::atomic<bool> waiting{false};
        std::atomic<bool> woke{false};
    };

    std::vector<std::unique_ptr<WaiterState>> states;
    states.emplace_back(std::make_unique<WaiterState>());
    states.back()->path = "/alpha";
    states.emplace_back(std::make_unique<WaiterState>());
    states.back()->path = "/alpha/beta";
    states.emplace_back(std::make_unique<WaiterState>());
    states.back()->path = "/*/beta";

    std::atomic<int> started{0};
    std::vector<std::thread> threads;
    threads.reserve(states.size());

    for (auto& state : states) {
        threads.emplace_back([&, statePtr = state.get()] {
            auto guard = waitMap.wait(statePtr->path);
            started.fetch_add(1, std::memory_order_acq_rel);
            statePtr->waiting.store(true, std::memory_order_release);
            auto status = guard.wait_until(std::chrono::system_clock::now() + 250ms);
            statePtr->woke.store(status == std::cv_status::no_timeout, std::memory_order_release);
        });
    }

    while (started.load(std::memory_order_acquire) < static_cast<int>(states.size())) {
        std::this_thread::yield();
    }
    // Ensure all waiters have called wait_until before notifying.
    bool allWaiting = false;
    for (int i = 0; i < 50 && !allWaiting; ++i) {
        allWaiting = std::all_of(states.begin(), states.end(), [](auto const& s) {
            return s->waiting.load(std::memory_order_acquire);
        });
        if (!allWaiting) {
            std::this_thread::sleep_for(2ms);
        }
    }

    waitMap.notifyAll();

    for (auto& t : threads) {
        t.join();
    }

    for (auto& state : states) {
        CHECK(state->woke.load(std::memory_order_acquire));
    }
}

TEST_CASE("wait_until without predicate exercises debug logging path") {
    testing::waitMapDebugOverride().store(true, std::memory_order_relaxed);

    WaitMap waitMap;
    auto guard  = waitMap.wait("/debug/log");
    auto status = guard.wait_until(std::chrono::system_clock::now() + 5ms);

    CHECK(status == std::cv_status::timeout);

    testing::waitMapDebugOverride().store(false, std::memory_order_relaxed);
}
}
