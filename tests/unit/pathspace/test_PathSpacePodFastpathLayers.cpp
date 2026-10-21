#include "PathSpacePodFastpathTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.pod.fastpath.layers");

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

TEST_SUITE_END();
