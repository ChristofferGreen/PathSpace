#include "WidgetEventCommon.hpp"

#include "WidgetStateMutators.hpp"
#include "widgets/Common.hpp"

#include "DescriptorDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace SP::UI::Declarative {

namespace DeclarativeDetail = SP::UI::Declarative::Detail;
namespace DescriptorHelpers = SP::UI::Declarative::DescriptorDetail;

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto enqueue_error(PathSpace& space, std::string const& message) -> void {
    auto inserted = space.insert(std::string{kWidgetEventsLogQueue}, message);
    (void)inserted;
}

auto normalize_root(std::string root) -> std::string {
    if (root.empty()) {
        return std::string{"/"};
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    if (root.empty()) {
        return std::string{"/"};
    }
    return root;
}

auto list_children(PathSpace& space, std::string const& path) -> std::vector<std::string> {
    SP::ConcretePathStringView view{path};
    return space.listChildren(view);
}

auto mark_widget_dirty(PathSpace& space, std::string const& widget_path) -> void {
    auto status = DeclarativeDetail::mark_render_dirty(space, widget_path);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to mark render dirty for " + widget_path);
    }
}

auto read_slider_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<SliderData> {
    auto widget = SP::UI::Runtime::WidgetPath{widget_path};
    if (auto theme = DescriptorHelpers::ResolveThemeForWidget(space, widget)) {
        auto descriptor = DescriptorHelpers::ReadSliderDescriptor(space, widget_path, theme->theme);
        if (descriptor) {
            SliderData data{};
            data.state = descriptor->state;
            data.style = descriptor->style;
            data.range = descriptor->range;
            return data;
        }
        enqueue_error(space, "WidgetEventTrellis failed to read slider descriptor for " + widget_path);
    }

    SliderData data{};
    auto state = space.read<BuilderWidgets::SliderState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider state for " + widget_path);
        return std::nullopt;
    }
    data.state = *state;
    auto style = space.read<BuilderWidgets::SliderStyle, std::string>(widget_path + "/meta/style");
    if (!style) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider style for " + widget_path);
        return std::nullopt;
    }
    data.style = *style;
    auto range = space.read<BuilderWidgets::SliderRange, std::string>(widget_path + "/meta/range");
    if (!range) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider range for " + widget_path);
        return std::nullopt;
    }
    data.range = *range;
    return data;
}

auto write_slider_state(PathSpace& space,
                        std::string const& widget_path,
                        BuilderWidgets::SliderState const& state) -> bool {
    auto status = DeclarativeDetail::replace_single<BuilderWidgets::SliderState>(space,
                                                             widget_path + "/state",
                                                             state);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write slider state for " + widget_path);
        return false;
    }
    mark_widget_dirty(space, widget_path);
    return true;
}

auto update_slider_hover(PathSpace& space,
                         std::string const& widget_path,
                         bool hovered) -> void {
    auto data = read_slider_data(space, widget_path);
    if (!data) {
        return;
    }
    if (data->state.hovered == hovered) {
        return;
    }
    data->state.hovered = hovered;
    write_slider_state(space, widget_path, data->state);
}

auto read_list_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<ListData> {
    auto widget = SP::UI::Runtime::WidgetPath{widget_path};
    if (auto theme = DescriptorHelpers::ResolveThemeForWidget(space, widget)) {
        auto descriptor = DescriptorHelpers::ReadListDescriptor(space, widget_path, theme->theme);
        if (descriptor) {
            ListData data{};
            data.state = descriptor->state;
            data.style = descriptor->style;
            data.items = descriptor->items;
            return data;
        }
        enqueue_error(space, "WidgetEventTrellis failed to read list descriptor for " + widget_path);
    }

    ListData data{};
    auto state = space.read<BuilderWidgets::ListState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read list state for " + widget_path);
        return std::nullopt;
    }
    data.state = *state;
    auto style = space.read<BuilderWidgets::ListStyle, std::string>(widget_path + "/meta/style");
    if (!style) {
        enqueue_error(space, "WidgetEventTrellis failed to read list style for " + widget_path);
        return std::nullopt;
    }
    data.style = *style;
    auto items = space.read<std::vector<BuilderWidgets::ListItem>, std::string>(widget_path + "/meta/items");
    if (!items) {
        enqueue_error(space, "WidgetEventTrellis failed to read list items for " + widget_path);
        return std::nullopt;
    }
    data.items = *items;
    return data;
}

auto read_tree_data(PathSpace& space, std::string const& widget_path) -> std::optional<TreeData> {
    auto widget = SP::UI::Runtime::WidgetPath{widget_path};
    if (auto theme = DescriptorHelpers::ResolveThemeForWidget(space, widget)) {
        auto descriptor = DescriptorHelpers::ReadTreeDescriptor(space, widget_path, theme->theme);
        if (descriptor) {
            TreeData data{};
            data.state = descriptor->state;
            data.style = descriptor->style;
            data.nodes = descriptor->nodes;
            return data;
        }
        enqueue_error(space, "WidgetEventTrellis failed to read tree descriptor for " + widget_path);
    }

    TreeData data{};
    auto state = space.read<BuilderWidgets::TreeState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read tree state for " + widget_path);
        return std::nullopt;
    }
    data.state = *state;
    auto style = space.read<BuilderWidgets::TreeStyle, std::string>(widget_path + "/meta/style");
    if (!style) {
        enqueue_error(space, "WidgetEventTrellis failed to read tree style for " + widget_path);
        return std::nullopt;
    }
    data.style = *style;
    auto nodes = space.read<std::vector<BuilderWidgets::TreeNode>, std::string>(widget_path + "/meta/nodes");
    if (!nodes) {
        enqueue_error(space, "WidgetEventTrellis failed to read tree nodes for " + widget_path);
        return std::nullopt;
    }
    data.nodes = *nodes;
    return data;
}

auto tree_node_expanded(TreeData const& data, std::string const& node_id) -> bool {
    auto const& expanded = data.state.expanded_ids;
    return std::find(expanded.begin(), expanded.end(), node_id) != expanded.end();
}

auto build_tree_rows(TreeData const& data) -> std::vector<TreeRowInfo> {
    std::unordered_map<std::string, std::vector<BuilderWidgets::TreeNode>> children;
    for (auto const& node : data.nodes) {
        children[node.parent_id].push_back(node);
    }

    std::vector<TreeRowInfo> rows;
    auto visit = [&](auto&& self, std::string const& parent, int depth) -> void {
        auto it = children.find(parent);
        if (it == children.end()) {
            return;
        }
        for (auto const& child : it->second) {
            TreeRowInfo row{};
            row.id = child.id;
            row.parent_id = child.parent_id;
            row.expandable = child.expandable;
            row.expanded = child.expandable && tree_node_expanded(data, child.id);
            row.enabled = child.enabled;
            row.depth = depth;
            rows.push_back(row);
            if (row.expandable && row.expanded) {
                self(self, child.id, depth + 1);
            }
        }
    };

    visit(visit, std::string{}, 0);
    return rows;
}

auto tree_row_index(std::vector<TreeRowInfo> const& rows,
                    std::string const& node_id) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id == node_id) {
            return i;
        }
    }
    return std::nullopt;
}

auto read_text_state(PathSpace& space, std::string const& widget_path)
    -> std::optional<BuilderWidgets::TextFieldState> {
    auto state = space.read<BuilderWidgets::TextFieldState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read text state for " + widget_path);
        return std::nullopt;
    }
    return *state;
}

auto write_text_state(PathSpace& space,
                      std::string const& widget_path,
                      BuilderWidgets::TextFieldState const& state) -> bool {
    auto status = DeclarativeDetail::replace_single<BuilderWidgets::TextFieldState>(space,
                                                                widget_path + "/state",
                                                                state);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write text state for " + widget_path);
        return false;
    }
    mark_widget_dirty(space, widget_path);
    return true;
}

auto default_focus_pointer() -> WidgetBindings::PointerInfo {
    WidgetBindings::PointerInfo pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    pointer.WithInside(true);
    pointer.WithPrimary(true);
    return pointer;
}

auto focus_pointer_with_local(float local_x, float local_y) -> WidgetBindings::PointerInfo {
    WidgetBindings::PointerInfo pointer = default_focus_pointer();
    pointer.WithLocal(local_x, local_y);
    return pointer;
}

auto classify_focus_nav(SP::IO::ButtonEvent const& event) -> std::optional<FocusNavEvent> {
    if (event.source != SP::IO::ButtonSource::Keyboard
        && event.source != SP::IO::ButtonSource::Gamepad) {
        return std::nullopt;
    }

    FocusNavEvent nav{};
    nav.pressed = event.state.pressed;
    nav.repeat = event.state.repeat;
    nav.from_keyboard = (event.source == SP::IO::ButtonSource::Keyboard);
    nav.from_gamepad = (event.source == SP::IO::ButtonSource::Gamepad);

    if (!nav.pressed) {
        return std::nullopt;
    }

    if (nav.from_keyboard) {
        switch (event.button_code) {
        case kKeycodeLeft:
            nav.direction = FocusDirection::Left;
            break;
        case kKeycodeRight:
            nav.direction = FocusDirection::Right;
            break;
        case kKeycodeUp:
            nav.direction = FocusDirection::Up;
            break;
        case kKeycodeDown:
            nav.direction = FocusDirection::Down;
            break;
        case kKeycodeReturn:
        case kKeycodeEnter:
            nav.command = FocusCommand::Submit;
            break;
        case kKeycodeDeleteBackward:
            nav.command = FocusCommand::DeleteBackward;
            break;
        case kKeycodeDeleteForward:
            nav.command = FocusCommand::DeleteForward;
            break;
        default:
            break;
        }
    } else {
        switch (event.button_id) {
        case kGamepadDpadLeft:
        case kGamepadLeftShoulder:
            nav.direction = FocusDirection::Left;
            break;
        case kGamepadDpadRight:
        case kGamepadRightShoulder:
            nav.direction = FocusDirection::Right;
            break;
        case kGamepadDpadUp:
            nav.direction = FocusDirection::Up;
            break;
        case kGamepadDpadDown:
            nav.direction = FocusDirection::Down;
            break;
        case kGamepadButtonA:
            nav.command = FocusCommand::Submit;
            break;
        default:
            break;
        }
    }

    if (nav.direction == FocusDirection::None && nav.command == FocusCommand::None) {
        return std::nullopt;
    }
    return nav;
}

auto focused_widget_path(PathSpace& space,
                         WindowBinding const& binding) -> std::optional<std::string> {
    if (!binding.app_root.empty()) {
        auto app_focus = space.read<std::string, std::string>(binding.app_root + "/widgets/focus/current");
        if (app_focus && !app_focus->empty()) {
            return *app_focus;
        }
    }

    auto component = DeclarativeDetail::window_component_for(binding.window_path);
    if (!component) {
        enqueue_error(space, "WidgetEventTrellis failed to derive window component for focus path");
        return std::nullopt;
    }
    auto make_focus_path = [&](std::string const& root) {
        return root + "/structure/window/" + *component + "/focus/current";
    };

    auto read_focus = [&](std::string const& path) -> std::optional<std::string> {
        auto value = space.read<std::string, std::string>(path);
        if (!value || value->empty()) {
            return std::nullopt;
        }
        return *value;
    };

    if (!binding.scene_path.empty()) {
        if (auto value = read_focus(make_focus_path(binding.scene_path))) {
            return value;
        }
    }
    return std::nullopt;
}

auto clamp_slider_value(SliderData const& data, float value) -> float {
    auto min = std::min(data.range.minimum, data.range.maximum);
    auto max = std::max(data.range.minimum, data.range.maximum);
    if (max - min <= 1e-6f) {
        return min;
    }
    auto clamped = std::clamp(value, min, max);
    if (data.range.step > 0.0f) {
        auto steps = std::round((clamped - min) / data.range.step);
        clamped = min + steps * data.range.step;
        clamped = std::clamp(clamped, min, max);
    }
    return clamped;
}

auto slider_value_from_local(SliderData const& data, float local_x) -> float {
    auto width = std::max(data.style.width, 1.0f);
    auto clamped = std::clamp(local_x, 0.0f, width);
    auto progress = clamped / width;
    auto value = data.range.minimum + (data.range.maximum - data.range.minimum) * progress;
    return clamp_slider_value(data, value);
}

auto slider_local_from_value(SliderData const& data, float value) -> float {
    float width = std::max(data.style.width, 1.0f);
    float range = std::max(data.range.maximum - data.range.minimum, 1e-6f);
    float normalized = (value - data.range.minimum) / range;
    return std::clamp(normalized, 0.0f, 1.0f) * width;
}

auto slider_step_size(SliderData const& data) -> float {
    if (data.range.step > 0.0f) {
        return data.range.step;
    }
    float span = std::abs(data.range.maximum - data.range.minimum);
    if (span <= 1e-6f) {
        return 0.0f;
    }
    return span * 0.05f;
}

auto list_index_from_local(ListData const& data, float local_y) -> std::optional<std::int32_t> {
    if (data.items.empty()) {
        return std::nullopt;
    }
    auto row_height = data.style.item_height;
    if (row_height <= 0.0f) {
        return std::nullopt;
    }
    auto offset = local_y / row_height;
    auto index = static_cast<int>(std::floor(offset));
    if (index < 0 || index >= static_cast<int>(data.items.size())) {
        return std::nullopt;
    }
    return index;
}

auto list_local_center(ListData const& data, std::int32_t index) -> std::pair<float, float> {
    float height = std::max(data.style.item_height, 1.0f);
    float y = (static_cast<float>(index) + 0.5f) * height;
    float x = data.style.width * 0.5f;
    return {x, y};
}

auto list_item_id(ListData const& data, std::int32_t index) -> std::optional<std::string> {
    if (index < 0 || index >= static_cast<std::int32_t>(data.items.size())) {
        return std::nullopt;
    }
    return data.items[static_cast<std::size_t>(index)].id;
}

auto write_stack_active_panel(PathSpace& space,
                              std::string const& widget_path,
                              std::string const& panel_id) -> bool {
    auto status = DeclarativeDetail::replace_single<std::string>(space,
                                              widget_path + "/state/active_panel",
                                              panel_id);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write stack active panel for " + widget_path);
        return false;
    }
    mark_widget_dirty(space, widget_path);
    return true;
}

} // namespace SP::UI::Declarative
