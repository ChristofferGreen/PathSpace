#include "third_party/doctest.h"

#include <pathspace/layer/BoundedPathSpace.hpp>
#include <pathspace/PathSpace.hpp>

#include <string>

using namespace SP;

namespace {
struct TestEvent {
    int payload = 0;
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

TEST_SUITE_END();
