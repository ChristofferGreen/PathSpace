#include "third_party/doctest.h"

#define protected public
#include <pathspace/layer/BoundedPathSpace.hpp>
#include <pathspace/PathSpace.hpp>
#undef protected

#include <pathspace/core/Node.hpp>
#include <pathspace/core/PathSpaceContext.hpp>

#include <string>
#include <atomic>
#include <mutex>

using namespace SP;

namespace {
struct TestEvent {
    int payload = 0;
};

struct RecordingPathSpace : PathSpace {
    using PathSpace::PathSpace;

    void notify(std::string const& notificationPath) override {
        std::lock_guard<std::mutex> lock(mutex_);
        notifications.push_back(notificationPath);
        PathSpace::notify(notificationPath);
    }

    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            adoptedPrefix  = prefix;
            adoptedContext = context;
        }
        PathSpace::adoptContextAndPrefix(std::move(context), std::move(prefix));
    }

    auto shutdown() -> void override {
        shutdownCalled.store(true);
        PathSpace::shutdown();
    }

    std::vector<std::string> flushNotifications() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = notifications;
        notifications.clear();
        return out;
    }

    std::atomic<bool>                        shutdownCalled{false};
    std::vector<std::string>                 notifications;
    std::string                              adoptedPrefix;
    std::shared_ptr<PathSpaceContext>        adoptedContext;
    std::mutex                               mutex_;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.bounded");

TEST_CASE("BoundedPathSpace pops oldest to allow insert") {
    auto backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 2};

    bounded.in(Iterator{"/queue"}, InputData{TestEvent{1}});
    bounded.in(Iterator{"/queue"}, InputData{TestEvent{2}});
    bounded.in(Iterator{"/queue"}, InputData{TestEvent{3}}); // pops 1, keeps 2,3

    auto v1 = backing->take<TestEvent>("/queue");
    auto v2 = backing->take<TestEvent>("/queue");
    auto v3 = backing->take<TestEvent>("/queue");

    REQUIRE(v1.has_value());
    REQUIRE(v2.has_value());
    CHECK(v1->payload == 2);
    CHECK(v2->payload == 3);
    CHECK_FALSE(v3.has_value());
}

TEST_CASE("BoundedPathSpace works mounted under /devices/test") {
    PathSpace root;
    auto backing = std::make_shared<PathSpace>();
    auto bounded = std::make_unique<BoundedPathSpace>(backing, 1);
    auto* raw    = bounded.get();
    auto r = root.insert<"/devices/test">(std::move(bounded));
    REQUIRE(r.errors.empty());

    raw->in(Iterator{"/queue"}, InputData{TestEvent{10}});
    raw->in(Iterator{"/queue"}, InputData{TestEvent{20}});

    auto val = root.take<TestEvent>("/devices/test/queue");
    REQUIRE(val.has_value());
    CHECK(val->payload == 20);
}

TEST_CASE("BoundedPathSpace allows insert after pop via take") {
    auto backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 1};

    bounded.in(Iterator{"/queue"}, InputData{TestEvent{1}});
    // Pop via take should decrement count.
    TestEvent tmp{};
    auto popped = bounded.out(Iterator{"/queue"}, InputMetadataT<TestEvent>{}, Out{.doPop = true}, &tmp);
    CHECK_FALSE(popped.has_value());
    CHECK(tmp.payload == 1);

    bounded.in(Iterator{"/queue"}, InputData{TestEvent{2}});
    auto val = backing->take<TestEvent>("/queue");
    REQUIRE(val.has_value());
    CHECK(val->payload == 2);
}

TEST_CASE("BoundedPathSpace rejects on type mismatch and preserves existing data") {
    auto backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 1};

    bounded.in(Iterator{"/queue"}, InputData{std::string{"old"}});
    bounded.in(Iterator{"/queue"}, InputData{TestEvent{99}}); // should fail to pop string, drop insert

    auto s = backing->take<std::string>("/queue");
    REQUIRE(s.has_value());
    CHECK(*s == "old");
    auto none = backing->take<TestEvent>("/queue");
    CHECK_FALSE(none.has_value());
}

TEST_CASE("BoundedPathSpace preserves caller object after pop loop") {
    auto backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 1};

    bounded.in(Iterator{"/queue"}, InputData{TestEvent{1}});
    TestEvent incoming{99};
    bounded.in(Iterator{"/queue"}, InputData{incoming}); // should pop 1, insert 99

    CHECK(incoming.payload == 99); // caller object restored
    auto val = backing->take<TestEvent>("/queue");
    REQUIRE(val.has_value());
    CHECK(val->payload == 99);
}

TEST_CASE("BoundedPathSpace replaceExistingPayload resets counts and maxItems floor") {
    auto backing = std::make_shared<PathSpace>();

    SUBCASE("replaceExistingPayload clears previous queue") {
        BoundedPathSpace bounded{backing, 2};
        TestEvent first{1};
        TestEvent second{2};
        bounded.in(Iterator{"/queue"}, InputData{first});
        bounded.in(Iterator{"/queue"}, InputData{second});

        TestEvent resetValue{9};
        InputData reset{resetValue};
        reset.replaceExistingPayload = true;
        auto ins = bounded.in(Iterator{"/queue"}, reset);
        CHECK(ins.errors.empty());

        auto poppedFirst = backing->take<TestEvent>("/queue");
        auto poppedSecond = backing->take<TestEvent>("/queue");
        REQUIRE(poppedFirst.has_value());
        CHECK(poppedFirst->payload == 9);
        CHECK_FALSE(poppedSecond.has_value());
    }

    SUBCASE("zero maxItems still enforces at least one slot") {
        BoundedPathSpace bounded{backing, 0}; // coerces to 1
        TestEvent first{4};
        TestEvent second{5};
        bounded.in(Iterator{"/queue"}, InputData{first});
        bounded.in(Iterator{"/queue"}, InputData{second}); // should evict 4

        auto popped = backing->take<TestEvent>("/queue");
        REQUIRE(popped.has_value());
        CHECK(popped->payload == 5);
        CHECK_FALSE(backing->take<TestEvent>("/queue").has_value());
    }
}

TEST_CASE("BoundedPathSpace forwards visit and exposes backing root node") {
    auto backing = std::make_shared<PathSpace>();
    BoundedPathSpace bounded{backing, 2};

    backing->insert("/alpha", 7);

    int visited = 0;
    VisitOptions opts;
    opts.includeValues = true;
    auto visitResult = bounded.visit(
        [&](PathEntry const&, ValueHandle&) {
            ++visited;
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(visitResult.has_value());
    CHECK(visited >= 1);

    CHECK(bounded.getRootNode() == backing->getRootNode());

    BoundedPathSpace missing{nullptr, 1};
    CHECK(missing.getRootNode() == nullptr);
}

TEST_CASE("BoundedPathSpace surfaces backing errors and forwards control helpers") {
    SUBCASE("Missing backing surfaces invalid permissions") {
        BoundedPathSpace bounded{nullptr, 2};

        auto ins = bounded.in(Iterator{"/queue"}, InputData{TestEvent{5}});
        CHECK_FALSE(ins.errors.empty());
        CHECK(ins.errors.front().code == Error::Code::InvalidPermissions);

        TestEvent tmp{};
        auto err = bounded.out(Iterator{"/queue"}, InputMetadataT<TestEvent>{}, Out{.doPop = true}, &tmp);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::InvalidPermissions);

        auto visited = bounded.visit(
            [](PathEntry const&, ValueHandle&) { return VisitControl::Continue; }, {});
        CHECK_FALSE(visited.has_value());
        CHECK(visited.error().code == Error::Code::InvalidPermissions);
    }

    SUBCASE("Control paths forward to backing PathSpace") {
        auto backing = std::make_shared<RecordingPathSpace>();
        BoundedPathSpace bounded{backing, 3};

        auto context = std::make_shared<PathSpaceContext>();
        bounded.adoptContextAndPrefix(context, "/mounted");
        CHECK(backing->adoptedContext == context);
        CHECK(backing->adoptedPrefix == "/mounted");

        bounded.notify("/foo");
        auto notes = backing->flushNotifications();
        CHECK(notes == std::vector<std::string>{"/foo"});

        bounded.shutdown();
        CHECK(backing->shutdownCalled.load());
    }
}

TEST_SUITE_END();
