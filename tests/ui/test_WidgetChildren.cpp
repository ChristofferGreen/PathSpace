#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <cstdint>

using SP::UI::Runtime::Widgets::WidgetChildRoot;
using SP::UI::Runtime::Widgets::WidgetChildren;

TEST_CASE("WidgetChildren flattens legacy nested capsules") {
    SP::PathSpace space;
    std::string widget_root = "/app/widgets/legacy_parent";

    // Simulate legacy layout: <widget>/children/children/<child>
    auto legacy_child = widget_root + "/children/children/legacy_child/meta/label";
    auto inserted = space.insert(legacy_child, std::string{"legacy"});
    REQUIRE(inserted.errors.empty());

    auto view = WidgetChildren(space, widget_root);
    CHECK_EQ(view.root, widget_root + "/children/children");
    REQUIRE(view.names.size() == 1);
    CHECK_EQ(view.names.front(), "legacy_child");

    auto resolved_child = WidgetChildRoot(space, widget_root, "legacy_child");
    CHECK_EQ(resolved_child, widget_root + "/children/children/legacy_child");
}

TEST_CASE("WidgetChildren filters housekeeping nodes") {
    SP::PathSpace space;
    std::string widget_root = "/app/widgets/housekeeping";

    auto child_path = widget_root + "/children/real/meta/label";
    auto child_insert = space.insert(child_path, std::string{"real"});
    REQUIRE(child_insert.errors.empty());

    // Noise that should be filtered from the children list.
    (void)space.insert(widget_root + "/children/space/log", std::string{"keep"});
    (void)space.insert(widget_root + "/children/log/events", std::string{"keep"});
    (void)space.insert(widget_root + "/children/metrics/total", std::uint64_t{1});
    (void)space.insert(widget_root + "/children/runtime/state", std::string{"idle"});

    auto view = WidgetChildren(space, widget_root);
    CHECK_EQ(view.root, widget_root + "/children");
    REQUIRE(view.names.size() == 1);
    CHECK_EQ(view.names.front(), "real");
}
