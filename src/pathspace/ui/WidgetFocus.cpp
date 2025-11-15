#include "WidgetDetail.hpp"
#include "declarative/widgets/Common.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <pathspace/path/ConcretePath.hpp>

namespace SP::UI::Builders::Widgets {

using namespace Detail;

namespace {

auto mark_declarative_focus_dirty(PathSpace& space,
                                  std::string const& widget_root) -> void {
    auto dirty = read_optional<bool>(space, widget_root + "/render/dirty");
    if (!dirty) {
        return;
    }
    if (!dirty->has_value()) {
        return;
    }
    (void)SP::UI::Declarative::Detail::mark_render_dirty(space, widget_root);
}

auto determine_widget_kind(PathSpace& space,
                           std::string const& rootPath) -> SP::Expected<WidgetKind>;

struct FocusScope {
    std::string app_root;
    std::string widgets_root;
    std::optional<std::string> window_component;
};

auto make_focus_scope(SP::App::AppRootPathView app_root,
                      std::string const& widget_root) -> SP::Expected<FocusScope> {
    FocusScope scope{
        .app_root = std::string(app_root.getPath()),
        .widgets_root = std::string(app_root.getPath()) + "/widgets",
        .window_component = std::nullopt,
    };

    if (widget_root.find("/windows/") == std::string::npos) {
        return scope;
    }

    auto window_root = derive_window_root_for(widget_root);
    if (!window_root) {
        return std::unexpected(window_root.error());
    }
    scope.widgets_root = window_root->getPath() + "/widgets";

    auto component = window_component_for(widget_root);
    if (!component) {
        return std::unexpected(component.error());
    }
    scope.window_component = *component;
    return scope;
}

auto make_focus_scope_for_window(SP::App::AppRootPathView app_root,
                                 WindowPath const& window_path) -> SP::Expected<FocusScope> {
    FocusScope scope{
        .app_root = std::string(app_root.getPath()),
        .widgets_root = std::string(window_path.getPath()) + "/widgets",
        .window_component = std::nullopt,
    };
    auto component = window_component_for(window_path.getPath());
    if (!component) {
        return std::unexpected(component.error());
    }
    scope.window_component = *component;
    return scope;
}

auto widget_footprint_path(std::string const& widget_root) -> std::string {
    return widget_root + "/meta/footprint";
}

auto is_focusable_kind(WidgetKind kind) -> bool {
    switch (kind) {
    case WidgetKind::Button:
    case WidgetKind::Toggle:
    case WidgetKind::Slider:
    case WidgetKind::List:
    case WidgetKind::Tree:
    case WidgetKind::TextField:
    case WidgetKind::TextArea:
    case WidgetKind::InputField:
    case WidgetKind::PaintSurface:
        return true;
    case WidgetKind::Stack:
    case WidgetKind::Label:
        return false;
    }
    return false;
}

auto is_focusable_widget(PathSpace& space,
                         std::string const& widget_root,
                         WidgetKind kind) -> SP::Expected<bool> {
    auto disabled = read_optional<bool>(space, widget_root + "/focus/disabled");
    if (!disabled) {
        return std::unexpected(disabled.error());
    }
    if (disabled->value_or(false)) {
        return false;
    }
    return is_focusable_kind(kind);
}

auto set_widget_focus_flag(PathSpace& space,
                           std::string const& widget_root,
                           bool focused) -> SP::Expected<void> {
    auto path = widget_root + "/focus/current";
    auto existing = read_optional<bool>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value() && **existing == focused) {
        return {};
    }
    if (auto status = replace_single<bool>(space, path, focused); !status) {
        return std::unexpected(status.error());
    }
    mark_declarative_focus_dirty(space, widget_root);
    return {};
}

auto update_window_focus_nodes(PathSpace& space,
                               FocusScope const& scope,
                               std::optional<std::string> const& widget_path) -> void {
    std::optional<std::string> window_component = scope.window_component;
    if (!window_component.has_value() && widget_path.has_value()) {
        auto derived = window_component_for(*widget_path);
        if (derived) {
            window_component = *derived;
        }
    }
    if (!window_component.has_value()) {
        return;
    }
    std::string scenes_root = scope.app_root + "/scenes";
    auto scenes = space.listChildren(SP::ConcretePathStringView{scenes_root});
    for (auto const& scene : scenes) {
        auto focus_path = scenes_root + "/" + scene + "/structure/window/" + *window_component + "/focus/current";
        auto existing = read_optional<std::string>(space, focus_path);
        if (!existing) {
            continue;
        }
        auto value = widget_path.value_or(std::string{});
#if 0
        std::cerr << "[focus] update " << focus_path << " <= '" << value << "'" << std::endl;
#endif
        (void)replace_single<std::string>(space, focus_path, value);
    }
}

auto collect_focus_order(PathSpace& space,
                         std::string const& widget_root,
                         std::vector<WidgetPath>& order) -> SP::Expected<void> {
    auto kind = determine_widget_kind(space, widget_root);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    auto focusable = is_focusable_widget(space, widget_root, *kind);
    if (!focusable) {
        return std::unexpected(focusable.error());
    }
    if (*focusable) {
        order.emplace_back(widget_root);
    }

    auto children_root = widget_root + "/children";
    auto children = space.listChildren(SP::ConcretePathStringView{children_root});
    for (auto const& child : children) {
        auto status = collect_focus_order(space, children_root + "/" + child, order);
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    return {};
}

auto build_focus_order(PathSpace& space,
                       FocusScope const& scope) -> SP::Expected<std::vector<WidgetPath>> {
    std::vector<WidgetPath> order;
    auto roots = space.listChildren(SP::ConcretePathStringView{scope.widgets_root});
    for (auto const& name : roots) {
        auto root = scope.widgets_root + "/" + name;
        auto status = collect_focus_order(space, root, order);
        if (!status) {
            return std::unexpected(status.error());
        }
    }

    for (std::size_t i = 0; i < order.size(); ++i) {
        auto order_path = std::string(order[i].getPath()) + "/focus/order";
        (void)replace_single<std::uint32_t>(space, order_path, static_cast<std::uint32_t>(i));
    }
    return order;
}

auto ensure_focus_order(PathSpace& space,
                        FocusScope const& scope) -> SP::Expected<void> {
    auto order = build_focus_order(space, scope);
    if (!order) {
        return std::unexpected(order.error());
    }
    return {};
}

auto read_widget_footprint(PathSpace& space,
                           std::string const& widget_root) -> SP::Expected<std::optional<DirtyRectHint>> {
    auto footprint = read_optional<DirtyRectHint>(space, widget_footprint_path(widget_root));
    if (!footprint) {
        return std::unexpected(footprint.error());
    }
    if (!footprint->has_value()) {
        return std::optional<DirtyRectHint>{};
    }
    DirtyRectHint hint = ensure_valid_hint(**footprint);
    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return std::optional<DirtyRectHint>{};
    }
    return std::optional<DirtyRectHint>{hint};
}

auto expand_focus_dirty_hint(DirtyRectHint hint) -> DirtyRectHint {
    float padding = Input::FocusHighlightPadding();
    DirtyRectHint expanded{};
    expanded.min_x = std::max(0.0f, hint.min_x - padding);
    expanded.min_y = std::max(0.0f, hint.min_y - padding);
    expanded.max_x = hint.max_x + padding;
    expanded.max_y = hint.max_y + padding;
    return ensure_valid_hint(expanded);
}

auto append_unique_hint(std::vector<DirtyRectHint>& hints,
                        DirtyRectHint const& hint) -> void {
    auto it = std::find_if(hints.begin(), hints.end(), [&](DirtyRectHint const& existing) {
        return existing.min_x == hint.min_x
            && existing.min_y == hint.min_y
            && existing.max_x == hint.max_x
            && existing.max_y == hint.max_y;
    });
    if (it == hints.end()) {
        hints.push_back(hint);
    }
}


auto widget_name_from_root(std::string const& app_root,
                           std::string const& widget_root) -> SP::Expected<std::string> {
    auto prefix = app_root + "/widgets/";
    if (widget_root.rfind(prefix, 0) == 0) {
        auto name = widget_root.substr(prefix.size());
        if (name.empty()) {
            return std::unexpected(make_error("widget path missing identifier", SP::Error::Code::InvalidPath));
        }
        return name;
    }

    auto windows_prefix = app_root + "/windows/";
    if (widget_root.rfind(windows_prefix, 0) != 0) {
        return std::unexpected(make_error("widget path must belong to app widgets subtree",
                                          SP::Error::Code::InvalidPath));
    }
    auto widgets_pos = widget_root.find("/widgets/", windows_prefix.size());
    if (widgets_pos == std::string::npos) {
        return std::unexpected(make_error("widget path missing '/widgets' segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto name = widget_root.substr(widgets_pos + std::strlen("/widgets/"));
    if (name.empty()) {
        return std::unexpected(make_error("widget path missing identifier", SP::Error::Code::InvalidPath));
    }
    return name;
}

auto widget_scene_path(std::string const& app_root,
                       std::string const& widget_name) -> std::string {
    return app_root + "/scenes/widgets/" + widget_name;
}

auto focus_config_path(std::string const& app_root) -> std::string {
    return app_root + "/widgets/focus/config";
}

auto pulsing_highlight_path(std::string const& app_root) -> std::string {
    return focus_config_path(app_root) + "/pulsingHighlight";
}

auto read_pulsing_highlight(PathSpace& space,
                            std::string const& app_root) -> SP::Expected<bool> {
    auto existing = read_optional<bool>(space, pulsing_highlight_path(app_root));
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return false;
    }
    return **existing;
}

auto write_pulsing_highlight(PathSpace& space,
                             std::string const& app_root,
                             bool enabled) -> SP::Expected<void> {
    return replace_single<bool>(space, pulsing_highlight_path(app_root), enabled);
}

auto determine_widget_kind(PathSpace& space,
                           std::string const& rootPath) -> SP::Expected<WidgetKind> {
    auto kindPath = rootPath + "/meta/kind";
    auto kindValue = read_optional<std::string>(space, kindPath);
    if (!kindValue) {
        return std::unexpected(kindValue.error());
    }
    if (kindValue->has_value()) {
        std::string_view kind = **kindValue;
        if (kind == "button") {
            return WidgetKind::Button;
        }
        if (kind == "toggle") {
            return WidgetKind::Toggle;
        }
        if (kind == "slider") {
            return WidgetKind::Slider;
        }
        if (kind == "list") {
            return WidgetKind::List;
        }
        if (kind == "tree") {
            return WidgetKind::Tree;
        }
        if (kind == "stack") {
            return WidgetKind::Stack;
        }
        if (kind == "text_field") {
            return WidgetKind::TextField;
        }
        if (kind == "text_area") {
            return WidgetKind::TextArea;
        }
        if (kind == "label") {
            return WidgetKind::Label;
        }
        if (kind == "input_field" || kind == "input" || kind == "text_input") {
            return WidgetKind::InputField;
        }
        if (kind == "paint_surface") {
            return WidgetKind::PaintSurface;
        }
    }

    auto computedPath = rootPath + "/layout/computed";
    auto computedValue = read_optional<Widgets::StackLayoutState>(space, computedPath);
    if (!computedValue) {
        return std::unexpected(computedValue.error());
    }
    if (computedValue->has_value()) {
        return WidgetKind::Stack;
    }

    auto nodesPath = rootPath + "/meta/nodes";
    auto nodesValue = read_optional<std::vector<Widgets::TreeNode>>(space, nodesPath);
    if (!nodesValue) {
        return std::unexpected(nodesValue.error());
    }
    if (nodesValue->has_value()) {
        return WidgetKind::Tree;
    }

    auto itemsPath = rootPath + "/meta/items";
    auto itemsValue = read_optional<std::vector<Widgets::ListItem>>(space, itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    if (itemsValue->has_value()) {
        return WidgetKind::List;
    }

    auto rangePath = rootPath + "/meta/range";
    auto rangeValue = read_optional<Widgets::SliderRange>(space, rangePath);
    if (!rangeValue) {
        return std::unexpected(rangeValue.error());
    }
    if (rangeValue->has_value()) {
        return WidgetKind::Slider;
    }

    auto labelPath = rootPath + "/meta/label";
    auto labelValue = read_optional<std::string>(space, labelPath);
    if (!labelValue) {
        return std::unexpected(labelValue.error());
    }
    if (labelValue->has_value()) {
        return WidgetKind::Button;
    }

    return WidgetKind::Toggle;
}

auto update_text_field_focus(PathSpace& space,
                             std::string const& widget_root,
                             std::string const& app_root,
                             bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::TextFieldState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }

    Widgets::TextFieldState desired = *stateValue;
    desired.hovered = focused;
    desired.focused = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::TextFieldPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
    };

    auto updated = Widgets::UpdateTextFieldState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_text_area_focus(PathSpace& space,
                            std::string const& widget_root,
                            std::string const& app_root,
                            bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::TextAreaState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }

    Widgets::TextAreaState desired = *stateValue;
    desired.hovered = focused;
    desired.focused = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::TextAreaPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
    };

    auto updated = Widgets::UpdateTextAreaState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_button_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ButtonState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ButtonState desired = *stateValue;
    desired.hovered = focused;
    desired.focused = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::ButtonPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .label = ConcretePath{widget_root + "/meta/label"},
    };
    auto updated = Widgets::UpdateButtonState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_toggle_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ToggleState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ToggleState desired = *stateValue;
    desired.hovered = focused;
    desired.focused = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::TogglePaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
    };
    auto updated = Widgets::UpdateToggleState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_slider_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::SliderState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::SliderState desired = *stateValue;
    desired.hovered = focused;
    desired.focused = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::SliderPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .range = ConcretePath{widget_root + "/meta/range"},
    };
    auto updated = Widgets::UpdateSliderState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_list_focus(PathSpace& space,
                       std::string const& widget_root,
                       std::string const& app_root,
                       bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ListState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ListState desired = *stateValue;
    desired.focused = focused;

    auto itemsPath = widget_root + "/meta/items";
    auto itemsValue = space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    auto const& items = *itemsValue;
    if (focused) {
        if (!items.empty()) {
            auto max_index = static_cast<std::int32_t>(items.size()) - 1;
            auto hovered = desired.hovered_index;
            if (hovered < 0 || hovered > max_index) {
                if (desired.selected_index >= 0 && desired.selected_index <= max_index) {
                    hovered = desired.selected_index;
                } else {
                    hovered = 0;
                }
            }
            desired.hovered_index = hovered;
        } else {
            desired.hovered_index = -1;
        }
    } else {
        desired.hovered_index = -1;
    }

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::ListPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .items = ConcretePath{itemsPath},
    };
    auto updated = Widgets::UpdateListState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_tree_focus(PathSpace& space,
                       std::string const& widget_root,
                       std::string const& app_root,
                       bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::TreeState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }

    auto nodesPath = widget_root + "/meta/nodes";
    auto nodesValue = space.read<std::vector<Widgets::TreeNode>, std::string>(nodesPath);
    if (!nodesValue) {
        return std::unexpected(nodesValue.error());
    }

    Widgets::TreeState desired = *stateValue;
    desired.focused = focused;
    if (focused) {
        if (desired.hovered_id.empty()) {
            if (!desired.selected_id.empty()) {
                desired.hovered_id = desired.selected_id;
            } else {
                auto it = std::find_if(nodesValue->begin(), nodesValue->end(), [](Widgets::TreeNode const& node) {
                    return node.enabled;
                });
                if (it != nodesValue->end()) {
                    desired.hovered_id = it->id;
                }
            }
        }
    } else {
        desired.hovered_id.clear();
    }

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::TreePaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .nodes = ConcretePath{nodesPath},
    };
    auto updated = Widgets::UpdateTreeState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_widget_focus(PathSpace& space,
                         std::string const& widget_root,
                         bool focused) -> SP::Expected<bool> {
    auto appRootPath = derive_app_root_for(ConcretePathView{widget_root});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto kind = determine_widget_kind(space, widget_root);
    if (!kind) {
        return std::unexpected(kind.error());
    }

    auto const& app_root = appRootPath->getPath();
    switch (*kind) {
        case WidgetKind::Button:
            return update_button_focus(space, widget_root, app_root, focused);
        case WidgetKind::Toggle:
            return update_toggle_focus(space, widget_root, app_root, focused);
        case WidgetKind::Slider:
            return update_slider_focus(space, widget_root, app_root, focused);
        case WidgetKind::List:
            return update_list_focus(space, widget_root, app_root, focused);
        case WidgetKind::Stack:
            return false;
        case WidgetKind::Tree:
            return update_tree_focus(space, widget_root, app_root, focused);
        case WidgetKind::TextField:
            return update_text_field_focus(space, widget_root, app_root, focused);
        case WidgetKind::TextArea:
            return update_text_area_focus(space, widget_root, app_root, focused);
        case WidgetKind::Label:
            return false;
        case WidgetKind::InputField:
            return update_text_field_focus(space, widget_root, app_root, focused);
        case WidgetKind::PaintSurface:
            return false;
    }
    return std::unexpected(make_error("unknown widget kind", SP::Error::Code::InvalidType));
}

} // namespace

namespace Focus {

using namespace Detail;


auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath {
    return ConcretePath{std::string(appRoot.getPath()) + "/widgets/focus/current"};
}

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target,
                std::optional<bool> pulsing_highlight) -> Config {
    bool desired_pulsing = pulsing_highlight.value_or(true);
    Config config{
        .focus_state = FocusStatePath(appRoot),
        .auto_render_target = std::move(auto_render_target),
        .pulsing_highlight = std::optional<bool>{desired_pulsing},
    };
    return config;
}

auto Current(PathSpace const& space,
             ConcretePathView focus_state) -> SP::Expected<std::optional<std::string>> {
    std::string path{focus_state.getPath()};
    auto existing = read_optional<std::string>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return std::optional<std::string>{};
    }
    auto const& value = **existing;
    if (value.empty()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{value};
}

auto set_focus_string(PathSpace& space,
                      ConcretePathView focus_state,
                      std::string const& value) -> SP::Expected<void> {
    std::string path{focus_state.getPath()};
    if (auto status = replace_single<std::string>(space, path, value); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto maybe_schedule_focus_render(PathSpace& space,
                                 Config const& config,
                                 bool changed) -> SP::Expected<void> {
    if (!changed) {
        return {};
    }
    if (!config.auto_render_target.has_value()) {
        return {};
    }
    return enqueue_auto_render_event(space,
                                     config.auto_render_target->getPath(),
                                     "focus-navigation",
                                     0);
}

auto Set(PathSpace& space,
         Config const& config,
         WidgetPath const& widget) -> SP::Expected<UpdateResult> {
    std::string target_path{widget.getPath()};
    auto app_root_path = derive_app_root_for(ConcretePathView{target_path});
    if (!app_root_path) {
        return std::unexpected(app_root_path.error());
    }
    auto app_root_view = SP::App::AppRootPathView{app_root_path->getPath()};
    auto scope = make_focus_scope(app_root_view, target_path);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    if (auto ensured = ensure_focus_order(space, *scope); !ensured) {
        return std::unexpected(ensured.error());
    }
    if (config.pulsing_highlight.has_value()) {
        if (auto status = SetPulsingHighlight(space, app_root_view, *config.pulsing_highlight); !status) {
            return std::unexpected(status.error());
        }
    }
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    auto previous = *current;

    std::vector<DirtyRectHint> dirty_hints;
    auto append_dirty_hint = [&](std::string const& widget_root) -> SP::Expected<void> {
        if (!config.auto_render_target.has_value()) {
            return {};
        }
        auto footprint = read_widget_footprint(space, widget_root);
        if (!footprint) {
            return std::unexpected(footprint.error());
        }
        if (!footprint->has_value()) {
            return {};
        }
        DirtyRectHint expanded = expand_focus_dirty_hint(**footprint);
        if (expanded.max_x <= expanded.min_x || expanded.max_y <= expanded.min_y) {
            return {};
        }
        append_unique_hint(dirty_hints, expanded);
        return {};
    };

    auto apply_focus = update_widget_focus(space, target_path, true);
    if (!apply_focus) {
        return std::unexpected(apply_focus.error());
    }
    auto set_flag = set_widget_focus_flag(space, target_path, true);
    if (!set_flag) {
        return std::unexpected(set_flag.error());
    }
    bool changed = *apply_focus;
    bool mark_new_dirty = *apply_focus;
    bool mark_prev_dirty = false;

    if (!previous.has_value() || *previous != target_path) {
        if (previous.has_value()) {
            auto prev_scope = make_focus_scope(app_root_view, *previous);
            if (!prev_scope) {
                return std::unexpected(prev_scope.error());
            }
            auto clear_prev = update_widget_focus(space, *previous, false);
            if (!clear_prev) {
                return std::unexpected(clear_prev.error());
            }
            changed = changed || *clear_prev;
            mark_prev_dirty = true;
            auto clear_flag = set_widget_focus_flag(space, *previous, false);
            if (!clear_flag) {
                return std::unexpected(clear_flag.error());
            }
            update_window_focus_nodes(space, *prev_scope, std::nullopt);
        }
        if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, target_path); !status) {
            return std::unexpected(status.error());
        }
        changed = true;
        mark_new_dirty = true;
    }

    update_window_focus_nodes(space, *scope, target_path);

    if (mark_new_dirty) {
        if (auto status = append_dirty_hint(target_path); !status) {
            return std::unexpected(status.error());
        }
    }
    if (mark_prev_dirty && previous.has_value()) {
        if (auto status = append_dirty_hint(*previous); !status) {
            return std::unexpected(status.error());
        }
    }

    if (!dirty_hints.empty() && config.auto_render_target.has_value()) {
        auto submit = Renderer::SubmitDirtyRects(space,
                                                 SP::ConcretePathStringView{config.auto_render_target->getPath()},
                                                 std::span<DirtyRectHint const>(dirty_hints.data(),
                                                                                dirty_hints.size()));
        if (!submit) {
            return std::unexpected(submit.error());
        }
    }

    if (auto status = maybe_schedule_focus_render(space, config, changed); !status) {
        return std::unexpected(status.error());
    }

    return UpdateResult{
        .widget = widget,
        .changed = changed,
    };
}

auto Clear(PathSpace& space,
           Config const& config) -> SP::Expected<bool> {
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    if (!current->has_value()) {
        return false;
    }

    auto app_root_path = derive_app_root_for(ConcretePathView{**current});
    if (!app_root_path) {
        return std::unexpected(app_root_path.error());
    }
    auto app_root_view = SP::App::AppRootPathView{app_root_path->getPath()};
    auto scope = make_focus_scope(app_root_view, **current);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    if (config.pulsing_highlight.has_value()) {
        if (auto status = SetPulsingHighlight(space, app_root_view, *config.pulsing_highlight); !status) {
            return std::unexpected(status.error());
        }
    }

    std::vector<DirtyRectHint> dirty_hints;
    auto append_dirty_hint = [&](std::string const& widget_root) -> SP::Expected<void> {
    if (!config.auto_render_target.has_value()) {
        return {};
    }
    auto footprint = read_widget_footprint(space, widget_root);
    if (!footprint) {
        return std::unexpected(footprint.error());
    }
    if (!footprint->has_value()) {
        return {};
    }
    DirtyRectHint expanded = expand_focus_dirty_hint(**footprint);
    if (expanded.max_x <= expanded.min_x || expanded.max_y <= expanded.min_y) {
        return {};
    }
    append_unique_hint(dirty_hints, expanded);
    return {};
};

    bool changed = false;
    auto clear_prev = update_widget_focus(space, **current, false);
    if (!clear_prev) {
        return std::unexpected(clear_prev.error());
    }
    auto clear_flag = set_widget_focus_flag(space, **current, false);
    if (!clear_flag) {
        return std::unexpected(clear_flag.error());
    }
    update_window_focus_nodes(space, *scope, std::nullopt);
    changed = *clear_prev;
    if (auto status = append_dirty_hint(**current); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, std::string{}); !status) {
        return std::unexpected(status.error());
    }
    changed = true;

    if (!dirty_hints.empty() && config.auto_render_target.has_value()) {
        auto submit = Renderer::SubmitDirtyRects(space,
                                                 SP::ConcretePathStringView{config.auto_render_target->getPath()},
                                                 std::span<DirtyRectHint const>(dirty_hints.data(),
                                                                                dirty_hints.size()));
        if (!submit) {
            return std::unexpected(submit.error());
        }
    }

    if (auto status = maybe_schedule_focus_render(space, config, true); !status) {
        return std::unexpected(status.error());
    }

    return changed;
}

auto Move(PathSpace& space,
          Config const& config,
          std::span<WidgetPath const> order,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>> {
    if (order.empty()) {
        return std::optional<UpdateResult>{};
    }

    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    auto current_value = current->value_or(std::string{});

    std::size_t next_index = 0;
    if (!current_value.empty()) {
        auto it = std::find_if(order.begin(), order.end(), [&](WidgetPath const& path) {
            return path.getPath() == current_value;
        });
        if (it != order.end()) {
            auto index = static_cast<std::size_t>(std::distance(order.begin(), it));
            if (direction == Direction::Forward) {
                next_index = (index + 1) % order.size();
            } else {
                next_index = (index + order.size() - 1) % order.size();
            }
        } else {
            next_index = (direction == Direction::Forward) ? 0 : order.size() - 1;
        }
    } else {
        next_index = (direction == Direction::Forward) ? 0 : order.size() - 1;
    }

    auto result = Set(space, config, order[next_index]);
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::optional<UpdateResult>{*result};
}

auto Move(PathSpace& space,
          Config const& config,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>> {
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }

    auto app_root = derive_app_root_for(ConcretePathView{config.focus_state.getPath()});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto app_root_view = SP::App::AppRootPathView{app_root->getPath()};

    std::string app_root_string{app_root_view.getPath()};
    FocusScope scope{
        .app_root = app_root_string,
        .widgets_root = app_root_string + "/widgets",
        .window_component = std::nullopt,
    };

    if (current->has_value()) {
        auto derived = make_focus_scope(app_root_view, **current);
        if (!derived) {
            return std::unexpected(derived.error());
        }
        scope = *derived;
    }

    auto order = build_focus_order(space, scope);
    if (!order) {
        return std::unexpected(order.error());
    }
    if (order->empty()) {
        return std::optional<UpdateResult>{};
    }

    return Move(space,
                config,
                std::span<const WidgetPath>(order->data(), order->size()),
                direction);
}

auto ApplyHit(PathSpace& space,
              Config const& config,
              Scene::HitTestResult const& hit) -> SP::Expected<std::optional<UpdateResult>> {
    auto target = ResolveHitTarget(hit);
    if (!target) {
        return std::optional<UpdateResult>{};
    }
    auto result = Set(space, config, target->widget);
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::optional<UpdateResult>{*result};
}

auto SetPulsingHighlight(PathSpace& space,
                         AppRootPathView appRoot,
                         bool enabled) -> SP::Expected<void> {
    std::string root{appRoot.getPath()};
    return write_pulsing_highlight(space, root, enabled);
}

auto PulsingHighlightEnabled(PathSpace& space,
                             AppRootPathView appRoot) -> SP::Expected<bool> {
    std::string root{appRoot.getPath()};
    return read_pulsing_highlight(space, root);
}

auto BuildWindowOrder(PathSpace& space,
                      WindowPath const& window_path) -> SP::Expected<std::vector<WidgetPath>> {
    auto app_root = derive_app_root_for(ConcretePathView{window_path.getPath()});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto app_root_view = SP::App::AppRootPathView{app_root->getPath()};
    auto scope = make_focus_scope_for_window(app_root_view, window_path);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    return build_focus_order(space, *scope);
}

} // namespace Focus

} // namespace SP::UI::Builders::Widgets
