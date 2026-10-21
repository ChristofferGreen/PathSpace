#define private public
#include "core/PodPayload.hpp"
#undef private

#include "third_party/doctest.h"

#include <memory>

using namespace SP;

namespace {
struct DummyPayload final : PodPayloadBase {
    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const&) const noexcept override { return true; }
    std::size_t size() const noexcept override { return 0; }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0; }
    std::size_t publishedTail() const noexcept override { return 0; }
    InputMetadata const& podMetadata() const noexcept override {
        static InputMetadata meta{InputMetadataT<int>{}};
        return meta;
    }
    bool pushValue(void const*) override { return false; }
    std::optional<Error> readTo(void*) const override { return std::nullopt; }
    std::optional<Error> takeTo(void*) override { return std::nullopt; }
    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn) override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanRawFrom(std::size_t,
                                         std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRawFrom(std::size_t,
                                                std::function<void(void*, std::size_t)> const& fn) override {
        fn(nullptr, 0);
        return std::nullopt;
    }
    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return false; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}
};
} // namespace

TEST_SUITE("core.pod_payload.corruption") {
TEST_CASE("PodPayloadBase pinned wrappers call through raw spans") {
    DummyPayload payload;

    bool sawPinned = false;
    auto err = payload.withSpanRawPinned([&](void const*, std::size_t, std::shared_ptr<void> const& token) {
        sawPinned = true;
        CHECK(token == nullptr);
    });
    CHECK_FALSE(err.has_value());
    CHECK(sawPinned);

    bool sawPinnedMut = false;
    auto errMut = payload.withSpanMutableRawPinned([&](void*, std::size_t, std::shared_ptr<void> const& token) {
        sawPinnedMut = true;
        CHECK(token == nullptr);
    });
    CHECK_FALSE(errMut.has_value());
    CHECK(sawPinnedMut);

    bool sawFromPinned = false;
    auto errFrom = payload.withSpanRawFromPinned(1, [&](void const*, std::size_t, std::shared_ptr<void> const& token) {
        sawFromPinned = true;
        CHECK(token == nullptr);
    });
    CHECK_FALSE(errFrom.has_value());
    CHECK(sawFromPinned);

    bool sawFromPinnedMut = false;
    auto errFromMut = payload.withSpanMutableRawFromPinned(2, [&](void*, std::size_t, std::shared_ptr<void> const& token) {
        sawFromPinnedMut = true;
        CHECK(token == nullptr);
    });
    CHECK_FALSE(errFromMut.has_value());
    CHECK(sawFromPinnedMut);
}

TEST_CASE("PodPayload span helpers report corrupted indices") {
    PodPayload<int> payload;
    payload.head_.store(2, std::memory_order_relaxed);
    payload.publishedTail_.store(1, std::memory_order_relaxed);

    auto err = payload.withSpanRaw([](void const*, std::size_t) {});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);

    auto errMut = payload.withSpanMutableRaw([](void*, std::size_t) {});
    REQUIRE(errMut.has_value());
    CHECK(errMut->code == Error::Code::UnknownError);

    auto errPinned = payload.withSpanRawPinned([](void const*, std::size_t, std::shared_ptr<void> const&) {});
    REQUIRE(errPinned.has_value());
    CHECK(errPinned->code == Error::Code::UnknownError);

    auto errPinnedMut = payload.withSpanMutableRawPinned([](void*, std::size_t, std::shared_ptr<void> const&) {});
    REQUIRE(errPinnedMut.has_value());
    CHECK(errPinnedMut->code == Error::Code::UnknownError);

    auto errFrom = payload.withSpanRawFrom(1, [](void const*, std::size_t) {});
    REQUIRE(errFrom.has_value());
    CHECK(errFrom->code == Error::Code::UnknownError);

    auto errFromMut = payload.withSpanMutableRawFrom(1, [](void*, std::size_t) {});
    REQUIRE(errFromMut.has_value());
    CHECK(errFromMut->code == Error::Code::UnknownError);

    auto errFromPinned = payload.withSpanRawFromPinned(1, [](void const*, std::size_t, std::shared_ptr<void> const&) {});
    REQUIRE(errFromPinned.has_value());
    CHECK(errFromPinned->code == Error::Code::UnknownError);

    auto errFromPinnedMut = payload.withSpanMutableRawFromPinned(1, [](void*, std::size_t, std::shared_ptr<void> const&) {});
    REQUIRE(errFromPinnedMut.has_value());
    CHECK(errFromPinnedMut->code == Error::Code::UnknownError);
}

TEST_CASE("PodPayload read/take report buffer bounds errors") {
    PodPayload<int> payload;
    auto cap = payload.buffer_->capacity;
    payload.head_.store(cap + 1, std::memory_order_relaxed);
    payload.publishedTail_.store(cap + 2, std::memory_order_relaxed);

    int out = 0;
    auto readErr = payload.read(&out);
    REQUIRE(readErr.has_value());
    CHECK(readErr->code == Error::Code::UnknownError);

    payload.head_.store(cap + 1, std::memory_order_relaxed);
    payload.publishedTail_.store(cap + 2, std::memory_order_relaxed);

    auto takeErr = payload.take(&out);
    REQUIRE(takeErr.has_value());
    CHECK(takeErr->code == Error::Code::UnknownError);
}

TEST_CASE("PodPayload ensureCapacity handles small and oversized requests") {
    PodPayload<int> payload;
    auto cap = payload.buffer_->capacity;

    payload.ensureCapacity(1);
    CHECK(payload.buffer_->capacity == cap);

    payload.ensureCapacity(cap * 2);
    CHECK(payload.buffer_->capacity >= cap * 2 + 1);
}
} // TEST_SUITE
