#include "core/PodPayload.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <cstddef>
#include <vector>
#include <memory>

using namespace SP;

namespace {
std::atomic<int> gHookCalls{0};
void incrementHook() { gHookCalls.fetch_add(1, std::memory_order_relaxed); }
} // namespace

TEST_SUITE("core.podpayload.coverage") {
TEST_CASE("PodPayload exercises push/read/take/span paths") {
    PodPayload<int> payload;

    // Hook should fire on every successful push.
    testing::SetPodPayloadPushHook(&incrementHook);
    CHECK(payload.push(1));
    CHECK(payload.push(2));
    CHECK(gHookCalls.load(std::memory_order_relaxed) == 2);
    testing::SetPodPayloadPushHook(nullptr);

    int value = 0;
    auto readErr = payload.read(&value);
    CHECK_FALSE(readErr.has_value());
    CHECK(value == 1);

    auto takeErr = payload.take(&value);
    CHECK_FALSE(takeErr.has_value());
    CHECK(value == 1);

    // Pop beyond available should surface an error.
    auto popErr = payload.popCount(5);
    CHECK(popErr.has_value());
    CHECK(popErr->code == Error::Code::NoObjectFound);

    // Push more data to cover span helpers.
    CHECK(payload.push(10));
    CHECK(payload.push(20));
    CHECK(payload.push(30));

    std::vector<int> seen;
    auto spanErr = payload.withSpan([&](std::span<const int> sp) { seen.assign(sp.begin(), sp.end()); });
    CHECK_FALSE(spanErr.has_value());
    CHECK(seen.size() == payload.size());

    // Mutable span should allow in-place edits.
    auto mutErr = payload.withSpanMutable([](std::span<int> sp) {
        for (auto& v : sp) {
            v += 1;
        }
    });
    CHECK_FALSE(mutErr.has_value());

    // Raw span from an offset (skip first element after prior take).
    std::size_t rawCount = 0;
    auto rawErr = payload.withSpanRawFrom(
        1, [&](void const* data, std::size_t count) { rawCount = count; CHECK(static_cast<int const*>(data)[0] == 3); });
    CHECK_FALSE(rawErr.has_value());
    CHECK(rawCount == payload.size());

    // Mutable raw span with shared_ptr token.
    std::size_t pinnedCount = 0;
    auto pinnedErr = payload.withSpanMutableRawFromPinned(
        0, [&](void* data, std::size_t count, std::shared_ptr<void> const& token) {
            pinnedCount = count;
            CHECK(token != nullptr);
            if (count > 0) {
                static_cast<int*>(data)[0] = 99;
            }
        });
    CHECK_FALSE(pinnedErr.has_value());
    CHECK(pinnedCount >= 1);

    int front = 0;
    CHECK_FALSE(payload.read(&front));
    CHECK(front == 99);

    // Pack span markers.
    CHECK_FALSE(payload.packSpanStart().has_value());
    payload.markPackSpanStart(0);
    auto marker = payload.packSpanStart();
    CHECK(marker.has_value());
    CHECK(*marker == 0);
}

TEST_CASE("PodPayload reserve/publish/rollback and freeze") {
    PodPayload<int> payload;

    auto reservation = payload.reserveOne();
    REQUIRE(reservation.has_value());
    auto idx = reservation->index;
    *static_cast<int*>(reservation->ptr) = 7;

    // Publish then verify size accounting.
    payload.publishOne(idx);
    int out = 0;
    CHECK_FALSE(payload.read(&out));
    CHECK(out == 7);

    // Rollback of next reservation should restore tail.
    auto reservation2 = payload.reserveOne();
    REQUIRE(reservation2.has_value());
    payload.rollbackOne(reservation2->index);
    // After rollback, publishing should no-op at same index.
    payload.publishOne(reservation->index);

    // Freezing prevents further pushes.
    CHECK(payload.freezeForUpgrade());
    CHECK_FALSE(payload.push(123));

    // Popping available element should succeed after freeze.
    CHECK_FALSE(payload.popCount(1));
}
}
