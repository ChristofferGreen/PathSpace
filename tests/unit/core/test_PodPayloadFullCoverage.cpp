#include "core/PodPayload.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace SP;

TEST_SUITE("core.podpayload.full") {
TEST_CASE("PodPayload pinned spans and raw-from helpers") {
    PodPayload<int> payload;

    // Grow beyond initial capacity to exercise ensureCapacity.
    for (int i = 0; i < 1100; ++i) {
        REQUIRE(payload.push(i));
    }
    CHECK(payload.size() == 1100);

    std::shared_ptr<void> rawToken;
    std::size_t rawCount = 0;
    auto rawErr = payload.withSpanRawPinned(
        [&](void const* data, std::size_t count, std::shared_ptr<void> const& token) {
            rawCount = count;
            rawToken = token;
            CHECK(token != nullptr);
            CHECK(count >= 1);
            CHECK(static_cast<int const*>(data)[0] == 0);
        });
    CHECK_FALSE(rawErr.has_value());
    CHECK(rawCount == payload.size());

    std::shared_ptr<void> mutToken;
    auto mutErr = payload.withSpanMutableRawPinned(
        [&](void* data, std::size_t count, std::shared_ptr<void> const& token) {
            mutToken = token;
            if (count > 0) {
                static_cast<int*>(data)[0] = 99;
            }
        });
    CHECK_FALSE(mutErr.has_value());
    CHECK(mutToken != nullptr);

    // Raw-from variants (const and mutable) with a non-zero start offset.
    std::size_t fromCount = 0;
    auto fromErr = payload.withSpanRawFrom(1000, [&](void const* data, std::size_t count) {
        fromCount = count;
        CHECK(count == 100);
        CHECK(static_cast<int const*>(data)[0] == 1000);
    });
    CHECK_FALSE(fromErr.has_value());
    CHECK(fromCount == 100);

    std::size_t mutFromCount = 0;
    auto mutFromErr = payload.withSpanMutableRawFrom(1000, [&](void* data, std::size_t count) {
        mutFromCount = count;
        if (count > 0) {
            static_cast<int*>(data)[count - 1] = -1;
        }
    });
    CHECK_FALSE(mutFromErr.has_value());
    CHECK(mutFromCount == 100);

    // Pinned from variants.
    std::shared_ptr<void> pinnedFromToken;
    auto pinnedFromErr = payload.withSpanRawFromPinned(
        1099, [&](void const* data, std::size_t count, std::shared_ptr<void> const& token) {
            pinnedFromToken = token;
            CHECK(count == 1);
            CHECK(static_cast<int const*>(data)[0] == -1);
        });
    CHECK_FALSE(pinnedFromErr.has_value());
    CHECK(pinnedFromToken != nullptr);

    std::shared_ptr<void> pinnedMutFromToken;
    auto pinnedMutFromErr = payload.withSpanMutableRawFromPinned(
        1099, [&](void* data, std::size_t count, std::shared_ptr<void> const& token) {
            pinnedMutFromToken = token;
            if (count > 0) {
                static_cast<int*>(data)[0] = -2;
            }
        });
    CHECK_FALSE(pinnedMutFromErr.has_value());
    CHECK(pinnedMutFromToken != nullptr);

    int front = 0;
    CHECK_FALSE(payload.read(&front));
    CHECK(front == 99);
}

TEST_CASE("PodPayload reservation, publish/rollback, and freeze waits for publish") {
    PodPayload<int> payload;

    // Reserve but do not publish yet so freezeForUpgrade must wait.
    auto reservation = payload.reserveOne();
    REQUIRE(reservation.has_value());
    *static_cast<int*>(reservation->ptr) = 7;

    std::atomic<bool> freezeStarted{false};
    std::atomic<bool> freezeFinished{false};

    std::thread freezer([&] {
        freezeStarted.store(true, std::memory_order_release);
        CHECK(payload.freezeForUpgrade());
        freezeFinished.store(true, std::memory_order_release);
    });

    // Wait until the freeze thread is spinning in waitForPublish().
    while (!freezeStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK_FALSE(freezeFinished.load(std::memory_order_acquire));

    // Publishing should unblock freezeForUpgrade().
    payload.publishOne(reservation->index);
    freezer.join();
    CHECK(freezeFinished.load(std::memory_order_acquire));

    // After freeze, pushes should fail and reserveOne should return nullopt.
    int value = 42;
    CHECK_FALSE(payload.push(value));
    CHECK_FALSE(payload.pushValue(&value));
    CHECK_FALSE(payload.reserveOne().has_value());
}

TEST_CASE("PodPayload pack span marker updates through popCount and rollback") {
    PodPayload<int> payload;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(payload.push(i));
    }

    payload.markPackSpanStart(0);
    // popCount should advance the packSpanStart marker.
    CHECK_FALSE(payload.popCount(2).has_value());
    auto marker = payload.packSpanStart();
    REQUIRE(marker.has_value());
    CHECK(*marker == 2);

    // Reserve + rollback leaves size unchanged.
    auto reservation = payload.reserveOne();
    REQUIRE(reservation.has_value());
    payload.rollbackOne(reservation->index);
    CHECK(payload.size() == 3);
}
}
