#include "PathSpacePodFastpathTestSupport.hpp"

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

TEST_SUITE_BEGIN("pathspace.pod.fastpath.core");

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
    constexpr int N = 1025; // > initial 1024
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

TEST_SUITE_END();
