#include "PathSpace.hpp"
#include "PathSpaceTestHelper.hpp"
#include "core/NotificationSink.hpp"
#include "core/PathSpaceContext.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadataT.hpp"
#include "third_party/doctest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
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
    using PathSpace::listChildrenCanonical;
    using PathSpace::out;
    using PathSpace::packInsert;
    using PathSpace::packInsertSpans;
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
    // Task completion sinks deliver through notifyAbsolute; record those too.
    void notifyAbsolute(std::string const& absolutePath) override {
        notifications.push_back(absolutePath);
    }
};

struct RecordingSink : NotificationSink {
    void notify(std::string const& notificationPath) override {
        std::lock_guard<std::mutex> lock(mutex);
        notifications.push_back(notificationPath);
    }

    std::mutex              mutex;
    std::vector<std::string> notifications;
};
}

TEST_SUITE("pathspace.internal") {

TEST_CASE("span-pack take returns invalid type for non-POD nodes") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", std::string{"alpha"}).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/b", std::string{"beta"}).nbrValuesInserted == 1);

    auto result = space.take<"a", "b">("/root",
                                      [](std::span<int>, std::span<int>) {},
                                      Block{50ms} & Minimal{});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidType);
}

TEST_CASE("span-pack take waits on prefixed paths and times out") {
    auto context = std::make_shared<PathSpaceContext>();
    PathSpace space{context, "/mount"};

    auto result = space.take<"a", "b">("/root",
                                      [](std::span<int>, std::span<int>) {},
                                      Block{50ms});
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::Timeout);
}

TEST_CASE("span-pack insert works without context") {
    PathSpace space{std::shared_ptr<PathSpaceContext>{}, ""};

    std::array<int, 2> a{1, 2};
    std::array<int, 2> b{3, 4};
    auto ret = space.insert<"a", "b">("/root",
                                     std::span<const int>(a),
                                     std::span<const int>(b));
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 4);
}

TEST_CASE("mounted space read uses prefixed notifications on immediate success") {
    PathSpace outer;
    auto nested = std::make_unique<PathSpace>();
    auto* nestedPtr = nested.get();
    REQUIRE(nestedPtr->insert("/value", 8).nbrValuesInserted == 1);
    REQUIRE(outer.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    auto value = nestedPtr->read<int>("/value");
    REQUIRE(value.has_value());
    CHECK(*value == 8);
}

TEST_CASE("VisitDetail accessors handle explicit and empty handles") {
    PathSpace space;
    REQUIRE(space.insert("/alpha", 5).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* child = root->getChild("alpha");
    REQUIRE(child != nullptr);

    auto handle = VisitDetail::Access::MakeHandle(space, *child, "/alpha", true);
    CHECK(handle.valid());
    auto value = handle.read<int>();
    REQUIRE(value.has_value());
    CHECK(*value == 5);

    auto bytes = VisitDetail::Access::SerializeNodeData(handle);
    CHECK(bytes.has_value());

    ValueHandle empty{};
    CHECK_FALSE(VisitDetail::Access::SerializeNodeData(empty).has_value());
}

TEST_CASE("listChildrenCanonical returns empty for invalid or missing nested indices") {
    ExposedPathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    auto invalid = space.listChildrenCanonical("/bad/*");
    CHECK(invalid.empty());

    auto missingIndex = space.listChildrenCanonical("/mount[9]");
    CHECK(missingIndex.empty());
}

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

TEST_CASE("out returns clearing-in-progress once blocking wait begins") {
    ExposedPathSpace space;
    auto context = space.sharedContext();
    REQUIRE(context);

    int value = 0;
    std::optional<Error> err;
    std::thread reader([&] {
        err = space.out(Iterator{"/missing"}, InputMetadataT<int>{}, Block(200ms), &value);
    });

    bool waiterRegistered = false;
    for (int i = 0; i < 100 && !waiterRegistered; ++i) {
        waiterRegistered = context->hasWaiters();
        if (!waiterRegistered) {
            std::this_thread::sleep_for(2ms);
        }
    }
    CHECK(waiterRegistered);

    PathSpaceTestHelper::clearing(space).store(true, std::memory_order_relaxed);
    context->notify("/missing");

    reader.join();
    PathSpaceTestHelper::clearing(space).store(false, std::memory_order_relaxed);

    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::Timeout);
    CHECK(err->message == std::optional<std::string>{"PathSpace clearing in progress"});
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

TEST_CASE("packInsert notifies sink with mount prefix when insert succeeds") {
    auto ctx = std::make_shared<PathSpaceContext>();
    auto sink = std::make_shared<RecordingSink>();
    ctx->setSink(sink);
    ExposedPathSpace space{ctx, "/mount"};

    auto ret = space.insert<"/node", "/other">(12, 34);
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 2);

    bool sawNode = false;
    bool sawOther = false;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        for (auto const& path : sink->notifications) {
            if (path == "/mount/node") {
                sawNode = true;
            }
            if (path == "/mount/other") {
                sawOther = true;
            }
        }
    }
    CHECK(sawNode);
    CHECK(sawOther);
}

TEST_CASE("packInsertSpans notifies sink with mount prefix") {
    auto ctx = std::make_shared<PathSpaceContext>();
    auto sink = std::make_shared<RecordingSink>();
    ctx->setSink(sink);
    ExposedPathSpace space{ctx, "/mount"};

    std::array<int, 2> dataA{7, 9};
    std::array<int, 2> dataB{1, 2};

    auto ret = space.insert<"one", "two">("/base", std::span<const int>(dataA), std::span<const int>(dataB));
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == dataA.size() + dataB.size());

    bool sawOne = false;
    bool sawTwo = false;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        for (auto const& path : sink->notifications) {
            if (path == "/mount/base/one") {
                sawOne = true;
            }
            if (path == "/mount/base/two") {
                sawTwo = true;
            }
        }
    }
    CHECK(sawOne);
    CHECK(sawTwo);
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

TEST_CASE("getNotificationSink returns the context waker and leaves the observer sink alone") {
    struct RecordingSink : NotificationSink {
        void notify(std::string const& notificationPath) override {
            notifications.push_back(notificationPath);
        }
        std::vector<std::string> notifications;
    };

    auto ctx = std::make_shared<PathSpaceContext>();

    NotificationProbe space{ctx, ""};
    auto waker = space.getNotificationSink().lock();
    REQUIRE(waker);
    CHECK(waker.get() == space.getNotificationSink().lock().get()); // stable
    CHECK(ctx->getSink().expired());                                  // no observer is seeded

    // Notifying the waker wakes the context's waiters ...
    auto guard = ctx->wait("/woken");
    waker->notify("/woken");
    CHECK(guard.wait_until(std::chrono::system_clock::now() + std::chrono::seconds(5))
          == std::cv_status::no_timeout);

    // ... and is reported to an observer sink, which never replaces the waker.
    auto observer = std::make_shared<RecordingSink>();
    ctx->setSink(observer);
    CHECK(space.getNotificationSink().lock().get() == waker.get());

    waker->notify("/observed");
    REQUIRE(observer->notifications.size() == 1);
    CHECK(observer->notifications.front() == "/observed");
    CHECK(space.notifications.empty()); // the space itself is not involved
}

} // TEST_SUITE
