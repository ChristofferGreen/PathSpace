#include <pathspace/ui/Builders.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace SP::UI::Builders::Widgets::Input {

namespace {

template <typename T>
auto require(T* ptr, char const* name) -> T& {
    if (!ptr) {
        throw std::runtime_error(std::string("WidgetInputContext missing required field: ") + name);
    }
    return *ptr;
}

struct ContextAdapter {
    explicit ContextAdapter(WidgetInputContext& base)
        : ctx(base) {}

    auto space() -> PathSpace& { return require(ctx.space, "space"); }
    auto space() const -> PathSpace const& { return require(ctx.space, "space"); }

    auto pointer_x() -> float& { return require(ctx.pointer_x, "pointer_x"); }
    auto pointer_x() const -> float const& { return require(ctx.pointer_x, "pointer_x"); }

    auto pointer_y() -> float& { return require(ctx.pointer_y, "pointer_y"); }
    auto pointer_y() const -> float const& { return require(ctx.pointer_y, "pointer_y"); }

    auto pointer_down() -> bool& { return require(ctx.pointer_down, "pointer_down"); }
    auto pointer_down() const -> bool const& { return require(ctx.pointer_down, "pointer_down"); }

    auto slider_dragging() -> bool& { return require(ctx.slider_dragging, "slider_dragging"); }
    auto slider_dragging() const -> bool const& { return require(ctx.slider_dragging, "slider_dragging"); }

    auto button_state() -> Widgets::ButtonState& { return require(ctx.button_state, "button_state"); }
    auto button_state() const -> Widgets::ButtonState const& { return require(ctx.button_state, "button_state"); }

    auto toggle_state() -> Widgets::ToggleState& { return require(ctx.toggle_state, "toggle_state"); }
    auto toggle_state() const -> Widgets::ToggleState const& { return require(ctx.toggle_state, "toggle_state"); }

    auto slider_state() -> Widgets::SliderState& { return require(ctx.slider_state, "slider_state"); }
    auto slider_state() const -> Widgets::SliderState const& { return require(ctx.slider_state, "slider_state"); }

    auto list_state() -> Widgets::ListState& { return require(ctx.list_state, "list_state"); }
    auto list_state() const -> Widgets::ListState const& { return require(ctx.list_state, "list_state"); }

    auto tree_state() -> Widgets::TreeState& { return require(ctx.tree_state, "tree_state"); }
    auto tree_state() const -> Widgets::TreeState const& { return require(ctx.tree_state, "tree_state"); }

    auto list_items() -> std::vector<Widgets::ListItem>& { return require(ctx.list_items, "list_items"); }
    auto list_items() const -> std::vector<Widgets::ListItem> const& { return require(ctx.list_items, "list_items"); }

    auto tree_nodes() -> std::vector<Widgets::TreeNode>& { return require(ctx.tree_nodes, "tree_nodes"); }
    auto tree_nodes() const -> std::vector<Widgets::TreeNode> const& { return require(ctx.tree_nodes, "tree_nodes"); }

    auto button_binding() -> Bindings::ButtonBinding& { return require(ctx.button_binding, "button_binding"); }
    auto toggle_binding() -> Bindings::ToggleBinding& { return require(ctx.toggle_binding, "toggle_binding"); }
    auto slider_binding() -> Bindings::SliderBinding& { return require(ctx.slider_binding, "slider_binding"); }
    auto list_binding() -> Bindings::ListBinding& { return require(ctx.list_binding, "list_binding"); }
    auto tree_binding() -> Bindings::TreeBinding& { return require(ctx.tree_binding, "tree_binding"); }

    auto button_paths() const -> Widgets::ButtonPaths const& { return require(ctx.button_paths, "button_paths"); }
    auto toggle_paths() const -> Widgets::TogglePaths const& { return require(ctx.toggle_paths, "toggle_paths"); }
    auto slider_paths() const -> Widgets::SliderPaths const& { return require(ctx.slider_paths, "slider_paths"); }
    auto list_paths() const -> Widgets::ListPaths const& { return require(ctx.list_paths, "list_paths"); }
    auto tree_paths() const -> Widgets::TreePaths const& { return require(ctx.tree_paths, "tree_paths"); }

    auto slider_style() const -> Widgets::SliderStyle const& { return require(ctx.slider_style, "slider_style"); }
    auto slider_range() const -> Widgets::SliderRange const& { return require(ctx.slider_range, "slider_range"); }
    auto list_style() const -> Widgets::ListStyle const& { return require(ctx.list_style, "list_style"); }
    auto tree_style() const -> Widgets::TreeStyle const& { return require(ctx.tree_style, "tree_style"); }

    auto tree_pointer_down_id() -> std::string& { return require(ctx.tree_pointer_down_id, "tree_pointer_down_id"); }
    auto tree_pointer_toggle() -> bool& { return require(ctx.tree_pointer_toggle, "tree_pointer_toggle"); }

    auto focus_config() -> Focus::Config& { return require(ctx.focus.config, "focus.config"); }
    auto focus_config() const -> Focus::Config const& { return require(ctx.focus.config, "focus.config"); }

    auto focus_target() -> FocusTarget& { return require(ctx.focus.current, "focus.current"); }
    auto focus_target() const -> FocusTarget const& { return require(ctx.focus.current, "focus.current"); }

    auto focus_order() const -> std::span<FocusTarget const> { return ctx.focus.order; }

    auto focus_button_path() const -> std::optional<WidgetPath> const& { return ctx.focus.button; }
    auto focus_toggle_path() const -> std::optional<WidgetPath> const& { return ctx.focus.toggle; }
    auto focus_slider_path() const -> std::optional<WidgetPath> const& { return ctx.focus.slider; }
    auto focus_list_path() const -> std::optional<WidgetPath> const& { return ctx.focus.list; }
    auto focus_tree_path() const -> std::optional<WidgetPath> const& { return ctx.focus.tree; }

    auto focus_list_index() -> int& { return require(ctx.focus.focus_list_index, "focus.focus_list_index"); }
    auto focus_list_index() const -> int const& { return require(ctx.focus.focus_list_index, "focus.focus_list_index"); }

    auto focus_tree_index() -> int& { return require(ctx.focus.focus_tree_index, "focus.focus_tree_index"); }
    auto focus_tree_index() const -> int const& { return require(ctx.focus.focus_tree_index, "focus.focus_tree_index"); }

    auto layout() const -> LayoutSnapshot const& { return ctx.layout; }

    WidgetInputContext& ctx;
};

auto make_pointer_info(float x, float y, bool inside) -> Bindings::PointerInfo {
    Bindings::PointerInfo info{};
    info.scene_x = x;
    info.scene_y = y;
    info.inside = inside;
    info.primary = true;
    return info;
}

auto make_pointer_info(WidgetInputContext const& ctx, bool inside) -> Bindings::PointerInfo {
    float x = ctx.pointer_x ? *ctx.pointer_x : 0.0f;
    float y = ctx.pointer_y ? *ctx.pointer_y : 0.0f;
    return make_pointer_info(x, y, inside);
}

auto read_button_state(ContextAdapter& c) -> void {
    auto updated = c.space().read<Widgets::ButtonState, std::string>(
        std::string(c.button_paths().state.getPath()));
    if (updated) {
        c.button_state() = *updated;
    }
}

auto read_toggle_state(ContextAdapter& c) -> void {
    auto updated = c.space().read<Widgets::ToggleState, std::string>(
        std::string(c.toggle_paths().state.getPath()));
    if (updated) {
        c.toggle_state() = *updated;
    }
}

auto read_slider_state(ContextAdapter& c) -> void {
    auto updated = c.space().read<Widgets::SliderState, std::string>(
        std::string(c.slider_paths().state.getPath()));
    if (updated) {
        c.slider_state() = *updated;
    }
}

auto read_list_state(ContextAdapter& c) -> void {
    auto updated = c.space().read<Widgets::ListState, std::string>(
        std::string(c.list_paths().state.getPath()));
    if (updated) {
        c.list_state() = *updated;
    }
}

auto read_tree_state(ContextAdapter& c) -> void {
    auto updated = c.space().read<Widgets::TreeState, std::string>(
        std::string(c.tree_paths().state.getPath()));
    if (updated) {
        c.tree_state() = *updated;
    }
}

auto dispatch_button(ContextAdapter& c,
                     Widgets::ButtonState const& desired,
                     Bindings::WidgetOpKind kind,
                     Bindings::PointerInfo const& pointer) -> bool {
    auto result = Bindings::DispatchButton(c.space(),
                                           c.button_binding(),
                                           desired,
                                           kind,
                                           pointer);
    if (!result) {
        std::cerr << "widgets::input: button dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        read_button_state(c);
    }
    return *result;
}

auto dispatch_toggle(ContextAdapter& c,
                     Widgets::ToggleState const& desired,
                     Bindings::WidgetOpKind kind,
                     Bindings::PointerInfo const& pointer) -> bool {
    auto result = Bindings::DispatchToggle(c.space(),
                                           c.toggle_binding(),
                                           desired,
                                           kind,
                                           pointer);
    if (!result) {
        std::cerr << "widgets::input: toggle dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        read_toggle_state(c);
    }
    return *result;
}

auto dispatch_slider(ContextAdapter& c,
                     Widgets::SliderState const& desired,
                     Bindings::WidgetOpKind kind,
                     Bindings::PointerInfo const& pointer) -> bool {
    auto result = Bindings::DispatchSlider(c.space(),
                                           c.slider_binding(),
                                           desired,
                                           kind,
                                           pointer);
    if (!result) {
        std::cerr << "widgets::input: slider dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        read_slider_state(c);
    }
    return *result;
}

auto dispatch_list(ContextAdapter& c,
                   Widgets::ListState const& desired,
                   Bindings::WidgetOpKind kind,
                   Bindings::PointerInfo const& pointer,
                   std::int32_t item_index,
                   float scroll_delta) -> bool {
    auto result = Bindings::DispatchList(c.space(),
                                         c.list_binding(),
                                         desired,
                                         kind,
                                         pointer,
                                         item_index,
                                         scroll_delta);
    if (!result) {
        std::cerr << "widgets::input: list dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        read_list_state(c);
    }
    return *result;
}

auto dispatch_tree(ContextAdapter& c,
                   Widgets::TreeState const& desired,
                   Bindings::WidgetOpKind kind,
                   std::string_view node_id,
                   Bindings::PointerInfo const& pointer,
                   float scroll_delta) -> bool {
    auto result = Bindings::DispatchTree(c.space(),
                                         c.tree_binding(),
                                         desired,
                                         kind,
                                         node_id,
                                         pointer,
                                         scroll_delta);
    if (!result) {
        std::cerr << "widgets::input: tree dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        read_tree_state(c);
    }
    return *result;
}

auto slider_value_from_position(ContextAdapter const& c, float x) -> float {
    if (!c.ctx.layout.slider) {
        return c.slider_range().minimum;
    }
    auto const& bounds = c.ctx.layout.slider->bounds;
    float width = bounds.width();
    if (width <= 0.0f) {
        return c.slider_range().minimum;
    }
    float t = (x - bounds.min_x) / width;
    t = std::clamp(t, 0.0f, 1.0f);
    return c.slider_range().minimum + t * (c.slider_range().maximum - c.slider_range().minimum);
}

auto slider_thumb_position(ContextAdapter const& c, float value) -> std::pair<float, float> {
    if (!c.ctx.layout.slider) {
        return {c.pointer_x(), c.pointer_y()};
    }
    auto const& bounds = c.ctx.layout.slider->bounds;
    float width = bounds.width();
    if (width <= 0.0f) {
        width = 1.0f;
    }
    float min_value = c.slider_range().minimum;
    float max_value = c.slider_range().maximum;
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    float range = std::max(max_value - min_value, 1e-6f);
    float progress = std::clamp((value - min_value) / range, 0.0f, 1.0f);

    float x = bounds.min_x + width * progress;
    float center_y = bounds.min_y + c.slider_style().height * 0.5f;
    return {x, center_y};
}

auto slider_step_magnitude(ContextAdapter const& c,
                           SliderStepOptions const& options) -> float {
    auto const& range = c.slider_range();
    float span = range.maximum - range.minimum;
    float span_abs = std::abs(span);

    float percent = std::max(options.percent_of_range, 0.0f);
    float step = span_abs * percent;

    if (options.respect_range_step && range.step > 0.0f) {
        step = step > 0.0f ? std::max(step, range.step) : range.step;
    }
    if (options.minimum_step > 0.0f) {
        step = step > 0.0f ? std::max(step, options.minimum_step) : options.minimum_step;
    }

    if (step <= 0.0f) {
        if (range.step > 0.0f) {
            step = range.step;
        } else if (span_abs > 0.0f) {
            step = span_abs;
        } else {
            step = 1.0f;
        }
    }

    return step;
}

auto slider_keyboard_step(ContextAdapter const& c) -> float {
    return slider_step_magnitude(c, SliderStepOptions{});
}

auto list_index_from_position(ContextAdapter const& c, float y) -> int {
    if (!c.ctx.layout.list) {
        return -1;
    }
    auto const& layout = *c.ctx.layout.list;
    if (layout.item_height <= 0.0f) {
        return -1;
    }
    float relative = y - layout.bounds.min_y - layout.content_top + c.list_state().scroll_offset;
    if (relative < 0.0f) {
        return -1;
    }
    int index = static_cast<int>(std::floor(relative / layout.item_height));
    if (index < 0 || index >= static_cast<int>(layout.item_bounds.size())) {
        return -1;
    }
    return index;
}

auto list_item_center(ContextAdapter const& c, int index) -> std::pair<float, float> {
    if (!c.ctx.layout.list) {
        return {c.pointer_x(), c.pointer_y()};
    }
    auto const& layout = *c.ctx.layout.list;
    if (index < 0 || index >= static_cast<int>(layout.item_bounds.size())) {
        index = std::clamp(index, 0, static_cast<int>(layout.item_bounds.size()) - 1);
    }
    auto const& bounds = layout.item_bounds[static_cast<std::size_t>(index)];
    return {bounds.min_x + bounds.width() * 0.5f,
            bounds.min_y + bounds.height() * 0.5f};
}

auto tree_toggle_contains(ContextAdapter const& c, int index, float x, float y) -> bool {
    if (!c.ctx.layout.tree) {
        return false;
    }
    auto const& layout = *c.ctx.layout.tree;
    if (index < 0 || index >= static_cast<int>(layout.rows.size())) {
        return false;
    }
    auto const& row = layout.rows[static_cast<std::size_t>(index)];
    return row.toggle.contains(x, y);
}

auto tree_row_index_from_position(ContextAdapter const& c, float y) -> int {
    if (!c.ctx.layout.tree) {
        return -1;
    }
    auto const& layout = *c.ctx.layout.tree;
    if (layout.row_height <= 0.0f) {
        return -1;
    }
    float relative = y - layout.bounds.min_y - layout.content_top + c.tree_state().scroll_offset;
    if (relative < 0.0f) {
        return -1;
    }
    int index = static_cast<int>(std::floor(relative / layout.row_height));
    if (index < 0 || index >= static_cast<int>(layout.rows.size())) {
        return -1;
    }
    return index;
}

auto tree_row_center(ContextAdapter const& c, int index) -> std::pair<float, float> {
    if (!c.ctx.layout.tree) {
        return {c.pointer_x(), c.pointer_y()};
    }
    auto const& layout = *c.ctx.layout.tree;
    if (index < 0 || index >= static_cast<int>(layout.rows.size())) {
        index = std::clamp(index, 0, static_cast<int>(layout.rows.size()) - 1);
    }
    auto const& bounds = layout.rows[static_cast<std::size_t>(index)].bounds;
    return {bounds.min_x + bounds.width() * 0.5f,
            bounds.min_y + bounds.height() * 0.5f};
}

auto tree_parent_index(ContextAdapter const& c, int index) -> int {
    if (!c.ctx.layout.tree) {
        return -1;
    }
    auto const& layout = *c.ctx.layout.tree;
    if (index < 0 || index >= static_cast<int>(layout.rows.size())) {
        return -1;
    }
    auto const& nodes = c.tree_nodes();
    auto const& row = layout.rows[static_cast<std::size_t>(index)];
    if (row.depth <= 0 || row.node_id.empty()) {
        return -1;
    }
    auto find_parent_id = [&](std::string const& id) -> std::optional<std::string> {
        auto it = std::find_if(nodes.begin(), nodes.end(), [&](auto const& node) {
            return node.id == id;
        });
        if (it == nodes.end()) {
            return std::nullopt;
        }
        return std::optional<std::string>{it->parent_id};
    };
    auto parent_id = find_parent_id(row.node_id);
    if (!parent_id || parent_id->empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        if (layout.rows[i].node_id == *parent_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

auto focus_widget_for_target(ContextAdapter const& c, FocusTarget target) -> std::optional<WidgetPath> {
    switch (target) {
    case FocusTarget::Button:
        return c.focus_button_path();
    case FocusTarget::Toggle:
        return c.focus_toggle_path();
    case FocusTarget::Slider:
        return c.focus_slider_path();
    case FocusTarget::List:
        return c.focus_list_path();
    case FocusTarget::Tree:
        return c.focus_tree_path();
    }
    return std::nullopt;
}

auto focus_target_from_path(ContextAdapter const& c, std::string const& path) -> std::optional<FocusTarget> {
    auto matches = [&](std::optional<WidgetPath> const& widget_path) -> bool {
        return widget_path && widget_path->getPath() == path;
    };
    if (matches(c.focus_button_path())) {
        return FocusTarget::Button;
    }
    if (matches(c.focus_toggle_path())) {
        return FocusTarget::Toggle;
    }
    if (matches(c.focus_slider_path())) {
        return FocusTarget::Slider;
    }
    if (matches(c.focus_list_path())) {
        return FocusTarget::List;
    }
    if (matches(c.focus_tree_path())) {
        return FocusTarget::Tree;
    }
    return std::nullopt;
}

auto ensure_tree_focus_index(ContextAdapter& c) -> bool {
    if (!c.ctx.layout.tree) {
        return false;
    }
    auto const& rows = c.ctx.layout.tree->rows;
    if (rows.empty()) {
        return false;
    }
    if (c.focus_tree_index() < 0 || c.focus_tree_index() >= static_cast<int>(rows.size())) {
        if (!c.tree_state().selected_id.empty()) {
            auto it = std::find_if(rows.begin(), rows.end(), [&](auto const& row) {
                return row.node_id == c.tree_state().selected_id;
            });
            if (it != rows.end()) {
                c.focus_tree_index() = static_cast<int>(std::distance(rows.begin(), it));
            } else {
                c.focus_tree_index() = 0;
            }
        } else {
            c.focus_tree_index() = 0;
        }
    }
    c.focus_tree_index() = std::clamp(c.focus_tree_index(), 0, static_cast<int>(rows.size()) - 1);
    return true;
}

auto make_pointer_info(ContextAdapter const& c, bool inside) -> Bindings::PointerInfo {
    return make_pointer_info(c.ctx, inside);
}

} // namespace

auto ProgrammaticPointer(float scene_x, float scene_y, bool inside) -> Bindings::PointerInfo {
    return make_pointer_info(scene_x, scene_y, inside);
}

auto SliderPointerForValue(WidgetInputContext const& ctx, float value) -> std::pair<float, float> {
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return slider_thumb_position(c, value);
}

auto SliderThumbPosition(WidgetInputContext const& ctx, float value) -> std::pair<float, float> {
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return slider_thumb_position(c, value);
}

auto ListItemCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float> {
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return list_item_center(c, index);
}

auto TreeRowCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float> {
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return tree_row_center(c, index);
}

auto TreeParentIndex(WidgetInputContext const& ctx, int index) -> int {
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return tree_parent_index(c, index);
}

auto RefreshFocusTargetFromSpace(WidgetInputContext& ctx) -> bool {
    ContextAdapter c{ctx};
    auto const& focus_path = c.focus_config().focus_state.getPath();
    if (focus_path.empty()) {
        return false;
    }
    auto focus_state = Focus::Current(c.space(), SP::ConcretePathStringView{focus_path});
    if (!focus_state) {
        std::cerr << "widgets::input: unable to read focus state: "
                  << focus_state.error().message.value_or("unknown error") << "\n";
        return false;
    }

    auto previous = c.focus_target();
    if (focus_state->has_value()) {
        if (auto mapped = focus_target_from_path(c, **focus_state)) {
            c.focus_target() = *mapped;
        }
    }
    return c.focus_target() != previous;
}

auto SetFocusTarget(WidgetInputContext& ctx,
                    FocusTarget target,
                    bool update_visuals) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    bool target_changed = (c.focus_target() != target);
    c.focus_target() = target;

    if (update_visuals) {
        if (auto widget_path = focus_widget_for_target(c, target)) {
            auto result = Focus::Set(c.space(), c.focus_config(), *widget_path);
            if (!result) {
                std::cerr << "widgets::input: focus set failed: "
                          << result.error().message.value_or("unknown error") << "\n";
            } else {
                update.focus_changed = result->changed;
            }
        }
    }

    if (RefreshFocusTargetFromSpace(ctx)) {
        update.focus_changed = true;
    }

    update.state_changed = target_changed || update.focus_changed;
    return update;
}

auto CycleFocus(WidgetInputContext& ctx, bool forward) -> InputUpdate {
    ContextAdapter c{ctx};
    auto order = c.focus_order();
    if (order.empty()) {
        return {};
    }
    auto it = std::find(order.begin(), order.end(), c.focus_target());
    std::size_t current = (it != order.end()) ? static_cast<std::size_t>(std::distance(order.begin(), it)) : 0;
    std::size_t next = (current + (forward ? 1 : order.size() - 1)) % order.size();
    return SetFocusTarget(ctx, order[next], true);
}

auto ActivateFocusedWidget(WidgetInputContext& ctx) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};

    switch (c.focus_target()) {
    case FocusTarget::Button: {
        auto const& bounds = c.layout().button;
        float cx = bounds.min_x + bounds.width() * 0.5f;
        float cy = bounds.min_y + bounds.height() * 0.5f;
        auto pointer = ProgrammaticPointer(cx, cy, true);
        auto desired = c.button_state();
        desired.hovered = true;
        if (dispatch_button(c,
                            desired,
                            Bindings::WidgetOpKind::Activate,
                            pointer)) {
            update.state_changed = true;
        }
        break;
    }
    case FocusTarget::Toggle: {
        auto const& bounds = c.layout().toggle;
        float cx = bounds.min_x + bounds.width() * 0.5f;
        float cy = bounds.min_y + bounds.height() * 0.5f;
        auto pointer = ProgrammaticPointer(cx, cy, true);
        auto desired = c.toggle_state();
        desired.hovered = true;
        desired.checked = !desired.checked;
        if (dispatch_toggle(c,
                            desired,
                            Bindings::WidgetOpKind::Toggle,
                            pointer)) {
            update.state_changed = true;
        }
        break;
    }
    case FocusTarget::Slider:
        // Slider activation is a no-op.
        break;
    case FocusTarget::List: {
        if (!c.ctx.layout.list || c.ctx.layout.list->item_bounds.empty()) {
            break;
        }
        int max_index = static_cast<int>(c.ctx.layout.list->item_bounds.size()) - 1;
        c.focus_list_index() = std::clamp(c.focus_list_index(), 0, max_index);
        auto desired = c.list_state();
        desired.hovered_index = c.focus_list_index();
        desired.selected_index = c.focus_list_index();
        auto center = list_item_center(c, c.focus_list_index());
        auto pointer = ProgrammaticPointer(center.first, center.second, true);
        if (dispatch_list(c,
                          desired,
                          Bindings::WidgetOpKind::ListActivate,
                          pointer,
                          c.focus_list_index(),
                          0.0f)) {
            update.state_changed = true;
        }
        break;
    }
    case FocusTarget::Tree: {
        auto result = TreeApplyOp(ctx, Bindings::WidgetOpKind::TreeToggle);
        update.state_changed |= result.state_changed;
        update.focus_changed |= result.focus_changed;
        break;
    }
    }

    return update;
}

auto MoveListFocus(WidgetInputContext& ctx, int direction) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    if (!c.ctx.layout.list || c.ctx.layout.list->item_bounds.empty()) {
        return update;
    }

    int max_index = static_cast<int>(c.ctx.layout.list->item_bounds.size()) - 1;
    if (c.focus_list_index() < 0) {
        c.focus_list_index() = c.list_state().selected_index >= 0
            ? c.list_state().selected_index
            : 0;
    }
    c.focus_list_index() = std::clamp(c.focus_list_index() + direction, 0, max_index);

    auto desired = c.list_state();
   desired.hovered_index = c.focus_list_index();
   desired.selected_index = c.focus_list_index();
   auto center = list_item_center(c, c.focus_list_index());
    auto pointer = ProgrammaticPointer(center.first, center.second, true);
    if (dispatch_list(c,
                      desired,
                      Bindings::WidgetOpKind::ListSelect,
                      pointer,
                      c.focus_list_index(),
                      0.0f)) {
        update.state_changed = true;
    }
    return update;
}

auto TreeApplyOp(WidgetInputContext& ctx, Bindings::WidgetOpKind op) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    if (!c.ctx.layout.tree) {
        return update;
    }
    if (!ensure_tree_focus_index(c)) {
        return update;
    }

    auto const& rows = c.ctx.layout.tree->rows;
    if (rows.empty()) {
        return update;
    }
    auto const& row = rows[static_cast<std::size_t>(c.focus_tree_index())];

    if ((op == Bindings::WidgetOpKind::TreeToggle
         || op == Bindings::WidgetOpKind::TreeExpand
         || op == Bindings::WidgetOpKind::TreeCollapse
         || op == Bindings::WidgetOpKind::TreeRequestLoad)
        && !row.expandable) {
        return update;
    }

    Widgets::TreeState desired = c.tree_state();
    desired.hovered_id = row.node_id;
    if (op == Bindings::WidgetOpKind::TreeSelect) {
        desired.selected_id = row.node_id;
    }

    auto center = tree_row_center(c, c.focus_tree_index());
    auto pointer = ProgrammaticPointer(center.first, center.second, true);
    if (dispatch_tree(c,
                      desired,
                      op,
                      row.node_id,
                      pointer,
                      0.0f)) {
        update.state_changed = true;
    }
    return update;
}

auto MoveTreeFocus(WidgetInputContext& ctx, int direction) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    if (!c.ctx.layout.tree) {
        return update;
    }
    if (!ensure_tree_focus_index(c)) {
        return update;
    }

    auto const& rows = c.ctx.layout.tree->rows;
    if (rows.empty()) {
        return update;
    }

    c.focus_tree_index() = std::clamp(c.focus_tree_index() + direction,
                                      0,
                                      static_cast<int>(rows.size()) - 1);
    auto const& row = rows[static_cast<std::size_t>(c.focus_tree_index())];
    Widgets::TreeState desired = c.tree_state();
    desired.hovered_id = row.node_id;
    desired.selected_id = row.node_id;
    auto center = tree_row_center(c, c.focus_tree_index());
    auto pointer = ProgrammaticPointer(center.first, center.second, true);
    if (dispatch_tree(c,
                      desired,
                      Bindings::WidgetOpKind::TreeSelect,
                      row.node_id,
                      pointer,
                      0.0f)) {
        update.state_changed = true;
    }
    return update;
}

auto AdjustSliderValue(WidgetInputContext& ctx, float delta) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    if (delta == 0.0f) {
        return update;
    }
    if (c.slider_range().maximum <= c.slider_range().minimum) {
        return update;
    }

    auto desired = c.slider_state();
    desired.hovered = true;
    desired.value = std::clamp(desired.value + delta,
                               c.slider_range().minimum,
                               c.slider_range().maximum);
    if (std::abs(desired.value - c.slider_state().value) <= 1e-6f) {
        return update;
    }

    auto thumb = slider_thumb_position(c, desired.value);
    auto pointer = ProgrammaticPointer(thumb.first, thumb.second, true);
    bool changed = false;
    changed |= dispatch_slider(c, desired, Bindings::WidgetOpKind::SliderUpdate, pointer);
    changed |= dispatch_slider(c, desired, Bindings::WidgetOpKind::SliderCommit, pointer);
    update.state_changed = changed;
    return update;
}

auto SliderStep(WidgetInputContext const& ctx,
                SliderStepOptions const& options) -> float {
    if (!ctx.slider_range) {
        return 0.0f;
    }
    WidgetInputContext copy = ctx;
    ContextAdapter c{copy};
    return slider_step_magnitude(c, options);
}

auto AdjustSliderByStep(WidgetInputContext& ctx,
                        int steps,
                        SliderStepOptions const& options) -> InputUpdate {
    if (steps == 0) {
        return {};
    }
    if (!ctx.slider_range || !ctx.slider_state) {
        return {};
    }
    float step = SliderStep(ctx, options);
    if (!(step > 0.0f) || !std::isfinite(step)) {
        return {};
    }
    float delta = static_cast<float>(steps) * step;
    if (delta == 0.0f) {
        return {};
    }
    return AdjustSliderValue(ctx, delta);
}

auto AdjustSliderAnalog(WidgetInputContext& ctx,
                        float axis_value,
                        SliderAnalogOptions const& options) -> InputUpdate {
    if (!ctx.slider_range || !ctx.slider_state) {
        return {};
    }
    if (!std::isfinite(axis_value)) {
        return {};
    }

    float axis = std::clamp(axis_value, -1.0f, 1.0f);
    float deadzone = std::clamp(options.deadzone, 0.0f, 0.99f);
    if (std::abs(axis) <= deadzone) {
        return {};
    }

    float step = SliderStep(ctx, options.step_options);
    if (!(step > 0.0f) || !std::isfinite(step)) {
        return {};
    }

    float scale = options.scale;
    if (!std::isfinite(scale) || scale <= 0.0f) {
        scale = 1.0f;
    }

    float normalized = (std::abs(axis) - deadzone) / (1.0f - deadzone);
    float delta = normalized * scale * step;
    if (axis < 0.0f) {
        delta = -delta;
    }

    if (delta == 0.0f) {
        return {};
    }
    return AdjustSliderValue(ctx, delta);
}

auto HandlePointerMove(WidgetInputContext& ctx, float x, float y) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    bool changed = false;

    c.pointer_x() = x;
    c.pointer_y() = y;

    auto const& layout = c.layout();

    // Button hover logic.
    bool inside_button = layout.button.contains(x, y);
    if (!c.pointer_down()) {
        if (inside_button != c.button_state().hovered) {
            auto desired = c.button_state();
            desired.hovered = inside_button;
            auto op = inside_button ? Bindings::WidgetOpKind::HoverEnter
                                    : Bindings::WidgetOpKind::HoverExit;
            auto pointer = make_pointer_info(c.ctx, inside_button);
            if (dispatch_button(c, desired, op, pointer)) {
                changed = true;
            }
        }
    } else if (c.button_state().pressed && !inside_button && c.button_state().hovered) {
        auto desired = c.button_state();
        desired.hovered = false;
        auto pointer = make_pointer_info(c.ctx, false);
        if (dispatch_button(c, desired, Bindings::WidgetOpKind::HoverExit, pointer)) {
            changed = true;
        }
    }

    // Toggle hover logic.
    bool inside_toggle = layout.toggle.contains(x, y);
    if (inside_toggle != c.toggle_state().hovered) {
        auto desired = c.toggle_state();
        desired.hovered = inside_toggle;
        auto op = inside_toggle ? Bindings::WidgetOpKind::HoverEnter
                                : Bindings::WidgetOpKind::HoverExit;
        auto pointer = make_pointer_info(c.ctx, inside_toggle);
        if (dispatch_toggle(c, desired, op, pointer)) {
            changed = true;
        }
    }

    // List hover logic.
    if (c.ctx.layout.list) {
        auto const& list_layout = *c.ctx.layout.list;
        bool inside_list = list_layout.bounds.contains(x, y);
        int hover_index = inside_list ? list_index_from_position(c, y) : -1;
        if (hover_index != c.list_state().hovered_index) {
            auto desired = c.list_state();
            desired.hovered_index = hover_index;
            auto pointer = make_pointer_info(c.ctx, inside_list);
            if (dispatch_list(c,
                              desired,
                              Bindings::WidgetOpKind::ListHover,
                              pointer,
                              hover_index,
                              0.0f)) {
                changed = true;
            }
        }
        if (hover_index >= 0) {
            c.focus_list_index() = hover_index;
        }
    }

    // Tree hover logic.
    if (c.ctx.layout.tree) {
        auto const& tree_layout = *c.ctx.layout.tree;
        bool inside_tree = tree_layout.bounds.contains(x, y);
        int tree_index = inside_tree ? tree_row_index_from_position(c, y) : -1;
        std::string hovered_id;
        if (tree_index >= 0 && static_cast<std::size_t>(tree_index) < tree_layout.rows.size()) {
            hovered_id = tree_layout.rows[static_cast<std::size_t>(tree_index)].node_id;
        }

        if (hovered_id != c.tree_state().hovered_id) {
            auto desired = c.tree_state();
            desired.hovered_id = hovered_id;
            auto pointer = make_pointer_info(c.ctx, inside_tree);
            if (dispatch_tree(c,
                              desired,
                              Bindings::WidgetOpKind::TreeHover,
                              hovered_id,
                              pointer,
                              0.0f)) {
                changed = true;
            }
        }
        if (tree_index >= 0) {
            c.focus_tree_index() = tree_index;
        }
    }

    // Slider hover/drag logic.
    if (c.ctx.layout.slider) {
        auto const& slider_layout = *c.ctx.layout.slider;
        bool inside_slider = slider_layout.bounds.contains(x, y);
        if (c.slider_dragging()) {
            auto desired = c.slider_state();
            desired.dragging = true;
            desired.hovered = inside_slider;
            desired.value = slider_value_from_position(c, x);
            auto pointer = make_pointer_info(c.ctx, desired.hovered);
            if (dispatch_slider(c,
                                desired,
                                Bindings::WidgetOpKind::SliderUpdate,
                                pointer)) {
                changed = true;
            }
        } else if (inside_slider != c.slider_state().hovered) {
            auto desired = c.slider_state();
            desired.hovered = inside_slider;
            auto op = inside_slider ? Bindings::WidgetOpKind::HoverEnter
                                    : Bindings::WidgetOpKind::HoverExit;
            auto pointer = make_pointer_info(c.ctx, inside_slider);
            if (dispatch_slider(c, desired, op, pointer)) {
                changed = true;
            }
        }
    }

    update.state_changed = changed;
    return update;
}

auto HandlePointerDown(WidgetInputContext& ctx) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    bool changed = false;

    c.pointer_down() = true;
    c.tree_pointer_down_id().clear();
    c.tree_pointer_toggle() = false;

    auto const& layout = c.layout();

    if (layout.button.contains(c.pointer_x(), c.pointer_y())) {
        auto desired = c.button_state();
        desired.hovered = true;
        desired.pressed = true;
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_button(c, desired, Bindings::WidgetOpKind::Press, pointer)) {
            changed = true;
        }
    }

    if (layout.toggle.contains(c.pointer_x(), c.pointer_y())) {
        auto desired = c.toggle_state();
        desired.hovered = true;
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_toggle(c, desired, Bindings::WidgetOpKind::Press, pointer)) {
            changed = true;
        }
    }

    if (c.ctx.layout.slider && c.ctx.layout.slider->bounds.contains(c.pointer_x(), c.pointer_y())) {
        c.slider_dragging() = true;
        auto desired = c.slider_state();
        desired.dragging = true;
        desired.hovered = true;
        desired.value = slider_value_from_position(c, c.pointer_x());
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_slider(c, desired, Bindings::WidgetOpKind::SliderBegin, pointer)) {
            changed = true;
        }
    }

    if (c.ctx.layout.list && c.ctx.layout.list->bounds.contains(c.pointer_x(), c.pointer_y())) {
        int index = list_index_from_position(c, c.pointer_y());
        auto desired = c.list_state();
        desired.hovered_index = index;
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_list(c,
                          desired,
                          Bindings::WidgetOpKind::ListHover,
                          pointer,
                          index,
                          0.0f)) {
            changed = true;
        }
        if (index >= 0) {
            c.focus_list_index() = index;
        }
    }

    if (c.ctx.layout.tree && c.ctx.layout.tree->bounds.contains(c.pointer_x(), c.pointer_y())) {
        int index = tree_row_index_from_position(c, c.pointer_y());
        if (index >= 0) {
            auto const& rows = c.ctx.layout.tree->rows;
            if (static_cast<std::size_t>(index) < rows.size()) {
                c.focus_tree_index() = index;
                auto const& row = rows[static_cast<std::size_t>(index)];
                auto desired = c.tree_state();
                desired.hovered_id = row.node_id;
                c.tree_pointer_down_id() = row.node_id;
                c.tree_pointer_toggle() = tree_toggle_contains(c, index, c.pointer_x(), c.pointer_y());
                auto pointer = make_pointer_info(c.ctx, true);
                if (dispatch_tree(c,
                                  desired,
                                  Bindings::WidgetOpKind::TreeHover,
                                  row.node_id,
                                  pointer,
                                  0.0f)) {
                    changed = true;
                }
            }
        }
    }

    if (RefreshFocusTargetFromSpace(ctx)) {
        update.focus_changed = true;
    }

    update.state_changed = changed;
    return update;
}

auto HandlePointerUp(WidgetInputContext& ctx) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    bool changed = false;

    auto const& layout = c.layout();

    bool inside_button = layout.button.contains(c.pointer_x(), c.pointer_y());
    if (c.button_state().pressed) {
        auto desired = c.button_state();
        desired.pressed = false;
        desired.hovered = inside_button;
        auto release_pointer = make_pointer_info(c.ctx, inside_button);
        if (dispatch_button(c,
                            desired,
                            Bindings::WidgetOpKind::Release,
                            release_pointer)) {
            changed = true;
        }
        if (inside_button) {
            auto activate_pointer = make_pointer_info(c.ctx, true);
            if (dispatch_button(c,
                                desired,
                                Bindings::WidgetOpKind::Activate,
                                activate_pointer)) {
                changed = true;
            }
        }
    }

    bool inside_toggle = layout.toggle.contains(c.pointer_x(), c.pointer_y());
    if (inside_toggle) {
        auto desired = c.toggle_state();
        desired.hovered = true;
        desired.checked = !desired.checked;
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_toggle(c,
                            desired,
                            Bindings::WidgetOpKind::Toggle,
                            pointer)) {
            changed = true;
        }
    }

    if (c.slider_dragging()) {
        c.slider_dragging() = false;
        if (c.ctx.layout.slider) {
            bool inside_slider = c.ctx.layout.slider->bounds.contains(c.pointer_x(), c.pointer_y());
            auto desired = c.slider_state();
            desired.dragging = false;
            desired.hovered = inside_slider;
            desired.value = slider_value_from_position(c, c.pointer_x());
            auto pointer = make_pointer_info(c.ctx, inside_slider);
            if (dispatch_slider(c,
                                desired,
                                Bindings::WidgetOpKind::SliderCommit,
                                pointer)) {
                changed = true;
            }
        }
    }

    if (c.ctx.layout.list && c.ctx.layout.list->bounds.contains(c.pointer_x(), c.pointer_y())) {
        int index = list_index_from_position(c, c.pointer_y());
        if (index >= 0) {
            auto desired = c.list_state();
            desired.selected_index = index;
            auto pointer = make_pointer_info(c.ctx, true);
            if (dispatch_list(c,
                              desired,
                              Bindings::WidgetOpKind::ListSelect,
                              pointer,
                              index,
                              0.0f)) {
                changed = true;
            }
            if (dispatch_list(c,
                              desired,
                              Bindings::WidgetOpKind::ListActivate,
                              pointer,
                              index,
                              0.0f)) {
                changed = true;
            }
            c.focus_list_index() = index;
        }
    }

    if (c.ctx.layout.tree && c.ctx.layout.tree->bounds.contains(c.pointer_x(), c.pointer_y())) {
        int tree_index = tree_row_index_from_position(c, c.pointer_y());
        if (!c.tree_pointer_down_id().empty()
            && tree_index >= 0
            && static_cast<std::size_t>(tree_index) < c.ctx.layout.tree->rows.size()) {
            auto const& row = c.ctx.layout.tree->rows[static_cast<std::size_t>(tree_index)];
            if (row.node_id == c.tree_pointer_down_id()) {
                auto desired = c.tree_state();
                desired.hovered_id = row.node_id;
                desired.selected_id = row.node_id;
                if (c.tree_pointer_toggle()) {
                    auto pointer = make_pointer_info(c.ctx, true);
                    if (dispatch_tree(c,
                                      desired,
                                      Bindings::WidgetOpKind::TreeToggle,
                                      row.node_id,
                                      pointer,
                                      0.0f)) {
                        changed = true;
                    }
                }
                auto pointer = make_pointer_info(c.ctx, true);
                if (dispatch_tree(c,
                                  desired,
                                  Bindings::WidgetOpKind::TreeSelect,
                                  row.node_id,
                                  pointer,
                                  0.0f)) {
                    changed = true;
                }
                c.focus_tree_index() = tree_index;
            }
        }
    }

    c.pointer_down() = false;
    c.tree_pointer_down_id().clear();
    c.tree_pointer_toggle() = false;

    if (RefreshFocusTargetFromSpace(ctx)) {
        update.focus_changed = true;
    }

    update.state_changed = changed;
    return update;
}

auto HandlePointerWheel(WidgetInputContext& ctx, int wheel_delta) -> InputUpdate {
    ContextAdapter c{ctx};
    InputUpdate update{};
    if (wheel_delta == 0) {
        return update;
    }

    bool changed = false;

    if (c.ctx.layout.list && c.ctx.layout.list->bounds.contains(c.pointer_x(), c.pointer_y())) {
        auto const& list_layout = *c.ctx.layout.list;
        float scroll_pixels = static_cast<float>(-wheel_delta) * (list_layout.item_height * 0.25f);
        auto desired = c.list_state();
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_list(c,
                          desired,
                          Bindings::WidgetOpKind::ListScroll,
                          pointer,
                          c.list_state().hovered_index,
                          scroll_pixels)) {
            changed = true;
        }
    }

    if (c.ctx.layout.tree && c.ctx.layout.tree->bounds.contains(c.pointer_x(), c.pointer_y())) {
        auto const& tree_layout = *c.ctx.layout.tree;
        float scroll_pixels = static_cast<float>(-wheel_delta) * (tree_layout.row_height * 0.25f);
        auto desired = c.tree_state();
        auto pointer = make_pointer_info(c.ctx, true);
        if (dispatch_tree(c,
                          desired,
                          Bindings::WidgetOpKind::TreeScroll,
                          c.tree_state().hovered_id,
                          pointer,
                          scroll_pixels)) {
            changed = true;
        }
    }

    update.state_changed = changed;
    return update;
}

} // namespace SP::UI::Builders::Widgets::Input
