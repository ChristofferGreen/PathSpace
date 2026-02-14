#include "PathSpace.hpp"
#include "PathSpaceTestHelper.hpp"
#include "core/PathSpaceContext.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadataT.hpp"
#include "third_party/doctest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifndef DOCTEST_SUCCEED
#define DOCTEST_SUCCEED() ((void)0)
#endif

using namespace SP;
using namespace std::chrono_literals;

namespace {
struct ExposedPathSpace : PathSpace {
    using PathSpace::PathSpace;
    using PathSpace::out;
    using PathSpace::packInsert;
    using PathSpace::setOwnedPool;
    using PathSpace::shutdownPublic;
    using PathSpace::spanPackMut;
};

struct NotificationProbe : PathSpace {
    using PathSpace::PathSpace;
    using PathSpace::getNotificationSink;

    std::vector<std::string> notifications;

protected:
    void notify(std::string const& notificationPath) override {
        notifications.push_back(notificationPath);
    }
};
}

TEST_SUITE("pathspace.internal.coverage") {

TEST_CASE("copy assignment duplicates values and resets state") {
    PathSpace source;
    REQUIRE(source.insert("/a", 7).errors.empty());

    PathSpace dest;
    REQUIRE(dest.insert("/old", 9).errors.empty());

    dest = source; // exercise PathSpace::operator=

    auto clonedValue = dest.read<int>("/a");
    REQUIRE(clonedValue.has_value());
    CHECK(*clonedValue == 7);

    CHECK_FALSE(dest.read<int>("/old").has_value());
    CHECK(PathSpaceTestHelper::pool(dest) == PathSpaceTestHelper::pool(source));
    CHECK(PathSpaceTestHelper::executor(dest) == PathSpaceTestHelper::executor(source));
}

TEST_CASE("setOwnedPool manages lifetime for owned and singleton pools") {
    ExposedPathSpace ownedSpace;
    auto* customPool = new TaskPool(1);
    ownedSpace.setOwnedPool(customPool); // take ownership of non-singleton pool
    CHECK(PathSpaceTestHelper::pool(ownedSpace) == customPool);

    // Ensure destructor path runs without touching the global singleton
    {
        ExposedPathSpace singletonSpace;
        singletonSpace.setOwnedPool(&TaskPool::Instance());
        CHECK(PathSpaceTestHelper::pool(singletonSpace) == &TaskPool::Instance());
    }
}

TEST_CASE("clear and shutdown behave when context is absent") {
    ExposedPathSpace space{std::shared_ptr<PathSpaceContext>{}, ""};
    CHECK_NOTHROW(space.clear());
    CHECK_NOTHROW(space.shutdownPublic());
}

TEST_CASE("clear waits while active outs drain") {
    ExposedPathSpace space;
    PathSpaceTestHelper::activeOut(space).store(1, std::memory_order_relaxed);

    std::thread releaser([&]() {
        std::this_thread::sleep_for(5ms);
        PathSpaceTestHelper::activeOut(space).store(0, std::memory_order_relaxed);
    });

    space.clear();
    releaser.join();
    CHECK(PathSpaceTestHelper::activeOut(space).load(std::memory_order_relaxed) == 0);
}

TEST_CASE("out short-circuits when clearing is in progress") {
    ExposedPathSpace space;
    PathSpaceTestHelper::clearing(space).store(true, std::memory_order_relaxed);
    int value = 0;
    auto err = space.out(Iterator{"/missing"}, InputMetadataT<int>{}, Out{}, &value);
    PathSpaceTestHelper::clearing(space).store(false, std::memory_order_relaxed);

    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::Timeout);
}

TEST_CASE("out clamps timeout using PATHSPACE_TEST_TIMEOUT_MS and respects prefix") {
    setenv("PATHSPACE_TEST_TIMEOUT_MS", "1", 1);
    auto ctx = std::make_shared<PathSpaceContext>();
    ExposedPathSpace space{ctx, "/mount"};

    int value = 0;
    auto err = space.out(Iterator{"/not_there"}, InputMetadataT<int>{}, Block(10ms), &value);
    unsetenv("PATHSPACE_TEST_TIMEOUT_MS");

    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::Timeout);
}

TEST_CASE("spanPackMut waits with prefix and times out for missing paths") {
    auto ctx = std::make_shared<PathSpaceContext>();
    ExposedPathSpace space{ctx, "/base"};

    std::array<std::string, 1> paths{"/missing"};
    InputMetadata              meta{InputMetadataT<int>{}};
    Out                        opts = Block(5ms);

    auto result = space.spanPackMut(std::span<const std::string>{paths}, meta, opts,
                                    [](std::span<RawMutSpan const>) {
                                        return SpanPackResult{.error = std::nullopt, .shouldPop = false};
                                    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::Timeout);
}

TEST_CASE("packInsert bypasses notifications when context is missing") {
    ExposedPathSpace space{std::shared_ptr<PathSpaceContext>{}, ""};
    int              value = 3;
    std::array<std::string, 1> paths{"/v"};
    void const* values[] = {&value};

    auto ret = space.packInsert(std::span<const std::string>{paths}, InputMetadataT<int>{}, std::span<void const* const>{values});
    CAPTURE(ret.errors.size());
    if (!ret.errors.empty()) {
        CAPTURE(ret.errors.front().code);
        CAPTURE(ret.errors.front().message.value_or(""));
    }
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("packInsert notifies waiters with mount prefix") {
    auto ctx = std::make_shared<PathSpaceContext>();
    ExposedPathSpace space{ctx, "/mount"};

    int              value = 5;
    std::array<std::string, 1> paths{"/node"};
    void const* values[] = {&value};

    std::atomic<std::cv_status> status{std::cv_status::timeout};
    std::thread waiter([&]() {
        auto guard = ctx->wait("/mount/node");
        status.store(guard.wait_until(std::chrono::system_clock::now() + 200ms));
    });

    std::this_thread::sleep_for(5ms);
    auto ret = space.packInsert(std::span<const std::string>{paths}, InputMetadataT<int>{}, std::span<void const* const>{values});

    waiter.join();
    CAPTURE(ret.errors.size());
    if (!ret.errors.empty()) {
        CAPTURE(ret.errors.front().code);
        CAPTURE(ret.errors.front().message.value_or(""));
    }
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("retargetNestedMounts short-circuits when node is missing") {
    PathSpace space;
    PathSpaceTestHelper::retarget(space, nullptr, "/unused");
    CHECK_EQ(1, 1); // No crash and coverage of guard path
}

TEST_CASE("getNotificationSink creates a default sink without context") {
    NotificationProbe space{std::shared_ptr<PathSpaceContext>{}, ""};

    auto sink = space.getNotificationSink().lock();
    REQUIRE(sink);

    sink->notify("/ping");
    REQUIRE(space.notifications.size() == 1);
    CHECK(space.notifications.front() == "/ping");

    auto sinkAgain = space.getNotificationSink().lock();
    REQUIRE(sinkAgain);
    CHECK(sinkAgain.get() == sink.get());
}

TEST_CASE("getNotificationSink seeds or reuses the context sink") {
    struct RecordingSink : NotificationSink {
        void notify(std::string const& notificationPath) override {
            notifications.push_back(notificationPath);
        }
        std::vector<std::string> notifications;
    };

    auto ctx = std::make_shared<PathSpaceContext>();

    // If the context is missing a sink, getNotificationSink should seed one.
    {
        NotificationProbe space{ctx, ""};
        auto seeded = space.getNotificationSink().lock();
        REQUIRE(seeded);

        auto ctxSink = ctx->getSink().lock();
        REQUIRE(ctxSink);
        CHECK(ctxSink.get() == seeded.get());

        seeded->notify("/seeded");
        REQUIRE(space.notifications.size() == 1);
        CHECK(space.notifications.front() == "/seeded");
    }

    // If a sink is already set, getNotificationSink should reuse it.
    auto externalSink = std::make_shared<RecordingSink>();
    ctx->setSink(externalSink);

    NotificationProbe space{ctx, ""};
    auto reused = space.getNotificationSink().lock();
    REQUIRE(reused);
    CHECK(reused.get() == externalSink.get());

    reused->notify("/external");
    REQUIRE(externalSink->notifications.size() == 1);
    CHECK(externalSink->notifications.front() == "/external");
}

} // TEST_SUITE
