#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/declarative/WidgetStateMutators.hpp>

using namespace SP;
using namespace SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;

namespace {

auto insert_button(PathSpace& space, std::string const& widget_path) -> void {
    BuilderWidgets::ButtonState state{};
    auto inserted = space.insert(widget_path + "/state", state);
    REQUIRE(inserted.errors.empty());
    (void)space.insert(widget_path + "/render/dirty", false);
}

auto insert_list(PathSpace& space, std::string const& widget_path) -> void {
    BuilderWidgets::ListState state{};
    auto inserted = space.insert(widget_path + "/state", state);
    REQUIRE(inserted.errors.empty());
    BuilderWidgets::ListStyle style{};
    (void)space.insert(widget_path + "/meta/style", style);
    (void)space.insert(widget_path + "/render/dirty", false);
}

} // namespace

TEST_CASE("SetButtonHovered flips hover flag and dirties render state") {
    PathSpace space;
    const std::string widget_path = "/system/applications/test/windows/main/widgets/button";
    insert_button(space, widget_path);

    SetButtonHovered(space, widget_path, true);
    auto state = space.read<BuilderWidgets::ButtonState, std::string>(widget_path + "/state");
    REQUIRE(state);
    CHECK(state->hovered);
    auto dirty = space.read<bool, std::string>(widget_path + "/render/dirty");
    REQUIRE(dirty);
    CHECK(*dirty);

    SetButtonPressed(space, widget_path, true);
    state = space.read<BuilderWidgets::ButtonState, std::string>(widget_path + "/state");
    REQUIRE(state);
    CHECK(state->pressed);
}

TEST_CASE("SetListHoverIndex assigns hovered and selected indices") {
    PathSpace space;
    const std::string widget_path = "/system/applications/test/windows/main/widgets/list";
    insert_list(space, widget_path);

    SetListHoverIndex(space, widget_path, 1);
    auto state = space.read<BuilderWidgets::ListState, std::string>(widget_path + "/state");
    REQUIRE(state);
    CHECK(state->hovered_index == 1);

    SetListSelectionIndex(space, widget_path, 1);
    state = space.read<BuilderWidgets::ListState, std::string>(widget_path + "/state");
    REQUIRE(state);
    CHECK(state->selected_index == 1);
}
