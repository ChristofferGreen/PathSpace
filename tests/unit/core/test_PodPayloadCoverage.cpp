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

TEST_CASE("PodPayload miscellaneous edge paths") {
    PodPayload<int> payload;

    // take/read on empty payload should surface NoObjectFound.
    int out = 0;
    auto takeErr = payload.takeTo(&out);
    CHECK(takeErr.has_value());
    CHECK(takeErr->code == Error::Code::NoObjectFound);

    auto readErr = payload.readTo(&out);
    CHECK(readErr.has_value());
    CHECK(readErr->code == Error::Code::NoObjectFound);

    // popCount with zero is a no-op.
    CHECK_FALSE(payload.popCount(0));

    // pushValue rejects null pointers.
    CHECK_FALSE(payload.pushValue(nullptr));

    // withSpanRaw on empty queue should yield zero-length span.
    std::size_t seen = 0;
    auto spanErr = payload.withSpanRaw([&](void const*, std::size_t count) { seen = count; });
    CHECK_FALSE(spanErr.has_value());
    CHECK(seen == 0);
}

TEST_CASE("PodPayload grows capacity and preserves contents") {
    PodPayload<int> payload;
    // Initial capacity is 1024; push past it to force ensureCapacity().
    int last = -1;
    for (int i = 0; i < 1100; ++i) {
        CHECK(payload.push(i));
        last = i;
    }
    // Confirm tail size and last element via span.
    std::size_t spanSize = 0;
    int         tailValue = -1;
    auto spanErr = payload.withSpanRaw([&](void const* data, std::size_t count) {
        spanSize = count;
        if (count > 0) {
            tailValue = static_cast<int const*>(data)[count - 1];
        }
    });
    CHECK_FALSE(spanErr.has_value());
    CHECK(spanSize == 1100);
    CHECK(tailValue == last);
}

TEST_CASE("PodPayload freezeForUpgrade blocks future pushes") {
    PodPayload<int> payload;
    CHECK(payload.push(7));

    CHECK(payload.freezeForUpgrade());
    CHECK_FALSE(payload.push(8)); // further pushes blocked
    CHECK_FALSE(payload.freezeForUpgrade()); // idempotent false on second call

    int out = 0;
    CHECK_FALSE(payload.read(&out));
    CHECK(out == 7);
}

TEST_CASE("PodPayload pinned spans respect start indices") {
    PodPayload<int> payload;
    CHECK(payload.push(10));
    CHECK(payload.push(20));
    CHECK(payload.push(30));

    std::size_t count = 0;
    int         first = -1;
    auto err = payload.withSpanRawFromPinned(
        2, [&](void const* data, std::size_t c, std::shared_ptr<void> const&) {
            count = c;
            if (c > 0) {
                first = static_cast<int const*>(data)[0];
            }
        });
    CHECK_FALSE(err.has_value());
    CHECK(count == 1);
    CHECK(first == 30);

    std::size_t emptyCount = 0;
    auto err2 = payload.withSpanMutableRawFromPinned(
        10, [&](void* data, std::size_t c, std::shared_ptr<void> const&) {
            emptyCount = c;
            (void)data;
        });
    CHECK_FALSE(err2.has_value());
    CHECK(emptyCount == 0);
}

TEST_CASE("PodPayload pack span marker advances past popped elements and only grows") {
    PodPayload<int> payload;
    CHECK(payload.push(1));
    CHECK(payload.push(2));
    CHECK(payload.push(3));

    // Mark start after first element, then pop two to force marker advancement.
    payload.markPackSpanStart(1);
    auto initialMarker = payload.packSpanStart();
    REQUIRE(initialMarker.has_value());
    CHECK(*initialMarker == 1);

    CHECK_FALSE(payload.popCount(2));
    auto advancedMarker = payload.packSpanStart();
    REQUIRE(advancedMarker.has_value());
    CHECK(*advancedMarker == 2); // advanced to head after popCount

    // Subsequent marks below the current marker should be ignored.
    payload.markPackSpanStart(1);
    auto stableMarker = payload.packSpanStart();
    REQUIRE(stableMarker.has_value());
    CHECK(*stableMarker == 2);

    // Marking a higher index should take effect.
    payload.markPackSpanStart(5);
    auto raisedMarker = payload.packSpanStart();
    REQUIRE(raisedMarker.has_value());
    CHECK(*raisedMarker == 5);
}

TEST_CASE("PodPayload reserveOne returns nullopt when frozen and pinned spans handle empty buffers") {
    PodPayload<int> payload;

    CHECK(payload.freezeForUpgrade());
    auto reservation = payload.reserveOne();
    CHECK_FALSE(reservation.has_value());

    std::size_t pinnedCount = 123;
    auto err = payload.withSpanRawPinned(
        [&](void const* data, std::size_t count, std::shared_ptr<void> const& token) {
            pinnedCount = count;
            (void)data;
            CHECK(token != nullptr);
        });
    CHECK_FALSE(err.has_value());
    CHECK(pinnedCount == 0);

    std::size_t fromCount = 456;
    auto err2 = payload.withSpanMutableRawFromPinned(
        10, [&](void* data, std::size_t count, std::shared_ptr<void> const& token) {
            fromCount = count;
            (void)data;
            CHECK(token != nullptr);
        });
    CHECK_FALSE(err2.has_value());
    CHECK(fromCount == 0);
}

TEST_CASE("PodPayload handles non-trivial types and maintains order after growth") {
    struct NonTrivial {
        std::string value;
    };

    PodPayload<NonTrivial> payload;
    for (int i = 0; i < 1100; ++i) {
        NonTrivial entry;
        entry.value = std::to_string(i);
        CHECK(payload.push(entry));
    }

    NonTrivial front{};
    CHECK_FALSE(payload.read(&front));
    CHECK(front.value == "0");

    // Drop a large prefix to move head forward and cover pack-span advancement with non-POD.
    CHECK_FALSE(payload.popCount(1000));

    std::size_t spanSize = 0;
    auto spanErr = payload.withSpan([&](std::span<const NonTrivial> sp) {
        spanSize = sp.size();
        REQUIRE(!sp.empty());
        CHECK(sp.front().value == "1000");
        CHECK(sp.back().value == "1099");
    });
    CHECK_FALSE(spanErr.has_value());
    CHECK(spanSize == payload.size());
}

TEST_CASE("PodPayload reserve/rollback leaves tail unchanged and popCount guards bounds") {
    PodPayload<int> payload;
    auto reservation = payload.reserveOne();
    REQUIRE(reservation.has_value());
    payload.rollbackOne(reservation->index);
    auto err = payload.popCount(1);
    CHECK(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
    CHECK(payload.size() == 0);
}

TEST_CASE("PodPayload freezeForUpgrade blocks further writes and reservations") {
    PodPayload<int> payload;
    CHECK(payload.push(42));

    CHECK(payload.freezeForUpgrade());
    CHECK_FALSE(payload.push(7));
    auto reservation = payload.reserveOne();
    CHECK_FALSE(reservation.has_value());
    int out = 0;
    CHECK_FALSE(payload.read(&out));
    CHECK(out == 42);
}

TEST_CASE("PodPayload packSpanStart is empty before marking") {
    PodPayload<int> payload;
    auto marker = payload.packSpanStart();
    CHECK_FALSE(marker.has_value());
}
}
