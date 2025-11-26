#include "PathSpace.hpp"
#include "inspector/InspectorSnapshot.hpp"

#include "third_party/doctest.h"

using SP::Inspector::InspectorSnapshotOptions;
using SP::Inspector::BuildInspectorSnapshot;

TEST_CASE("Inspector snapshot captures tree structure and summaries") {
    SP::PathSpace space;
    space.insert("/demo/button/meta/label", std::string{"Launch"});
    space.insert("/demo/button/state/enabled", true);
    space.insert("/demo/slider/state/value", std::uint64_t{75});
    space.insert("/demo/slider/state/range/min", std::uint64_t{0});
    space.insert("/demo/slider/state/range/max", std::uint64_t{100});

    InspectorSnapshotOptions options;
    options.root         = "/demo";
    options.max_depth    = 2;
    options.max_children = 8;

    auto snapshot = BuildInspectorSnapshot(space, options);
    REQUIRE(snapshot);
    CHECK(snapshot->root.child_count == 2);
    CHECK(snapshot->root.children.size() == 2);

    auto const& button = snapshot->root.children[0];
    CHECK(button.path == "/demo/button");
    CHECK(button.child_count == 2);

    auto const& slider = snapshot->root.children[1];
    CHECK(slider.path == "/demo/slider");
    CHECK(slider.child_count == 1);
}

TEST_CASE("Inspector snapshot respects child limit") {
    SP::PathSpace space;
    for (int i = 0; i < 10; ++i) {
        auto path = std::string{"/limits/item_"} + std::to_string(i);
        space.insert(path, static_cast<std::uint64_t>(i));
    }

    InspectorSnapshotOptions options;
    options.root         = "/limits";
    options.max_depth    = 1;
    options.max_children = 3;

    auto snapshot = BuildInspectorSnapshot(space, options);
    REQUIRE(snapshot);
    CHECK(snapshot->root.children.size() == 3);
    CHECK(snapshot->root.children_truncated);
}
