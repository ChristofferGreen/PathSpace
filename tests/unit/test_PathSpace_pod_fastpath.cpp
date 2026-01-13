#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/core/NotificationSink.hpp>
#include <pathspace/core/PathSpaceContext.hpp>
#include "PathSpaceTestHelper.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <chrono>
#include <core/Leaf.hpp>
#include <optional>
#include <span>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>
#include <type_traits>
#include <future>
#include <array>

using namespace SP;

namespace {
std::atomic<bool>*    gHookHold    = nullptr;
std::atomic<bool>*    gHookSeen    = nullptr;
void pod_push_hook() {
    if (gHookSeen) {
        gHookSeen->store(true, std::memory_order_release);
    }
    if (gHookHold) {
        while (gHookHold->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
}

std::atomic<bool>* gPackPause = nullptr;
std::atomic<bool>* gPackSeen  = nullptr;
void pack_reservation_hook() {
    if (gPackSeen) {
        gPackSeen->store(true, std::memory_order_release);
    }
    if (gPackPause) {
        while (gPackPause->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
}

struct RecordingSink : NotificationSink {
    void notify(const std::string& notificationPath) override {
        std::lock_guard<std::mutex> lg(mutex);
        paths.push_back(notificationPath);
    }
    std::vector<std::string> paths;
    std::mutex               mutex;
};

static Node* lookup(Node* root, std::initializer_list<std::string_view> components) {
    Node* cur = root;
    for (auto c : components) {
        if (!cur) return nullptr;
        cur = cur->getChild(c);
    }
    return cur;
}
} // namespace

struct NonPod {
    std::string s;
};

struct Vec2 {
    float x;
    float y;
};
static_assert(std::is_trivially_copyable_v<Vec2>);

struct Vec3 {
    int x;
    int y;
    int z;
};
static_assert(std::is_trivially_copyable_v<Vec3>);


TEST_SUITE_BEGIN("pathspace.pod_fastpath");

TEST_CASE("POD fast path preserves FIFO for ints") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).nbrValuesInserted == 1);
    CHECK(space.insert("/ints", 2).nbrValuesInserted == 1);
    CHECK(space.insert("/ints", 3).nbrValuesInserted == 1);

    auto t1 = space.take<int>("/ints");
    REQUIRE(t1.has_value());
    CHECK(t1.value() == 1);
    auto t2 = space.take<int>("/ints");
    REQUIRE(t2.has_value());
    CHECK(t2.value() == 2);
    auto t3 = space.take<int>("/ints");
    REQUIRE(t3.has_value());
    CHECK(t3.value() == 3);
}

TEST_CASE("Span read works only on POD fast path") {
    PathSpace space;
    for (int i = 0; i < 5; ++i) {
        CHECK(space.insert("/ints", i).errors.empty());
    }
    std::vector<int> observed;
    auto ret = space.read("/ints", [&](std::span<const int> ints) {
        observed.assign(ints.begin(), ints.end());
    });
    REQUIRE(ret.has_value());
    CHECK(observed == std::vector<int>({0, 1, 2, 3, 4}));

    // Span read on non-POD path should fail fast.
    CHECK(space.insert("/obj", std::string("hello")).errors.empty());
    auto badSpan = space.read("/obj", [&](std::span<const int> /*ignored*/) {});
    CHECK_FALSE(badSpan.has_value());
    CHECK(badSpan.error().code == Error::Code::NotSupported);
}

TEST_CASE("Span read returns empty span on empty POD queue") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    auto popped = space.take<int>("/ints");
    REQUIRE(popped.has_value());

    std::vector<int> observed;
    auto ret = space.read("/ints", [&](std::span<const int> ints) {
        observed.assign(ints.begin(), ints.end());
    });
    REQUIRE(ret.has_value());
    CHECK(observed.empty());

    auto mut = space.take("/ints", [&](std::span<int> ints) {
        REQUIRE(ints.empty());
    });
    REQUIRE(mut.has_value());
}

TEST_CASE("Span read returns InvalidType on element mismatch") {
    PathSpace space;
    CHECK(space.insert("/ints", 5).errors.empty());

    auto span = space.read("/ints", [&](std::span<const float>) {});
    CHECK_FALSE(span.has_value());
    CHECK(span.error().code == Error::Code::InvalidType);

    auto val = space.take<int>("/ints");
    REQUIRE(val.has_value());
    CHECK(val.value() == 5);
}

TEST_CASE("Span read respects Block on empty POD queue") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    REQUIRE(space.take<int>("/ints").has_value());

    auto ret = space.read("/ints", [&](std::span<const int> ints) {
        CHECK(ints.empty());
    }, Out{} & Block{std::chrono::milliseconds(5)});
    REQUIRE(ret.has_value());
}

TEST_CASE("Span read handles concurrent upgrade and preserves order") {
    PathSpace space;
    CHECK(space.insert("/race", 1).errors.empty());
    CHECK(space.insert("/race", 2).errors.empty());

    std::barrier start(2);
    std::vector<int> observed;
    std::optional<Expected<void>> spanResult;

    std::thread reader([&]() {
        start.arrive_and_wait();
        spanResult = space.read("/race", [&](std::span<const int> ints) {
            observed.assign(ints.begin(), ints.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    });

    std::thread upgrader([&]() {
        start.arrive_and_wait();
        CHECK(space.insert("/race", std::string("upgrade")).errors.empty());
    });

    reader.join();
    upgrader.join();

    if (spanResult.has_value()) {
        if (!spanResult->has_value()) {
            CHECK(spanResult->error().code == Error::Code::NotSupported);
        }
    }

    auto first = space.take<int>("/race");
    REQUIRE(first.has_value());
    CHECK(first.value() == 1);
    auto second = space.take<int>("/race");
    REQUIRE(second.has_value());
    CHECK(second.value() == 2);
    auto third = space.take<std::string>("/race");
    REQUIRE(third.has_value());
    CHECK(third.value() == "upgrade");
}

TEST_CASE("Clone preserves upgraded POD ordering and disables span") {
    PathSpace space;
    CHECK(space.insert("/clone", 10).errors.empty());
    CHECK(space.insert("/clone", std::string("str")).errors.empty()); // upgrade to generic

    auto clone = space.clone();

    auto span = clone.read("/clone", [&](std::span<const int>) {});
    CHECK_FALSE(span.has_value());
    CHECK(span.error().code == Error::Code::NotSupported);

    auto first = clone.take<int>("/clone");
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);
    auto second = clone.take<std::string>("/clone");
    REQUIRE(second.has_value());
    CHECK(second.value() == "str");
}

TEST_CASE("ValueHandle read succeeds after upgrade during visit") {
    PathSpace space;
    CHECK(space.insert("/visit", 1).errors.empty());
    CHECK(space.insert("/visit", 2).errors.empty());

    bool readOk = false;
    auto ok = space.visit([&](PathEntry const& entry, ValueHandle& handle) {
        if (entry.path != "/visit") return VisitControl::Continue;
        CHECK(space.insert("/visit", std::string("later")).errors.empty()); // upgrade
        auto v = handle.read<int>();
        REQUIRE(v.has_value());
        CHECK(*v == 1);
        readOk = true;
        return VisitControl::Stop;
    });
    REQUIRE(ok);
    CHECK(readOk);
}

TEST_CASE("PodPayload resizes without losing data") {
    PathSpace space;
    constexpr int N = 1300; // > initial 1024
    for (int i = 0; i < N; ++i) {
        CHECK(space.insert("/big", i).errors.empty());
    }

    std::vector<int> seen;
    auto span = space.read("/big", [&](std::span<const int> ints) {
        seen.assign(ints.begin(), ints.end());
    });
    REQUIRE(span.has_value());
    CHECK(seen.size() == static_cast<size_t>(N));
    CHECK(seen.front() == 0);
    CHECK(seen.back() == N - 1);
}

TEST_CASE("Insert count suppresses when no waiters and parent has value") {
    PathSpace space;
    CHECK(space.insert("/parent", 1).errors.empty());

    auto ret = space.insert("/parent/child", 2);
    CHECK(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0); // suppressed because parent already had value and no waiters

    auto val = space.read<int>("/parent/child");
    REQUIRE(val.has_value());
    CHECK(val.value() == 2);
}

TEST_CASE("Span read and mutable span work on nested POD paths") {
    PathSpace space;
    CHECK(space.insert("/root/ints", 1).errors.empty());
    CHECK(space.insert("/root/ints", 2).errors.empty());
    CHECK(space.insert("/root/ints", 3).errors.empty());

    std::vector<int> observed;
    auto span = space.read("/root/ints", [&](std::span<const int> ints) {
        observed.assign(ints.begin(), ints.end());
    });
    REQUIRE(span.has_value());
    CHECK(observed == std::vector<int>({1, 2, 3}));

    auto mutate = space.take("/root/ints", [&](std::span<int> ints) {
        REQUIRE(ints.size() == 3);
        ints[0] = 10;
        ints[2] = 30;
    });
    REQUIRE(mutate.has_value());

    auto first = space.take<int>("/root/ints");
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);
    auto second = space.take<int>("/root/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 2);
    auto third = space.take<int>("/root/ints");
    REQUIRE(third.has_value());
    CHECK(third.value() == 30);
}

TEST_CASE("Type mismatch after POD insert upgrades while preserving order") {
    PathSpace space;
    CHECK(space.insert("/mixed", 7).errors.empty());
    auto ret = space.insert("/mixed", 1.5f);
    CHECK(ret.errors.empty());

    auto first = space.take<int>("/mixed");
    REQUIRE(first.has_value());
    CHECK(first.value() == 7);
    auto second = space.take<float>("/mixed");
    REQUIRE(second.has_value());
    CHECK(second.value() == doctest::Approx(1.5f));
}

TEST_CASE("Non-POD read on POD node returns InvalidType without consuming data") {
    PathSpace space;
    CHECK(space.insert("/ints", 11).errors.empty());

    auto wrong = space.read<std::string>("/ints");
    CHECK_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Error::Code::InvalidType);

    auto take = space.take<int>("/ints");
    REQUIRE(take.has_value());
    CHECK(take.value() == 11);
}

TEST_CASE("Non-POD after POD insert migrates to generic while preserving FIFO") {
    PathSpace space;
    CHECK(space.insert("/mixed", 42).errors.empty());
    NonPod np{"hi"};
    auto ret = space.insert("/mixed", np);
    CHECK(ret.errors.empty());
    auto first = space.take<int>("/mixed");
    REQUIRE(first.has_value());
    CHECK(first.value() == 42);
    auto second = space.take<NonPod>("/mixed");
    REQUIRE(second.has_value());
    CHECK(second->s == "hi");
}

TEST_CASE("Non-POD insert upgrades POD node and preserves FIFO") {
    PathSpace space;
    CHECK(space.insert("/upgrade", 1).errors.empty());
    CHECK(space.insert("/upgrade", 2).errors.empty());
    auto upgrade = space.insert("/upgrade", std::string("done"));
    CHECK(upgrade.errors.empty());

    auto first = space.take<int>("/upgrade");
    REQUIRE(first.has_value());
    CHECK(first.value() == 1);
    auto second = space.take<int>("/upgrade");
    REQUIRE(second.has_value());
    CHECK(second.value() == 2);
    auto final = space.take<std::string>("/upgrade");
    REQUIRE(final.has_value());
    CHECK(final.value() == "done");
}

TEST_CASE("POD node can still host child paths") {
    PathSpace space;
    CHECK(space.insert("/pod", 9).errors.empty());

    auto childInsert = space.insert("/pod/child", std::string("leaf"));
    CHECK(childInsert.errors.empty());

    auto childRead = space.read<std::string>("/pod/child");
    REQUIRE(childRead.has_value());
    CHECK(childRead.value() == "leaf");

    auto parentTake = space.take<int>("/pod");
    REQUIRE(parentTake.has_value());
    CHECK(parentTake.value() == 9);
}

TEST_CASE("Mixed POD types upgrade to generic while keeping queue order") {
    PathSpace space;
    CHECK(space.insert("/mixpod", 5).errors.empty());
    auto mixed = space.insert("/mixpod", 2.5f);
    CHECK(mixed.errors.empty());

    auto first = space.take<int>("/mixpod");
    REQUIRE(first.has_value());
    CHECK(first.value() == 5);
    auto second = space.take<float>("/mixpod");
    REQUIRE(second.has_value());
    CHECK(second.value() == doctest::Approx(2.5f));
}

TEST_CASE("Span read fails after POD node upgrades to generic") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    CHECK(space.insert("/ints", 2).errors.empty());
    auto preSpan = space.read("/ints", [&](std::span<const int> ints) {
        CHECK(ints.size() == 2);
    });
    REQUIRE(preSpan.has_value());

    CHECK(space.insert("/ints", std::string("up")).errors.empty()); // triggers upgrade
    auto postSpan = space.read("/ints", [&](std::span<const int>) {});
    CHECK_FALSE(postSpan.has_value());
    CHECK(postSpan.error().code == Error::Code::NotSupported);

    auto t1 = space.take<int>("/ints");
    REQUIRE(t1.has_value());
    CHECK(t1.value() == 1);
    auto t2 = space.take<int>("/ints");
    REQUIRE(t2.has_value());
    CHECK(t2.value() == 2);
    auto t3 = space.take<std::string>("/ints");
    REQUIRE(t3.has_value());
    CHECK(t3.value() == "up");
}

TEST_CASE("Mutable span rejects after POD node upgrade and preserves queue") {
    PathSpace space;
    CHECK(space.insert("/ints", 10).errors.empty());
    CHECK(space.insert("/ints", 20).errors.empty());
    CHECK(space.insert("/ints", 3.5f).errors.empty()); // upgrade to generic

    auto spanTake = space.take("/ints", [&](std::span<int> ints) {
        if (!ints.empty()) ints[0] = 999;
    });
    CHECK_FALSE(spanTake.has_value());
    CHECK(spanTake.error().code == Error::Code::NotSupported);

    auto first = space.take<int>("/ints");
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);
    auto second = space.take<int>("/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 20);
    auto third = space.take<float>("/ints");
    REQUIRE(third.has_value());
    CHECK(third.value() == doctest::Approx(3.5f));
}

TEST_CASE("Compile-time span read uses POD fast path") {
    PathSpace space;
    CHECK(space.insert("/ints", 4).errors.empty());
    CHECK(space.insert("/ints", 5).errors.empty());

    std::vector<int> seen;
    auto ret = space.read<"/ints">([&](std::span<const int> ints) {
        seen.assign(ints.begin(), ints.end());
    });
    REQUIRE(ret.has_value());
    CHECK(seen == std::vector<int>({4, 5}));
}

TEST_CASE("Compile-time span read returns NotSupported after upgrade") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    CHECK(space.insert("/ints", std::string("x")).errors.empty()); // upgrade to generic

    auto ret = space.read<"/ints">([&](std::span<const int>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::NotSupported);
}

TEST_CASE("Compile-time mutable span take updates POD queue without pop") {
    PathSpace space;
    CHECK(space.insert("/ints", 7).errors.empty());
    CHECK(space.insert("/ints", 8).errors.empty());

    auto ret = space.take<"/ints">([&](std::span<int> ints) {
        REQUIRE(ints.size() == 2);
        ints[0] = 70;
        ints[1] = 80;
    });
    REQUIRE(ret.has_value());

    auto first = space.take<int>("/ints");
    REQUIRE(first.has_value());
    CHECK(first.value() == 70);
    auto second = space.take<int>("/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 80);
}

TEST_CASE("Concurrent POD inserts remain visible in span read") {
    PathSpace space;
    constexpr int perThread = 100;
    std::thread t1([&]() {
        for (int i = 0; i < perThread; ++i) {
            space.insert("/ints", i);
        }
    });
    std::thread t2([&]() {
        for (int i = perThread; i < perThread * 2; ++i) {
            space.insert("/ints", i);
        }
    });
    t1.join();
    t2.join();

    std::vector<int> observed;
    auto ret = space.read("/ints", [&](std::span<const int> ints) { observed.assign(ints.begin(), ints.end()); });
    REQUIRE(ret.has_value());
    CHECK(observed.size() == static_cast<std::size_t>(perThread * 2));
    // Ensure all inserted values are present (order may interleave, but count and set should match).
    std::vector<int> sorted = observed;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < perThread * 2; ++i) {
        CHECK(std::binary_search(sorted.begin(), sorted.end(), i));
    }
}

TEST_CASE("Mutable span take can update POD queue without popping") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).errors.empty());
    CHECK(space.insert("/ints", 2).errors.empty());
    CHECK(space.insert("/ints", 3).errors.empty());

    auto ret = space.take("/ints", [&](std::span<int> ints) {
        REQUIRE(ints.size() == 3);
        ints[1] = 42;
        ints[2] = 99;
    });
    REQUIRE(ret.has_value());

    // Queue reflects the mutated values while preserving FIFO (span take does not pop).
    auto t1 = space.take<int>("/ints");
    REQUIRE(t1.has_value());
    CHECK(t1.value() == 1);
    auto t2 = space.take<int>("/ints");
    REQUIRE(t2.has_value());
    CHECK(t2.value() == 42);
    auto t3 = space.take<int>("/ints");
    REQUIRE(t3.has_value());
    CHECK(t3.value() == 99);
}

TEST_CASE("Visit reports queue depth for POD nodes") {
    PathSpace space;
    CHECK(space.insert("/ints", 10).errors.empty());
    CHECK(space.insert("/ints", 20).errors.empty());
    CHECK(space.insert("/ints", 30).errors.empty());

    std::optional<std::size_t> depth;
    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/ints") {
                depth = handle.queueDepth();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(ok);
    REQUIRE(depth.has_value());
    CHECK(*depth == 3);
}

TEST_CASE("Visit can read bool POD value") {
    PathSpace space;
    CHECK(space.insert("/flag", true).errors.empty());

    bool seen = false;
    auto ok = space.visit([&](PathEntry const& entry, ValueHandle& handle) {
        if (entry.path == "/flag") {
            auto value = handle.read<bool>();
            REQUIRE(value.has_value());
            CHECK(*value);
            seen = true;
            return VisitControl::Stop;
        }
        return VisitControl::Continue;
    });
    REQUIRE(ok);
    CHECK(seen);
}

TEST_CASE("POD bool payload survives clone copy") {
    PathSpace space;
    CHECK(space.insert("/flags", true).errors.empty());
    CHECK(space.insert("/flags", false).errors.empty());

    auto clone = space.clone();

    auto first = clone.take<bool>("/flags");
    REQUIRE(first.has_value());
    CHECK(*first);

    auto second = clone.take<bool>("/flags");
    REQUIRE(second.has_value());
    CHECK_FALSE(*second);
}

TEST_CASE("User POD struct uses fast path for queue and span") {
    PathSpace space;
    Vec2 a{1.0f, 2.0f};
    Vec2 b{3.0f, 4.0f};
    CHECK(space.insert("/vec", a).errors.empty());
    CHECK(space.insert("/vec", b).errors.empty());

    std::vector<Vec2> observed;
    auto span = space.read("/vec", [&](std::span<const Vec2> vals) {
        observed.assign(vals.begin(), vals.end());
    });
    REQUIRE(span.has_value());
    REQUIRE(observed.size() == 2);
    CHECK(observed[0].x == doctest::Approx(1.0f));
    CHECK(observed[1].y == doctest::Approx(4.0f));

    auto first = space.take<Vec2>("/vec");
    REQUIRE(first.has_value());
    CHECK(first->x == doctest::Approx(1.0f));
    CHECK(first->y == doctest::Approx(2.0f));
    auto second = space.take<Vec2>("/vec");
    REQUIRE(second.has_value());
    CHECK(second->x == doctest::Approx(3.0f));
    CHECK(second->y == doctest::Approx(4.0f));
}

TEST_CASE("User POD span mutable edits without popping") {
    PathSpace space;
    Vec2 a{10.0f, 20.0f};
    Vec2 b{30.0f, 40.0f};
    CHECK(space.insert("/vecm", a).errors.empty());
    CHECK(space.insert("/vecm", b).errors.empty());

    auto ret = space.take("/vecm", [&](std::span<Vec2> vals) {
        REQUIRE(vals.size() == 2);
        vals[0].x = 11.0f;
        vals[1].y = 44.0f;
    });
    REQUIRE(ret.has_value());

    auto first = space.take<Vec2>("/vecm");
    REQUIRE(first.has_value());
    CHECK(first->x == doctest::Approx(11.0f));
    CHECK(first->y == doctest::Approx(20.0f));
    auto second = space.take<Vec2>("/vecm");
    REQUIRE(second.has_value());
    CHECK(second->x == doctest::Approx(30.0f));
    CHECK(second->y == doctest::Approx(44.0f));
}

TEST_CASE("User POD mixed types trigger upgrade and disable span") {
    PathSpace space;
    Vec2 v2{5.0f, 6.0f};
    Vec3 v3{7, 8, 9};
    CHECK(space.insert("/mixstruct", v2).errors.empty());
    CHECK(space.insert("/mixstruct", v3).errors.empty()); // should upgrade to generic

    auto span = space.read("/mixstruct", [&](std::span<const Vec2>) {});
    CHECK_FALSE(span.has_value());
    CHECK(span.error().code == Error::Code::NotSupported);

    auto first = space.take<Vec2>("/mixstruct");
    REQUIRE(first.has_value());
    CHECK(first->x == doctest::Approx(5.0f));
    CHECK(first->y == doctest::Approx(6.0f));
    auto second = space.take<Vec3>("/mixstruct");
    REQUIRE(second.has_value());
    CHECK(second->x == 7);
    CHECK(second->y == 8);
    CHECK(second->z == 9);
}

TEST_CASE("Concurrent mutable span and take preserve count") {
    PathSpace space;
    for (int i = 1; i <= 8; ++i) {
        CHECK(space.insert("/concurrent", i).errors.empty());
    }

    std::atomic<bool> spanStarted{false};
    std::atomic<bool> spanDone{false};
    std::vector<int> taken;

    std::thread mutator([&]() {
        auto ret = space.take("/concurrent", [&](std::span<int> ints) {
            spanStarted.store(true, std::memory_order_release);
            if (!ints.empty()) {
                ints[0] = 999;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        spanDone.store(true, std::memory_order_release);
        REQUIRE(ret.has_value());
    });

    std::thread taker([&]() {
        while (!spanStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!spanDone.load(std::memory_order_acquire) || taken.size() < 8) {
            auto v = space.take<int>("/concurrent");
            if (v.has_value()) {
                taken.push_back(*v);
            } else {
                std::this_thread::yield();
            }
        }
    });

    mutator.join();
    taker.join();

    REQUIRE(taken.size() == 8);
    CHECK(std::count(taken.begin(), taken.end(), 999) <= 1);
    for (int expected = 1; expected <= 8; ++expected) {
        if (std::count(taken.begin(), taken.end(), 999) == 1 && expected == 1) {
            continue;
        }
        CHECK(std::find(taken.begin(), taken.end(), expected) != taken.end());
    }
}

TEST_CASE("Snapshot after POD upgrade reports mixed queue") {
    PathSpace space;
    CHECK(space.insert("/snap", 7).errors.empty());
    CHECK(space.insert("/snap", 8).errors.empty());
    CHECK(space.insert("/snap", std::string("text")).errors.empty());

    std::optional<ValueSnapshot> snap;
    auto ok = space.visit([&](PathEntry const& entry, ValueHandle& handle) {
        if (entry.path == "/snap") {
            snap = handle.snapshot().value();
            return VisitControl::Stop;
        }
        return VisitControl::Continue;
    });
    REQUIRE(ok);
    REQUIRE(snap.has_value());
    CHECK(snap->queueDepth >= 2);
    REQUIRE(snap->types.size() >= 2);
    bool hasFund = false;
    bool hasOther = false;
    for (auto const& t : snap->types) {
        if (t.category == DataCategory::Fundamental) hasFund = true;
        else hasOther = true;
    }
    CHECK(hasFund);
    CHECK(hasOther);
}

TEST_CASE("PathAlias forwards POD span read and mutable take") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/root"};

    CHECK(alias.insert("/ints", 1).errors.empty());
    CHECK(alias.insert("/ints", 2).errors.empty());

    std::vector<int> seen;
    auto span = alias.read("/ints", [&](std::span<const int> ints) {
        seen.assign(ints.begin(), ints.end());
    });
    REQUIRE(span.has_value());
    CHECK(seen == std::vector<int>({1, 2}));

    auto mut = alias.take("/ints", [&](std::span<int> ints) {
        REQUIRE(ints.size() == 2);
        ints[0] = 10;
    });
    REQUIRE(mut.has_value());

    auto first = alias.take<int>("/ints");
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);
    auto second = upstream->take<int>("/root/ints");
    REQUIRE(second.has_value());
    CHECK(second.value() == 2);
}

TEST_CASE("PathAlias forwards pack insert and mutable span pack (minimal slicing)") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/root"};

    // Seed pre-existing values so minimal mode should slice to the latest batch.
    REQUIRE(alias.insert("/ints/x", 1).errors.empty());
    REQUIRE(alias.insert("/ints/y", 2).errors.empty());

    // Pack insert an aligned batch through the alias.
    auto ins = alias.insert<"/ints/x","/ints/y">(3, 4);
    REQUIRE(ins.errors.empty());

    // Minimal take should surface only the newly inserted batch and allow mutation.
    auto take = alias.take<"x","y">("/ints",
                                    [&](std::span<int> xs, std::span<int> ys) {
                                        REQUIRE(xs.size() == 1);
                                        REQUIRE(ys.size() == 1);
                                        CHECK(xs[0] == 3);
                                        CHECK(ys[0] == 4);
                                        xs[0] += 10;
                                        ys[0] += 20;
                                    },
                                    Out{} & Minimal{});
    if (!take) {
        CAPTURE(take.error().code);
        if (take.error().message) CAPTURE(*take.error().message);
    }
    REQUIRE(take.has_value());

    // Original seeds remain at the head; mutated batch follows.
    auto x1 = alias.take<int>("/ints/x");
    auto y1 = alias.take<int>("/ints/y");
    REQUIRE(x1.has_value());
    REQUIRE(y1.has_value());
    CHECK(x1.value() == 1);
    CHECK(y1.value() == 2);

    auto x2 = alias.take<int>("/ints/x");
    auto y2 = alias.take<int>("/ints/y");
    REQUIRE(x2.has_value());
    REQUIRE(y2.has_value());
    CHECK(x2.value() == 13);
    CHECK(y2.value() == 24);
}

TEST_CASE("PathAlias forwards pack insert and span pack read") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/root"};

    auto ins = alias.insert<"/ints/x","/ints/y">(5, 6);
    REQUIRE(ins.errors.empty());

    auto span = alias.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 1);
        REQUIRE(ys.size() == 1);
        CHECK(xs[0] == 5);
        CHECK(ys[0] == 6);
    });
    if (!span) {
        CAPTURE(span.error().code);
        if (span.error().message) CAPTURE(*span.error().message);
    }
    REQUIRE(span.has_value());
}

TEST_CASE("PathSpaceTrellis forwards POD span read and rejects root span") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    CHECK(trellis.insert("/pod", 4).errors.empty());
    CHECK(trellis.insert("/pod", 5).errors.empty());

    std::vector<int> seen;
    auto span = trellis.read("/pod", [&](std::span<const int> ints) { seen.assign(ints.begin(), ints.end()); });
    REQUIRE(span.has_value());
    CHECK(seen == std::vector<int>({4, 5}));

    auto mut = trellis.take("/pod", [&](std::span<int> ints) {
        REQUIRE(ints.size() == 2);
        ints[1] = 50;
    });
    REQUIRE(mut.has_value());

    auto first = trellis.take<int>("/pod");
    REQUIRE(first.has_value());
    CHECK(first.value() == 4);
    auto second = trellis.take<int>("/pod");
    REQUIRE(second.has_value());
    CHECK(second.value() == 50);

    auto rootSpan = trellis.read("/", [&](std::span<const int>) {});
    CHECK_FALSE(rootSpan.has_value());
    CHECK(rootSpan.error().code == Error::Code::NotSupported);
}

TEST_CASE("Span glob and indexed paths rejected via alias and trellis") {
    auto backing = std::make_shared<PathSpace>();
    PathAlias alias{backing, "/root"};
    PathSpaceTrellis trellis{backing};
    CHECK(alias.insert("/vals", 1).errors.empty());

    auto aliasGlob = alias.read("/vals/*", [&](std::span<const int>) {});
    CHECK_FALSE(aliasGlob.has_value());
    CHECK(aliasGlob.error().code == Error::Code::InvalidPath);

    auto aliasIndexed = alias.take("/vals[0]", [&](std::span<int>) {});
    CHECK_FALSE(aliasIndexed.has_value());
    CHECK(aliasIndexed.error().code == Error::Code::InvalidPath);

    CHECK(trellis.insert("/vals", 2).errors.empty());
    auto trellisGlob = trellis.read("/vals/*", [&](std::span<const int>) {});
    CHECK_FALSE(trellisGlob.has_value());
    CHECK(trellisGlob.error().code == Error::Code::InvalidPath);

    auto trellisIndexed = trellis.take("/vals[0]", [&](std::span<int>) {});
    CHECK_FALSE(trellisIndexed.has_value());
    CHECK(trellisIndexed.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Span pack take pops when callback returns true") {
    PathSpace space;
    auto ins = space.insert<"/pair/a","/pair/b">(1, 2);
    REQUIRE(ins.errors.empty());

    auto take = space.take<"a","b">("/pair",
                                    [&](std::span<int> a, std::span<int> b) {
                                        REQUIRE(a.size() == 1);
                                        REQUIRE(b.size() == 1);
                                        CHECK(a[0] == 1);
                                        CHECK(b[0] == 2);
                                        return true; // request pop
                                    });
    if (!take) {
        CAPTURE(take.error().code);
        if (take.error().message) CAPTURE(*take.error().message);
    }
    REQUIRE(take.has_value());

    auto next = space.take<int>("/pair/a");
    CHECK_FALSE(next.has_value());
    CHECK(next.error().code == Error::Code::NoObjectFound);
}

TEST_CASE("Span pack take keeps data when callback returns void/false") {
    PathSpace space;
    auto ins = space.insert<"/keep/a","/keep/b">(3, 4);
    REQUIRE(ins.errors.empty());

    auto takeVoid = space.take<"a","b">("/keep",
                                        [&](std::span<int> a, std::span<int> b) {
                                            CHECK(a.size() == 1);
                                            CHECK(b.size() == 1);
                                            a[0] = 30;
                                            b[0] = 40;
                                        });
    REQUIRE(takeVoid.has_value());

    auto a = space.take<int>("/keep/a");
    auto b = space.take<int>("/keep/b");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a.value() == 30);
    CHECK(b.value() == 40);

    auto takeFalse = space.insert<"/keep/a","/keep/b">(5, 6);
    REQUIRE(takeFalse.errors.empty());
    auto noop = space.take<"a","b">("/keep",
                                    [&](std::span<int> aSpan, std::span<int> bSpan) {
                                        CHECK(aSpan[0] == 5);
                                        CHECK(bSpan[0] == 6);
                                        return false; // explicit no-pop
                                    });
    REQUIRE(noop.has_value());
    auto a2 = space.take<int>("/keep/a");
    auto b2 = space.take<int>("/keep/b");
    REQUIRE(a2.has_value());
    REQUIRE(b2.has_value());
    CHECK(a2.value() == 5);
    CHECK(b2.value() == 6);
}

TEST_CASE("Span pack take blocks until data available") {
    PathSpace space;
    auto start = std::chrono::steady_clock::now();

    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto ins = space.insert<"/block/a","/block/b">(7, 8);
        REQUIRE(ins.errors.empty());
    });

    auto res = space.take<"a","b">("/block",
                                   [&](std::span<int> a, std::span<int> b) {
                                       CHECK(a.size() == 1);
                                       CHECK(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Block{std::chrono::milliseconds(200)});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (!res) {
        CAPTURE(res.error().code);
        if (res.error().message) CAPTURE(*res.error().message);
    }
    REQUIRE(res.has_value());
    CHECK(elapsed.count() >= 40); // verify it actually waited

    producer.join();
}

TEST_CASE("Span pack take pops without Pop flag when callback returns true") {
    PathSpace space;
    auto ins = space.insert<"/nopop/a","/nopop/b">(11, 22);
    REQUIRE(ins.errors.empty());

    auto res = space.take<"a","b">("/nopop",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true; // pop even without Pop{}
                                   });
    REQUIRE(res.has_value());

    auto a = space.take<int>("/nopop/a");
    auto b = space.take<int>("/nopop/b");
    CHECK_FALSE(a.has_value());
    CHECK_FALSE(b.has_value());
}

TEST_CASE("Span pack take blocks until paths materialize") {
    PathSpace space;
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto ins = space.insert<"/appear/a","/appear/b">(9, 10);
        REQUIRE(ins.errors.empty());
    });

    auto res = space.take<"a","b">("/appear",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Block{std::chrono::milliseconds(200)});
    if (!res) {
        CAPTURE(res.error().code);
        if (res.error().message) CAPTURE(*res.error().message);
    }
    REQUIRE(res.has_value());
    producer.join();
}

TEST_CASE("Span pack pop rejected on minimal misaligned window") {
    PathSpace space;
    REQUIRE(space.insert("/mis/x", 1).errors.empty());
    REQUIRE(space.insert("/mis/y", 2).errors.empty());
    // New pack begins after existing head, so minimal window will start past head.
    REQUIRE(space.insert<"/mis/x","/mis/y">(3, 4).errors.empty());

    auto take = space.take<"x","y">("/mis",
                                    [&](std::span<int>, std::span<int>) {
                                        return true; // request pop
                                    },
                                    Out{} & Minimal{});
    CHECK_FALSE(take.has_value());
    CHECK(take.error().code == Error::Code::InvalidType);
}

TEST_CASE("Visit snapshot reports user POD depth and category") {
    PathSpace space;
    CHECK(space.insert("/vecsnap", Vec2{1.0f, 2.0f}).errors.empty());
    CHECK(space.insert("/vecsnap", Vec2{3.0f, 4.0f}).errors.empty());

    std::optional<ValueSnapshot> snap;
    auto ok = space.visit([&](PathEntry const& entry, ValueHandle& handle) {
        if (entry.path == "/vecsnap") {
            snap = handle.snapshot().value();
            return VisitControl::Stop;
        }
        return VisitControl::Continue;
    });
    REQUIRE(ok);
    REQUIRE(snap.has_value());
    CHECK(snap->queueDepth == 2);
    REQUIRE_FALSE(snap->types.empty());
    CHECK(snap->types.front().category == DataCategory::SerializationLibraryCompatible);
}

TEST_CASE("User POD upgrades on mismatch while preserving order") {
    PathSpace space;
    Vec2 v{5.0f, 6.0f};
    CHECK(space.insert("/mixed_vec", v).errors.empty());
    CHECK(space.insert("/mixed_vec", std::string("later")).errors.empty());

    auto first = space.take<Vec2>("/mixed_vec");
    REQUIRE(first.has_value());
    CHECK(first->x == doctest::Approx(5.0f));
    CHECK(first->y == doctest::Approx(6.0f));

    auto second = space.take<std::string>("/mixed_vec");
    REQUIRE(second.has_value());
    CHECK(second == "later");
}

TEST_CASE("Concurrent POD insert/take retains every value") {
    PathSpace space;
    constexpr int kTotal = 20000;
    std::barrier start(2);

    std::vector<int> consumed;
    consumed.reserve(kTotal);

    std::thread producer([&]() {
        start.arrive_and_wait();
        for (int i = 1; i <= kTotal; ++i) {
            auto ret = space.insert("/ints", i);
            REQUIRE(ret.errors.empty());
        }
    });

    std::thread consumer([&]() {
        start.arrive_and_wait();
        while (static_cast<int>(consumed.size()) < kTotal) {
            auto val = space.take<int>("/ints");
            if (val.has_value()) {
                consumed.push_back(*val);
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(static_cast<int>(consumed.size()) == kTotal);
    std::sort(consumed.begin(), consumed.end());
    for (int i = 1; i <= kTotal; ++i) {
        CHECK(consumed[i - 1] == i);
    }
}

TEST_CASE("Span pack read returns aligned spans") {
    PathSpace space;
    std::vector<float> xs{1.f, 2.f, 3.f};
    std::vector<float> ys{4.f, 5.f, 6.f};
    std::vector<float> zs{7.f, 8.f, 9.f};
    for (std::size_t i = 0; i < xs.size(); ++i) {
        CHECK(space.insert("/ints/values/x", xs[i]).errors.empty());
        CHECK(space.insert("/ints/values/y", ys[i]).errors.empty());
        CHECK(space.insert("/ints/values/z", zs[i]).errors.empty());
    }
    auto ret = space.read<"x","y","z">("/ints/values", [&](std::span<const float> x,
                                                           std::span<const float> y,
                                                           std::span<const float> z) {
        CHECK(x.size() == xs.size());
        CHECK(y.size() == ys.size());
        CHECK(z.size() == zs.size());
        for (std::size_t i = 0; i < xs.size(); ++i) {
            CHECK(x[i] == doctest::Approx(xs[i]));
            CHECK(y[i] == doctest::Approx(ys[i]));
            CHECK(z[i] == doctest::Approx(zs[i]));
        }
    });
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
        std::cerr << "span_pack_read_error=" << static_cast<int>(ret.error().code) << std::endl;
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack read keeps buffer alive across concurrent growth") {
    PathSpace space;
    // Seed small aligned lanes.
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(space.insert("/ints/a", i).errors.empty());
        REQUIRE(space.insert("/ints/b", i * 10).errors.empty());
    }

    std::barrier start(2);
    std::atomic<bool> callbackEntered{false};

    // Grower will force PodPayload reallocation after the span snapshot is taken.
    std::thread grower([&]() {
        start.arrive_and_wait();
        for (int i = 5; i < 1200; ++i) { // exceed initial capacity to trigger ensureCapacity
            auto ra = space.insert("/ints/a", i);
            auto rb = space.insert("/ints/b", i * 10);
            REQUIRE(ra.errors.empty());
            REQUIRE(rb.errors.empty());
        }
    });

    auto ret = space.read<"a","b">("/ints", [&](std::span<const int> a, std::span<const int> b) {
        callbackEntered.store(true, std::memory_order_release);
        start.arrive_and_wait(); // let grower run while spans are held
        REQUIRE(a.size() == 4);
        REQUIRE(b.size() == 4);
        CHECK(a[0] == 1);
        CHECK(a[3] == 4);
        CHECK(b[0] == 10);
        CHECK(b[3] == 40);
    });

    grower.join();
    REQUIRE(callbackEntered.load(std::memory_order_acquire));
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack read rejects length mismatch") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/x", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 10.f).errors.empty());
    CHECK(space.insert("/ints/values/z", 20.f).errors.empty());
    CHECK(space.insert("/ints/values/z", 30.f).errors.empty());

    auto ret = space.read<"x","y","z">("/ints/values", [&](std::span<const float>, std::span<const float>, std::span<const float>) {});
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Span pack mutable take edits in place") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/x", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 3.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 4.f).errors.empty());

    auto mut = space.take<"x","y">("/ints/values", [&](std::span<float> x, std::span<float> y) {
        REQUIRE(x.size() == y.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += 10.f;
            y[i] += 20.f;
        }
    });
    if (!mut) {
        CAPTURE(mut.error().code);
        if (mut.error().message) CAPTURE(*mut.error().message);
    }
    REQUIRE(mut.has_value());

    auto check = space.read<"x","y">("/ints/values", [&](std::span<const float> x, std::span<const float> y) {
        REQUIRE(x.size() == 2);
        CHECK(x[0] == doctest::Approx(11.f));
        CHECK(x[1] == doctest::Approx(12.f));
        CHECK(y[0] == doctest::Approx(23.f));
        CHECK(y[1] == doctest::Approx(24.f));
    });
    REQUIRE(check.has_value());
}

TEST_CASE("Span pack read supports empty queues") {
    PathSpace space;
    // Create empty POD queues by inserting then consuming
    CHECK(space.insert("/ints/values/x", 0.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 0.f).errors.empty());
    CHECK(space.take<float>("/ints/values/x").has_value());
    CHECK(space.take<float>("/ints/values/y").has_value());

    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float> x, std::span<const float> y) {
        CHECK(x.empty());
        CHECK(y.empty());
    });
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack rejects mixed POD types") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 5).errors.empty());
    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Span pack mutable take length mismatch keeps data unchanged") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 3.f).errors.empty());

    auto mut = space.take<"x","y">("/ints/values", [&](std::span<float> x, std::span<float> y) {
        (void)x;
        (void)y;
    });
    CHECK_FALSE(mut.has_value());
    CHECK(mut.error().code == Error::Code::InvalidType);

    auto xFront = space.read<float>("/ints/values/x");
    REQUIRE(xFront.has_value());
    CHECK(xFront.value() == doctest::Approx(1.f));
    auto yVals = space.read<"y">("/ints/values", [&](std::span<const float> y) {
        REQUIRE(y.size() == 2);
        CHECK(y[0] == doctest::Approx(2.f));
        CHECK(y[1] == doctest::Approx(3.f));
    });
    REQUIRE(yVals.has_value());
}

TEST_CASE("Span pack read rejects blocking; take allows optional pop") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());
    auto block = space.read<"x","y">("/ints/values",
                                     [&](std::span<const float>, std::span<const float>) {},
                                     Out{} & Block{std::chrono::milliseconds(5)});
    CHECK_FALSE(block.has_value());
    CHECK(block.error().code == Error::Code::NotSupported);

    auto pop = space.take<"x","y">("/ints/values",
                                   [&](std::span<float> xs, std::span<float> ys) {
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       return true;
                                   },
                                   Out{} & Pop{});
    REQUIRE(pop.has_value());
    auto x = space.take<float>("/ints/values/x");
    auto y = space.take<float>("/ints/values/y");
    CHECK_FALSE(x.has_value());
    CHECK_FALSE(y.has_value());
}

TEST_CASE("Span pack rejects glob and indexed paths") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", 1.f).errors.empty());
    CHECK(space.insert("/ints/values/y", 2.f).errors.empty());

    auto glob = space.read<"x","y">("/ints/*", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(glob.has_value());
    CHECK(glob.error().code == Error::Code::InvalidPath);

    auto indexed = space.read<"x","y">("/ints/values[0]", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(indexed.has_value());
    CHECK(indexed.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Span pack fails fast on non-POD payloads") {
    PathSpace space;
    CHECK(space.insert("/ints/values/x", std::string("hello")).errors.empty());
    CHECK(space.insert("/ints/values/y", std::string("world")).errors.empty());
    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const float>, std::span<const float>) {});
    CHECK_FALSE(ret.has_value());
    CHECK((ret.error().code == Error::Code::NotSupported || ret.error().code == Error::Code::InvalidType));
}

TEST_CASE("Span pack handles larger arity") {
    PathSpace space;
    for (int i = 0; i < 3; ++i) {
        CHECK(space.insert("/ints/values/a", i).errors.empty());
        CHECK(space.insert("/ints/values/b", i + 10).errors.empty());
        CHECK(space.insert("/ints/values/c", i + 20).errors.empty());
        CHECK(space.insert("/ints/values/d", i + 30).errors.empty());
    }
    auto ret = space.read<"a","b","c","d">("/ints/values",
                                           [&](std::span<const int> a,
                                               std::span<const int> b,
                                               std::span<const int> c,
                                               std::span<const int> d) {
                                               REQUIRE(a.size() == 3);
                                               for (int i = 0; i < 3; ++i) {
                                                   CHECK(a[i] == i);
                                                   CHECK(b[i] == i + 10);
                                                   CHECK(c[i] == i + 20);
                                               CHECK(d[i] == i + 30);
                                           }
                                           });
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack keeps POD buffers alive during callback") {
    PathSpace space;
    constexpr std::size_t kFill = 2048;
    for (std::size_t i = 0; i < kFill; ++i) {
        CHECK(space.insert("/ints/values/x", static_cast<int>(i)).errors.empty());
        CHECK(space.insert("/ints/values/y", static_cast<int>(1000 + i)).errors.empty());
    }

    auto ret = space.read<"x","y">("/ints/values", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == kFill);
        REQUIRE(ys.size() == kFill);

        // Force both payloads to resize and free their original buffers.
        CHECK(space.insert("/ints/values/x", 999999).errors.empty());
        CHECK(space.insert("/ints/values/y", 888888).errors.empty());

        // Allocate similarly sized buffers to encourage reuse of the freed memory.
        std::vector<int> clobberA(kFill, 42);
        std::vector<int> clobberB(kFill, 84);
        (void)clobberA;
        (void)clobberB;

        for (std::size_t i = 0; i < kFill; ++i) {
            CHECK(xs[i] == static_cast<int>(i));
            CHECK(ys[i] == static_cast<int>(1000 + i));
        }
    });

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack traits reject arity mismatch at compile time") {
    using BadFn = decltype([](std::span<const float> x, std::span<const float> y, int) {});
    static_assert(!SP::Detail::SpanPackTraits<BadFn>::isSpanPack);
}

TEST_CASE("Pack insert enqueues all paths atomically") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 1).errors.empty());
    CHECK(space.insert("/ints/y", 1).errors.empty());

    std::atomic<bool> mismatch{false};
    std::thread reader([&]() {
        for (int i = 0; i < 256; ++i) {
            auto ret = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
                if (xs.size() != ys.size()) {
                    mismatch.store(true, std::memory_order_release);
                }
            });
            REQUIRE(ret.has_value());
        }
    });

    std::thread writer([&]() {
        auto ret = space.insert<"/ints/x","/ints/y">(2, 3);
        CHECK(ret.errors.empty());
    });

    writer.join();
    reader.join();

    CHECK_FALSE(mismatch.load(std::memory_order_acquire));

    auto final = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 2);
        REQUIRE(ys.size() == 2);
        CHECK(xs.back() == 2);
        CHECK(ys.back() == 3);
    });
    REQUIRE(final.has_value());
}

TEST_CASE("Pack insert concurrent writers preserves alignment") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 0).errors.empty());
    CHECK(space.insert("/ints/y", 0).errors.empty());
    constexpr int kThreads    = 4;
    constexpr int kPerThread  = 200;
    std::atomic<int> next{1};

    auto worker = [&]() {
        for (int i = 0; i < kPerThread; ++i) {
            int v = next.fetch_add(1, std::memory_order_relaxed);
            auto ret = space.insert<"/ints/x","/ints/y">(v, v);
            REQUIRE(ret.errors.empty());
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    auto check = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == ys.size());
        REQUIRE(xs.size() == static_cast<std::size_t>(kThreads * kPerThread + 1)); // +1 seed per lane above
        std::unordered_set<int> seen;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            CHECK(xs[i] == ys[i]);
            seen.insert(xs[i]);
        }
        CHECK(seen.size() == xs.size());
    });
    REQUIRE(check.has_value());
}

TEST_CASE("Pack insert concurrent readers never see skew") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 0).errors.empty());
    CHECK(space.insert("/ints/y", 0).errors.empty());
    std::atomic<bool> stop{false};
    std::atomic<bool> skew{false};

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto res = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
                if (xs.size() != ys.size()) {
                    skew.store(true, std::memory_order_release);
                }
            });
            if (!res.has_value()) {
                std::this_thread::yield();
            }
        }
    });

    constexpr int kThreads    = 3;
    constexpr int kPerThread  = 150;
    std::atomic<int> next{10000};
    auto worker = [&]() {
        for (int i = 0; i < kPerThread; ++i) {
            int v = next.fetch_add(1, std::memory_order_relaxed);
            auto ret = space.insert<"/ints/x","/ints/y">(v, v);
            REQUIRE(ret.errors.empty());
        }
    };
    std::vector<std::thread> writers;
    for (int i = 0; i < kThreads; ++i) writers.emplace_back(worker);
    for (auto& t : writers) t.join();

    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK_FALSE(skew.load(std::memory_order_acquire));
}

TEST_CASE("Pack insert concurrent take keeps lanes aligned") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 0).errors.empty());
    CHECK(space.insert("/ints/y", 0).errors.empty());

    constexpr int kWrites = 400;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> skew{false};

    std::thread writer([&]() {
        for (int i = 0; i < kWrites; ++i) {
            auto ret = space.insert<"/ints/x","/ints/y">(i, i);
            REQUIRE(ret.errors.empty());
            produced.fetch_add(1, std::memory_order_release);
        }
    });

    std::thread taker([&]() {
        int backoff = 0;
        while (consumed.load(std::memory_order_acquire) < kWrites) {
            auto ret = space.take<"x","y">("/ints", [&](std::span<int> xs, std::span<int> ys) {
                if (xs.size() != ys.size()) {
                    skew.store(true, std::memory_order_release);
                    return;
                }
                for (std::size_t i = 0; i < xs.size(); ++i) {
                    CHECK(xs[i] == ys[i]);
                }
                consumed.fetch_add(static_cast<int>(xs.size()), std::memory_order_release);
            },
                                        Out{} & Minimal{});
            if (!ret.has_value()) {
                std::this_thread::yield();
                if (++backoff % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    writer.join();
    taker.join();

    CHECK_FALSE(skew.load(std::memory_order_acquire));
    CHECK(consumed.load(std::memory_order_acquire) == kWrites);
    auto final = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        CHECK(xs.size() == ys.size());
    });
    REQUIRE(final.has_value());
}

TEST_CASE("Pack insert span take returns full queue without minimal slicing") {
    PathSpace space;

    auto ins1 = space.insert<"/ints/x","/ints/y">(1, 10);
    REQUIRE(ins1.errors.empty());
    auto ins2 = space.insert<"/ints/x","/ints/y">(2, 20);
    REQUIRE(ins2.errors.empty());
    auto ins3 = space.insert<"/ints/x","/ints/y">(3, 30);
    REQUIRE(ins3.errors.empty());

    auto ret = space.take<"x","y">("/ints", [&](std::span<int> xs, std::span<int> ys) {
        REQUIRE(xs.size() == 3);
        REQUIRE(ys.size() == 3);
        CHECK(xs[0] == 1);
        CHECK(xs[1] == 2);
        CHECK(xs[2] == 3);
        CHECK(ys[0] == 10);
        CHECK(ys[1] == 20);
        CHECK(ys[2] == 30);
    });

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Pack insert span take minimal slices to newest batch only") {
    PathSpace space;
    // Seed existing entries so minimal mode should skip them.
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/y", 2).errors.empty());

    // Insert a new aligned batch.
    auto ins = space.insert<"/ints/x","/ints/y">(3, 4);
    REQUIRE(ins.errors.empty());

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int> xs, std::span<int> ys) {
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       CHECK(xs[0] == 3);
                                       CHECK(ys[0] == 4);
                                   },
                                   Out{} & Minimal{});

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
}

TEST_CASE("Span pack minimal take falls back to head when no pack markers exist") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 10).errors.empty());
    REQUIRE(space.insert("/ints/y", 20).errors.empty());

    bool invoked = false;
    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int> xs, std::span<int> ys) {
                                       invoked = true;
                                       REQUIRE(xs.size() == 1);
                                       REQUIRE(ys.size() == 1);
                                       CHECK(xs[0] == 10);
                                       CHECK(ys[0] == 20);
                                   },
                                   Out{} & Minimal{});

    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
    CHECK(invoked);
}

TEST_CASE("Minimal span take rejects drift after mixed history") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());
    REQUIRE(space.insert<"/ints/x","/ints/y">(2, 2).errors.empty());
    REQUIRE(space.insert("/ints/x", 999).errors.empty()); // drift lane x only

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int>, std::span<int>) {},
                                   Out{} & Minimal{});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);

    auto xSpan = space.read<"x">("/ints", [&](std::span<const int> xs) {
        REQUIRE(xs.size() == 3);
        CHECK(xs[0] == 1);
        CHECK(xs[1] == 2);
        CHECK(xs[2] == 999);
    });
    REQUIRE(xSpan.has_value());
    auto ySpan = space.read<"y">("/ints", [&](std::span<const int> ys) {
        REQUIRE(ys.size() == 2);
        CHECK(ys[0] == 1);
        CHECK(ys[1] == 2);
    });
    REQUIRE(ySpan.has_value());
}

TEST_CASE("Pack insert rollback clears partial reservations on failure") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* yNode = lookup(root, {"ints", "y"});
    REQUIRE(yNode != nullptr);
    REQUIRE(yNode->podPayload);
    // Force reservation failure on lane y
    CHECK(yNode->podPayload->freezeForUpgrade());

    auto ret = space.insert<"/ints/x","/ints/y">(2, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);

    auto spans = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 1);
        REQUIRE(ys.size() == 1);
        CHECK(xs[0] == 1); // lane x reservation rolled back
        CHECK(ys[0] == 1);
    });
    REQUIRE(spans.has_value());
}

TEST_CASE("Failed pack insert does not hide existing data in minimal take") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* yNode = lookup(root, {"ints", "y"});
    REQUIRE(yNode != nullptr);
    REQUIRE(yNode->podPayload);
    CHECK(yNode->podPayload->freezeForUpgrade());

    auto ret = space.insert<"/ints/x","/ints/y">(2, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);

    bool invoked = false;
    auto take = space.take<"x","y">("/ints",
                                    [&](std::span<int> xs, std::span<int> ys) {
                                        invoked = true;
                                        REQUIRE(xs.size() == 1);
                                        REQUIRE(ys.size() == 1);
                                        CHECK(xs[0] == 1);
                                        CHECK(ys[0] == 1);
                                    },
                                    Out{} & Minimal{});
    REQUIRE(take.has_value());
    CHECK(invoked);
}

TEST_CASE("Span pack read rejects block and pop options") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto block = space.read<"x","y">("/ints",
                                     [&](std::span<const int>, std::span<const int>) {},
                                     Out{} & Block{std::chrono::milliseconds{5}});
    CHECK_FALSE(block.has_value());
    CHECK(block.error().code == Error::Code::NotSupported);

    auto pop = space.read<"x","y">("/ints",
                                   [&](std::span<const int>, std::span<const int>) {},
                                   Out{} & Pop{});
    CHECK_FALSE(pop.has_value());
    CHECK(pop.error().code == Error::Code::NotSupported);
}

TEST_CASE("Span pack take supports block and optional pop flag") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto block = space.take<"x","y">("/ints",
                                     [&](std::span<int>, std::span<int>) {},
                                     Out{} & Block{std::chrono::milliseconds{5}});
    REQUIRE(block.has_value());

    auto pop = space.take<"x","y">("/ints",
                                   [&](std::span<int> a, std::span<int> b) {
                                       REQUIRE(a.size() == 1);
                                       REQUIRE(b.size() == 1);
                                       return true;
                                   },
                                   Out{} & Pop{});
    REQUIRE(pop.has_value());
    auto a = space.take<int>("/ints/x");
    auto b = space.take<int>("/ints/y");
    CHECK_FALSE(a.has_value());
    CHECK_FALSE(b.has_value());
}

TEST_CASE("Span pack read rejects glob and indexed base paths") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 2).errors.empty());

    auto glob = space.read<"x","y">("/ints/*", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(glob.has_value());
    CHECK(glob.error().code == Error::Code::InvalidPath);

    auto indexed = space.read<"x","y">("/ints[0]", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(indexed.has_value());
    CHECK(indexed.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Span pack read surfaces lane length mismatch") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/x", 2).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    auto ret = space.read<"x","y">("/ints", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);

    auto xs1 = space.take<int>("/ints/x");
    auto xs2 = space.take<int>("/ints/x");
    auto ys1 = space.take<int>("/ints/y");
    REQUIRE(xs1.has_value());
    REQUIRE(xs2.has_value());
    REQUIRE(ys1.has_value());
    CHECK(xs1.value() == 1);
    CHECK(xs2.value() == 2);
    CHECK(ys1.value() == 1);
}

TEST_CASE("Span pack read recovers after transient length mismatch") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/x", 2).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    auto first = space.read<"x","y">("/ints", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(first.has_value());
    CHECK(first.error().code == Error::Code::InvalidType);

    REQUIRE(space.insert("/ints/y", 2).errors.empty());

    auto second = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == 2);
        REQUIRE(ys.size() == 2);
        CHECK(xs[0] == 1);
        CHECK(xs[1] == 2);
        CHECK(ys[0] == 1);
        CHECK(ys[1] == 2);
    });
    if (!second) {
        CAPTURE(second.error().code);
        if (second.error().message) CAPTURE(*second.error().message);
    }
    REQUIRE(second.has_value());
}

TEST_CASE("Pack insert rejects glob paths") {
    PathSpace space;
    auto ret = space.insert<"/ints/*","/ints/y">(1, 2);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert notifies all affected paths") {
    PathSpace space;
    std::promise<Expected<int>> px;
    std::promise<Expected<int>> py;
    std::atomic<bool>           rx{false};
    std::atomic<bool>           ry{false};

    std::thread tx([&] {
        rx.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/x", Out{} & Block{std::chrono::milliseconds{200}});
        px.set_value(std::move(res));
    });
    std::thread ty([&] {
        ry.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/y", Out{} & Block{std::chrono::milliseconds{200}});
        py.set_value(std::move(res));
    });

    while (!rx.load(std::memory_order_acquire) || !ry.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto ret = space.insert<"/ints/x","/ints/y">(5, 6);
    REQUIRE(ret.errors.empty());

    auto fx = px.get_future();
    auto fy = py.get_future();
    REQUIRE(fx.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    REQUIRE(fy.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto vx = fx.get();
    auto vy = fy.get();
    REQUIRE(vx.has_value());
    REQUIRE(vy.has_value());
    CHECK(vx.value() == 5);
    CHECK(vy.value() == 6);

    tx.join();
    ty.join();
}

TEST_CASE("Concurrent minimal take waits gracefully for first pack marker") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1).errors.empty());
    REQUIRE(space.insert("/ints/y", 1).errors.empty());

    std::promise<Expected<void>> p;
    std::thread t([&] {
        auto ret = space.take<"x","y">("/ints",
                                       [&](std::span<int> xs, std::span<int> ys) {
                                           REQUIRE(xs.size() == ys.size());
                                           REQUIRE(!xs.empty());
                                           for (std::size_t i = 0; i < xs.size(); ++i) {
                                               CHECK(xs[i] == ys[i]);
                                           }
                                       },
                                       Out{} & Minimal{});
        p.set_value(std::move(ret));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto ins = space.insert<"/ints/x","/ints/y">(2, 2);
    REQUIRE(ins.errors.empty());

    auto fut    = p.get_future();
    auto status = fut.wait_for(std::chrono::seconds(1));
    REQUIRE(status == std::future_status::ready);
    auto ret = fut.get();
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    REQUIRE(ret.has_value());
    t.join();
}

TEST_CASE("Span pack mutable retries through transient length skew") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    Node* xNode = lookup(root, {"ints", "x"});
    Node* yNode = lookup(root, {"ints", "y"});
    REQUIRE(xNode != nullptr);
    REQUIRE(yNode != nullptr);
    auto px = xNode->podPayload;
    auto py = yNode->podPayload;
    REQUIRE(px);
    REQUIRE(py);

    auto rx = px->reserveOne();
    auto ry = py->reserveOne();
    REQUIRE(rx);
    REQUIRE(ry);

    *static_cast<int*>(rx->ptr) = 2;
    *static_cast<int*>(ry->ptr) = 2;

    std::barrier sync(2);
    std::promise<Expected<void>> resultPromise;

    std::thread reader([&] {
        sync.arrive_and_wait();
        auto ret = space.take<"x","y">("/ints",
                                       [&](std::span<int> xs, std::span<int> ys) {
                                           REQUIRE(xs.size() == ys.size());
                                           REQUIRE(xs.size() == 2);
                                           CHECK(xs[0] == 1);
                                           CHECK(ys[0] == 1);
                                           CHECK(xs[1] == 2);
                                           CHECK(ys[1] == 2);
                                       });
        resultPromise.set_value(std::move(ret));
    });

    // Publish lane x first to create a temporary skew that should be retried.
    px->publishOne(rx->index);
    sync.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::microseconds(150));
    py->publishOne(ry->index);

    auto fut = resultPromise.get_future();
    auto status = fut.wait_for(std::chrono::seconds(1));
    REQUIRE(status == std::future_status::ready);
    auto ret = fut.get();
    if (!ret) {
        CAPTURE(ret.error().code);
        if (ret.error().message) CAPTURE(*ret.error().message);
    }
    if (!ret.has_value()) {
        REQUIRE(ret.error().code == Error::Code::InvalidType);
        REQUIRE(ret.error().message == std::optional<std::string>{"Span lengths mismatch"});
        // Retry once more after skew should have healed.
        auto retry = space.take<"x","y">("/ints",
                                         [&](std::span<int> xs, std::span<int> ys) {
                                             REQUIRE(xs.size() == ys.size());
                                             REQUIRE(xs.size() == 2);
                                             CHECK(xs[0] == 1);
                                             CHECK(ys[0] == 1);
                                             CHECK(xs[1] == 2);
                                             CHECK(ys[1] == 2);
                                         });
        if (!retry) {
            CAPTURE(retry.error().code);
            if (retry.error().message) CAPTURE(*retry.error().message);
        }
        REQUIRE(retry.has_value());
    } else {
        REQUIRE(ret.has_value());
    }

    reader.join();
}

TEST_CASE("Pack insert notifies only touched lanes, not nested spaces") {
    auto context = std::make_shared<PathSpaceContext>();
    auto sink    = std::make_shared<RecordingSink>();
    context->setSink(sink);

    PathSpace parent{context};
    auto child = std::make_unique<PathSpace>(context, "/nested");
    REQUIRE(parent.insert("/nested", std::move(child)).errors.empty());

    {
        std::lock_guard<std::mutex> lg(sink->mutex);
        sink->paths.clear();
    }

    auto ret = parent.insert<"/ints/x","/ints/y">(5, 6);
    REQUIRE(ret.errors.empty());

    std::vector<std::string> pathsCopy;
    {
        std::lock_guard<std::mutex> lg(sink->mutex);
        pathsCopy = sink->paths;
    }
    std::unordered_set<std::string> expected{"/ints/x", "/ints/y"};
    CHECK(pathsCopy.size() == expected.size());
    for (auto const& p : pathsCopy) {
        CHECK(expected.contains(p));
    }
}

TEST_CASE("Pack insert detects lane length drift after single-lane insert") {
    PathSpace space;
    REQUIRE(space.insert<"/ints/x","/ints/y">(1, 1).errors.empty());
    REQUIRE(space.insert("/ints/x", 99).errors.empty()); // skew lane x only

    auto ret = space.read<"x","y">("/ints", [&](std::span<const int>, std::span<const int>) {});
    CHECK_FALSE(ret.has_value());
    CHECK(ret.error().code == Error::Code::InvalidType);
}

TEST_CASE("Pack insert concurrent const readers stay aligned") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 0).errors.empty());
    REQUIRE(space.insert("/ints/y", 0).errors.empty());

    std::atomic<bool> stop{false};
    std::atomic<bool> failure{false};

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto ret = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
                if (xs.size() != ys.size()) {
                    failure.store(true, std::memory_order_release);
                }
            });
            if (!ret.has_value()) {
                failure.store(true, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    for (int i = 1; i <= 200; ++i) {
        auto ret = space.insert<"/ints/x","/ints/y">(i, i);
        REQUIRE(ret.errors.empty());
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK_FALSE(failure.load(std::memory_order_acquire));
}

TEST_CASE("Pack insert multi-thread stress maintains alignment") {
    PathSpace space;
    CHECK(space.insert("/ints/x", 0).errors.empty());
    CHECK(space.insert("/ints/y", 0).errors.empty());

    constexpr int kThreads   = 6;
    constexpr int kPerThread = 150;
    std::atomic<int> next{1};
    std::atomic<bool> skew{false};

    auto worker = [&]() {
        for (int i = 0; i < kPerThread; ++i) {
            int v = next.fetch_add(1, std::memory_order_relaxed);
            auto ret = space.insert<"/ints/x","/ints/y">(v, v);
            REQUIRE(ret.errors.empty());
        }
    };

    std::vector<std::thread> writers;
    for (int i = 0; i < kThreads; ++i) writers.emplace_back(worker);

    std::thread reader([&]() {
        for (int i = 0; i < 300; ++i) {
            auto res = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
                if (xs.size() != ys.size()) skew.store(true, std::memory_order_release);
            });
            if (!res.has_value()) std::this_thread::yield();
        }
    });

    for (auto& t : writers) t.join();
    reader.join();

    CHECK_FALSE(skew.load(std::memory_order_acquire));
    auto final = space.read<"x","y">("/ints", [&](std::span<const int> xs, std::span<const int> ys) {
        REQUIRE(xs.size() == ys.size());
        REQUIRE(xs.size() == static_cast<std::size_t>(kThreads * kPerThread + 1)); // +1 seed
        for (std::size_t i = 0; i < xs.size(); ++i) {
            CHECK(xs[i] == ys[i]);
        }
    });
    REQUIRE(final.has_value());
}

TEST_CASE("Pack insert notifies blocking reads") {
    PathSpace space;
    std::promise<Expected<int>> resultPromise;
    std::atomic<bool>           readerStarted{false};

    std::thread reader([&]() {
        readerStarted.store(true, std::memory_order_release);
        auto res = space.read<int>("/ints/x", Out{} & Block{std::chrono::milliseconds{200}});
        resultPromise.set_value(std::move(res));
    });

    while (!readerStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto ret = space.insert<"/ints/x","/ints/y">(123, 456);
    REQUIRE(ret.errors.empty());

    auto future = resultPromise.get_future();
    auto status = future.wait_for(std::chrono::seconds{1});
    REQUIRE(status == std::future_status::ready);
    auto res = future.get();
    REQUIRE(res.has_value());
    CHECK(res.value() == 123);

    auto y = space.read<int>("/ints/y");
    REQUIRE(y.has_value());
    CHECK(y.value() == 456);

    reader.join();
}

TEST_CASE("Pack insert rejects glob and indexed paths") {
    PathSpace space;
    auto glob = space.insert<"/ints/*","/ints/y">(1, 2);
    CHECK_FALSE(glob.errors.empty());
    CHECK(glob.errors.front().code == Error::Code::InvalidPath);
    CHECK(glob.nbrValuesInserted == 0);

    auto indexed = space.insert<"/ints/values[0]","/ints/values[1]">(3, 4);
    CHECK_FALSE(indexed.errors.empty());
    CHECK(indexed.errors.front().code == Error::Code::InvalidPath);
    CHECK(indexed.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert rejects mixed existing POD types and preserves data") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 1.0f).errors.empty());
    REQUIRE(space.insert("/ints/y", 2.0f).errors.empty());

    auto ret = space.insert<"/ints/x","/ints/y">(5, 6);
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.nbrValuesInserted == 0);

    auto verify = space.read<"x","y">("/ints", [&](std::span<const float> xs, std::span<const float> ys) {
        REQUIRE(xs.size() == 1);
        REQUIRE(ys.size() == 1);
        CHECK(xs[0] == 1.0f);
        CHECK(ys[0] == 2.0f);
    });
    REQUIRE(verify.has_value());
}

TEST_CASE("Minimal span take fails fast when lane lacks POD payload") {
    PathSpace space;
    REQUIRE(space.insert("/ints/x", 7).errors.empty());
    REQUIRE(space.insert("/ints/y", std::string("nonpod")).errors.empty());

    auto ret = space.take<"x","y">("/ints",
                                   [&](std::span<int>, std::span<int>) {},
                                   Out{} & Minimal{});
    CHECK_FALSE(ret.has_value());
    auto code = ret.error().code;
    CHECK(code == Error::Code::InvalidType);
    if (code != Error::Code::InvalidType) {
        CHECK(code == Error::Code::NotSupported);
    }

    auto x = space.read<int>("/ints/x");
    REQUIRE(x.has_value());
    CHECK(x.value() == 7);
}

TEST_CASE("Pack insert rejects arity mismatch") {
    SP::Leaf leaf;
    std::array<std::string, 2> paths{"/a", "/b"};
    int                         value = 5;
    InputMetadata               md{InputMetadataT<int>{}};
    md.createPodPayload = &PodPayload<int>::CreateShared;

    std::array<void const*, 1> values{&value};
    auto ret = leaf.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                               md,
                               std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::InvalidType);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert rejects non-POD metadata") {
    SP::Leaf leaf;
    std::array<std::string, 1> paths{"/a"};
    std::string                value{"hello"};
    InputMetadata              md{InputMetadataT<std::string>{}}; // podPreferred == false

    std::array<void const*, 1> values{&value};
    auto ret = leaf.packInsert(std::span<const std::string>(paths.data(), paths.size()),
                               md,
                               std::span<void const* const>(values.data(), values.size()));
    CHECK_FALSE(ret.errors.empty());
    CHECK(ret.errors.front().code == Error::Code::NotSupported);
    CHECK(ret.nbrValuesInserted == 0);
}

TEST_CASE("Pack insert failure does not stall concurrent POD insert") {
    PathSpace space;
    REQUIRE(space.insert("/ints/y", std::string("nonpod")).errors.empty());

    std::atomic<bool> packPause(true);
    std::atomic<bool> packSeen(false);
    std::atomic<bool> pushDone(false);
    std::atomic<bool> hookHold(true);

    gPackPause = &packPause;
    gPackSeen  = &packSeen;
    gHookHold  = &hookHold;
    gHookSeen  = nullptr;
    testing::SetPackInsertReservationHook(&pack_reservation_hook);
    testing::SetPodPayloadPushHook(&pod_push_hook);

    std::thread packer([&]() {
        auto ret = space.insert<"/ints/x","/ints/y">(1, 2);
        CHECK_FALSE(ret.errors.empty());
    });

    for (int i = 0; i < 1000 && !packSeen.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    REQUIRE(packSeen.load(std::memory_order_acquire));

    std::thread inserter([&]() {
        auto ret = space.insert("/ints/x", 99);
        CHECK(ret.errors.empty());
        pushDone.store(true, std::memory_order_release);
    });

    packPause.store(false, std::memory_order_release);
    packer.join();

    hookHold.store(false, std::memory_order_release);

    bool finished = false;
    for (int i = 0; i < 2000; ++i) {
        if (pushDone.load(std::memory_order_acquire)) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (!finished) {
        inserter.detach();
    } else {
        inserter.join();
    }
    CHECK(finished);

    testing::SetPodPayloadPushHook(nullptr);
    testing::SetPackInsertReservationHook(nullptr);
    gHookHold  = nullptr;
    gHookSeen  = nullptr;
    gPackPause = nullptr;
    gPackSeen  = nullptr;

    auto val = space.take<int>("/ints/x");
    REQUIRE(val.has_value());
    CHECK(val.value() == 99);
}
TEST_CASE("POD fast path publishes data before readers see queue growth") {
    PathSpace space;
    std::atomic<bool> hold(true);
    std::atomic<bool> hookSeen(false);

    gHookHold    = &hold;
    gHookSeen    = &hookSeen;
    testing::SetPodPayloadPushHook(&pod_push_hook);

    std::optional<int> observed;
    std::optional<int> earlyObserved;

    std::thread consumer([&]() {
        int spins = 0;
        while (!hookSeen.load(std::memory_order_acquire) && spins < 1'000'000) {
            std::this_thread::yield();
            ++spins;
        }
        REQUIRE(hookSeen.load(std::memory_order_acquire));
        auto gotEarly = space.take<int>("/ints");
        if (gotEarly.has_value()) {
            earlyObserved = gotEarly.value();
        }
        hold.store(false, std::memory_order_release);
        for (int attempt = 0; attempt < 1000 && !observed.has_value(); ++attempt) {
            auto got = space.take<int>("/ints");
            if (got.has_value()) {
                observed = got.value();
                break;
            }
            std::this_thread::yield();
        }
    });

    std::thread producer([&]() {
        auto ret = space.insert("/ints", 123);
        REQUIRE(ret.errors.empty());
    });

    consumer.join();
    producer.join();

    testing::SetPodPayloadPushHook(nullptr);
    gHookHold    = nullptr;
    gHookSeen    = nullptr;

    CHECK_FALSE(earlyObserved.has_value());
    REQUIRE(observed.has_value());
    CHECK(*observed == 123);
}

TEST_SUITE_END();
