#include "WidgetDetail.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace SP::UI::Builders::Widgets {

using namespace Detail;

namespace {

auto widget_footprint_path(std::string const& widget_root) -> std::string {
    return widget_root + "/meta/footprint";
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
    if (widget_root.rfind(prefix, 0) != 0) {
        return std::unexpected(make_error("widget path must belong to app widgets subtree",
                                          SP::Error::Code::InvalidPath));
    }
    auto name = widget_root.substr(prefix.size());
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
    bool changed = *apply_focus;
    bool mark_new_dirty = *apply_focus;
    bool mark_prev_dirty = false;

    if (!previous.has_value() || *previous != target_path) {
        if (previous.has_value()) {
            auto clear_prev = update_widget_focus(space, *previous, false);
            if (!clear_prev) {
                return std::unexpected(clear_prev.error());
            }
            changed = changed || *clear_prev;
            mark_prev_dirty = true;
        }
        if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, target_path); !status) {
            return std::unexpected(status.error());
        }
        changed = true;
        mark_new_dirty = true;
    }

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

} // namespace Focus

} // namespace SP::UI::Builders::Widgets
