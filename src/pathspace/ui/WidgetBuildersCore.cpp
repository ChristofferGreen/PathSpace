#include "WidgetDetail.hpp"

#include <pathspace/ui/TextBuilder.hpp>

namespace SP::UI::Builders::Widgets {

using namespace Detail;

namespace {

auto sanitize_tree_style(TreeStyle style) -> TreeStyle {
    style.width = std::max(style.width, 96.0f);
    style.row_height = std::max(style.row_height, 24.0f);
    float radius_limit = std::min(style.width, style.row_height) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.border_thickness = std::clamp(style.border_thickness, 0.0f, style.row_height * 0.5f);
    style.indent_per_level = std::max(style.indent_per_level, 8.0f);
    style.toggle_icon_size = std::clamp(style.toggle_icon_size, 4.0f, style.row_height - 4.0f);
    style.label_typography.font_size = std::max(style.label_typography.font_size, 1.0f);
    style.label_typography.line_height = std::max(style.label_typography.line_height,
                                                  style.label_typography.font_size);
    style.label_typography.letter_spacing = std::max(style.label_typography.letter_spacing, 0.0f);
    return style;
}

auto validate_tree_nodes(std::vector<TreeNode> const& nodes) -> SP::Expected<void> {
    std::unordered_set<std::string> ids;
    ids.reserve(nodes.size());

    for (auto const& node : nodes) {
        if (auto status = ensure_identifier(node.id, "tree node id"); !status) {
            return status;
        }
        if (!node.parent_id.empty()) {
            if (auto status = ensure_identifier(node.parent_id, "tree node parent id"); !status) {
                return status;
            }
        }
        if (!ids.insert(node.id).second) {
            return std::unexpected(make_error("tree node ids must be unique",
                                              SP::Error::Code::MalformedInput));
        }
        if (!node.parent_id.empty() && node.parent_id == node.id) {
            return std::unexpected(make_error("tree node cannot parent itself",
                                              SP::Error::Code::MalformedInput));
        }
    }

    std::unordered_map<std::string, std::size_t> index;
    index.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        index.emplace(nodes[i].id, i);
    }

    for (auto const& node : nodes) {
        std::unordered_set<std::string> seen;
        std::string current = node.parent_id;
        while (!current.empty()) {
            if (!seen.insert(current).second) {
                return std::unexpected(make_error("cycle detected in tree node parents",
                                                  SP::Error::Code::MalformedInput));
            }
            if (current == node.id) {
                return std::unexpected(make_error("tree node cannot parent itself",
                                                  SP::Error::Code::MalformedInput));
            }
            auto it = index.find(current);
            if (it == index.end()) {
                break;
            }
            current = nodes[it->second].parent_id;
        }
    }

    return {};
}

auto finalize_tree_nodes(std::vector<TreeNode> nodes) -> std::vector<TreeNode> {
    auto [index, children, roots] = build_tree_children(nodes);
    (void)index;
    (void)roots;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i < children.size() && !children[i].empty()) {
            nodes[i].expandable = true;
            nodes[i].loaded = true;
        }
    }
    return nodes;
}

auto sanitize_list_style(Widgets::ListStyle style,
                         std::size_t item_count) -> Widgets::ListStyle {
    style.width = std::max(style.width, 96.0f);
    style.item_height = std::max(style.item_height, 24.0f);
    float radius_limit = std::min(style.width,
                                  style.item_height * static_cast<float>(std::max<std::size_t>(item_count, 1u))) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.border_thickness = std::clamp(style.border_thickness,
                                        0.0f,
                                        style.item_height * 0.5f);
    style.item_typography.font_size = std::max(style.item_typography.font_size, 1.0f);
    style.item_typography.line_height = std::max(style.item_typography.line_height,
                                                 style.item_typography.font_size);
    style.item_typography.letter_spacing = std::max(style.item_typography.letter_spacing, 0.0f);
    return style;
}

auto sanitize_list_state(Widgets::ListState state,
                         std::vector<Widgets::ListItem> const& items,
                         Widgets::ListStyle const& style) -> Widgets::ListState {
    auto sanitize_index = [&](std::int32_t index) -> std::int32_t {
        if (items.empty()) {
            return -1;
        }
        if (index < 0) {
            return -1;
        }
        if (index >= static_cast<std::int32_t>(items.size())) {
            index = static_cast<std::int32_t>(items.size()) - 1;
        }
        auto const find_enabled = [&](std::int32_t start, std::int32_t step) -> std::int32_t {
            for (std::int32_t candidate = start;
                 candidate >= 0 && candidate < static_cast<std::int32_t>(items.size());
                 candidate += step) {
                auto idx = static_cast<std::size_t>(candidate);
                if (items[idx].enabled) {
                    return candidate;
                }
            }
            return -1;
        };
        auto idx = static_cast<std::size_t>(index);
        if (items[idx].enabled) {
            return index;
        }
        auto forward = find_enabled(index + 1, 1);
        if (forward >= 0) {
            return forward;
        }
        auto backward = find_enabled(index - 1, -1);
        return backward;
    };

    Widgets::ListState sanitized = state;
    sanitized.enabled = state.enabled;
    sanitized.selected_index = sanitize_index(state.selected_index);
    sanitized.hovered_index = sanitize_index(state.hovered_index);
    sanitized.scroll_offset = std::max(state.scroll_offset, 0.0f);

    float const content_span = style.item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u));
    float const max_scroll = std::max(0.0f, content_span - style.item_height);
    sanitized.scroll_offset = std::clamp(sanitized.scroll_offset, 0.0f, max_scroll);

    if (!sanitized.enabled) {
        sanitized.hovered_index = -1;
        sanitized.selected_index = -1;
    }
    sanitized.focused = sanitized.enabled && state.focused;
    return sanitized;
}

auto sanitize_tree_state(TreeState state,
                         TreeStyle const& style,
                         std::vector<TreeNode> const& nodes) -> TreeState {
    auto [index, children, roots] = build_tree_children(nodes);
    (void)roots;

    auto sanitize_id = [&](std::string_view id,
                           bool require_enabled) -> std::string {
        if (id.empty()) {
            return {};
        }
        auto it = index.find(std::string{id});
        if (it == index.end()) {
            return {};
        }
        auto const& node = nodes[it->second];
        if (require_enabled && !node.enabled) {
            return {};
        }
        return node.id;
    };

    state.hovered_id = sanitize_id(state.hovered_id, true);
    state.selected_id = sanitize_id(state.selected_id, true);

    auto sanitize_collection = [&](std::vector<std::string>& entries) {
        std::vector<std::string> filtered;
        filtered.reserve(entries.size());
        std::unordered_set<std::string> seen;
        for (auto const& entry : entries) {
            auto it = index.find(entry);
            if (it == index.end()) {
                continue;
            }
            auto const& node = nodes[it->second];
            bool has_children = (it->second < children.size() && !children[it->second].empty())
                || node.expandable;
            if (!has_children) {
                continue;
            }
            if (!seen.insert(node.id).second) {
                continue;
            }
            filtered.push_back(node.id);
        }
        entries = std::move(filtered);
    };

    sanitize_collection(state.expanded_ids);
    sanitize_collection(state.loading_ids);

    if (!state.enabled) {
        state.hovered_id.clear();
        state.selected_id.clear();
    }

    auto rows = flatten_tree_rows(nodes, state);
    std::size_t visible = rows.empty() ? 1u : rows.size();
    float row_height = std::max(style.row_height, 1.0f);
    float content_height = row_height * static_cast<float>(visible);
    float max_scroll = std::max(0.0f, content_height - row_height);
    state.scroll_offset = std::clamp(std::max(state.scroll_offset, 0.0f), 0.0f, max_scroll);
    return state;
}

} // namespace

auto ResolveHitTarget(Scene::HitTestResult const& hit) -> std::optional<HitTarget> {
    if (!hit.hit) {
        return std::nullopt;
    }

    std::string_view authoring = hit.target.authoring_node_id;
    auto marker = authoring.find(kWidgetAuthoringMarker);
    if (marker == std::string::npos || marker == 0) {
        return std::nullopt;
    }

    std::string widget_root = std::string(authoring.substr(0, marker));
    if (widget_root.empty() || widget_root.front() != '/') {
        return std::nullopt;
    }

    std::string component;
    auto component_pos = marker + kWidgetAuthoringMarker.size();
    if (component_pos < authoring.size()) {
        component.assign(authoring.substr(component_pos));
    }

    HitTarget target{};
    target.widget = WidgetPath{std::move(widget_root)};
    target.component = std::move(component);
    return target;
}

auto CreateButton(PathSpace& space,
                  AppRootPathView appRoot,
                  ButtonParams const& params) -> SP::Expected<ButtonPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::ButtonStyle style = params.style;
    style.width = std::max(style.width, 1.0f);
    style.height = std::max(style.height, 1.0f);
    float radius_limit = std::min(style.width, style.height) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.typography.font_size = std::max(style.typography.font_size, 1.0f);
    style.typography.line_height = std::max(style.typography.line_height, style.typography.font_size);
    style.typography.letter_spacing = std::max(style.typography.letter_spacing, 0.0f);

    Widgets::ButtonState defaultState{};
    if (auto status = write_button_metadata(space,
                                            widgetRoot->getPath(),
                                            params.label,
                                            defaultState,
                                            style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_widget_scene(space,
                                         appRoot,
                                         params.name,
                                         std::string("Widget button: ") + params.label);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_button_state_scenes(space, appRoot, params.name, style);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_button_bucket(style, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    ButtonPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .label = ConcretePath{widgetRoot->getPath() + "/meta/label"},
    };
    return paths;
}

auto write_toggle_metadata(PathSpace& space,
                           std::string const& rootPath,
                           Widgets::ToggleState const& state,
                           Widgets::ToggleStyle const& style) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ToggleState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }
    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::ToggleStyle>(space, stylePath, style); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = write_widget_kind(space, rootPath, "toggle"); !status) {
        return status;
    }
    return {};
}

auto ensure_toggle_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget toggle");
}

auto CreateToggle(PathSpace& space,
                  AppRootPathView appRoot,
                  Widgets::ToggleParams const& params) -> SP::Expected<Widgets::TogglePaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::ToggleState defaultState{};
    if (auto status = write_toggle_metadata(space, widgetRoot->getPath(), defaultState, params.style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_toggle_scene(space, appRoot, params.name);
   if (!scenePath) {
       return std::unexpected(scenePath.error());
   }

    auto stateScenes = publish_toggle_state_scenes(space, appRoot, params.name, params.style);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_toggle_bucket(params.style, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::TogglePaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
    };
    return paths;
}

auto CreateSlider(PathSpace& space,
                  AppRootPathView appRoot,
                  Widgets::SliderParams const& params) -> SP::Expected<Widgets::SliderPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::SliderRange range{};
    range.minimum = std::min(params.minimum, params.maximum);
    range.maximum = std::max(params.minimum, params.maximum);
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }
    range.step = std::max(params.step, 0.0f);

    auto clamp_value = [&](float v) {
        float clamped = std::clamp(v, range.minimum, range.maximum);
        if (range.step > 0.0f) {
            float steps = std::round((clamped - range.minimum) / range.step);
            clamped = range.minimum + steps * range.step;
            clamped = std::clamp(clamped, range.minimum, range.maximum);
        }
        return clamped;
    };

    Widgets::SliderStyle style = params.style;
    style.width = std::max(style.width, 32.0f);
    style.height = std::max(style.height, 16.0f);
    style.track_height = std::clamp(style.track_height, 1.0f, style.height);
    style.thumb_radius = std::clamp(style.thumb_radius, style.track_height * 0.5f, style.height * 0.5f);
    style.label_typography.font_size = std::max(style.label_typography.font_size, 1.0f);
    style.label_typography.line_height = std::max(style.label_typography.line_height, style.label_typography.font_size);
    style.label_typography.letter_spacing = std::max(style.label_typography.letter_spacing, 0.0f);

    Widgets::SliderState defaultState{};
    defaultState.value = clamp_value(params.value);

    if (auto status = write_slider_metadata(space, widgetRoot->getPath(), defaultState, style, range); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_slider_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_slider_state_scenes(space,
                                                   appRoot,
                                                   params.name,
                                                   style,
                                                   range,
                                                   defaultState);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_slider_bucket(style, range, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::SliderPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .range = ConcretePath{widgetRoot->getPath() + "/meta/range"},
    };
    return paths;
}

auto CreateList(PathSpace& space,
                AppRootPathView appRoot,
                Widgets::ListParams const& params) -> SP::Expected<Widgets::ListPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    std::vector<Widgets::ListItem> items = params.items;
    if (items.empty()) {
        items.push_back(Widgets::ListItem{
            .id = "item-0",
            .label = "Item 1",
            .enabled = true,
        });
    }

    std::unordered_set<std::string> ids;
    ids.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto& item = items[index];
        if (item.id.empty()) {
            item.id = "item-" + std::to_string(index);
        }
        if (auto status = ensure_identifier(item.id, "list item id"); !status) {
            return std::unexpected(status.error());
        }
        if (!ids.insert(item.id).second) {
            return std::unexpected(make_error("list item ids must be unique",
                                              SP::Error::Code::MalformedInput));
        }
    }

    Widgets::ListStyle style = sanitize_list_style(params.style, items.size());

    auto first_enabled = std::find_if(items.begin(), items.end(), [](auto const& entry) {
        return entry.enabled;
    });

    Widgets::ListState defaultState{};
    defaultState.selected_index = (first_enabled != items.end())
        ? static_cast<std::int32_t>(std::distance(items.begin(), first_enabled))
        : -1;
    defaultState.hovered_index = -1;
    defaultState.scroll_offset = 0.0f;
    defaultState = sanitize_list_state(defaultState, items, style);

    if (auto status = write_list_metadata(space,
                                          widgetRoot->getPath(),
                                          defaultState,
                                          style,
                                          items); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_list_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_list_state_scenes(space,
                                                 appRoot,
                                                 params.name,
                                                 style,
                                                 items,
                                                 defaultState);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_list_bucket(style, items, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::ListPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .items = ConcretePath{widgetRoot->getPath() + "/meta/items"},
    };
    return paths;
}

auto CreateTree(PathSpace& space,
                AppRootPathView appRoot,
                Widgets::TreeParams const& params) -> SP::Expected<Widgets::TreePaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    std::vector<TreeNode> nodes = params.nodes;
    if (auto status = validate_tree_nodes(nodes); !status) {
        return std::unexpected(status.error());
    }
    nodes = finalize_tree_nodes(std::move(nodes));

    TreeStyle style = sanitize_tree_style(params.style);

    TreeState defaultState{};
    defaultState.enabled = true;
    defaultState.hovered_id.clear();
    defaultState.selected_id.clear();
    defaultState.expanded_ids.clear();
    defaultState.loading_ids.clear();
    defaultState.scroll_offset = 0.0f;
    defaultState = sanitize_tree_state(defaultState, style, nodes);

    if (auto status = write_tree_metadata(space,
                                          widgetRoot->getPath(),
                                          defaultState,
                                          style,
                                          nodes); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_tree_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_tree_state_scenes(space,
                                                 appRoot,
                                                 params.name,
                                                 style,
                                                 nodes,
                                                 defaultState);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_tree_bucket(style, nodes, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::TreePaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .nodes = ConcretePath{widgetRoot->getPath() + "/meta/nodes"},
    };
    return paths;
}

auto CreateTextField(PathSpace& space,
                     AppRootPathView appRoot,
                     Widgets::TextFieldParams const& params) -> SP::Expected<Widgets::TextFieldPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    auto style = sanitize_text_field_style(params.style);
    auto state = sanitize_text_field_state(params.state, style);
    if (auto status = write_text_field_metadata(space,
                                                widgetRoot->getPath(),
                                                state,
                                                style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_text_field_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_text_field_state_scenes(space,
                                                       appRoot,
                                                       params.name,
                                                       style,
                                                       state);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_text_field_bucket(style, state, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::TextFieldPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
    };
    return paths;
}

auto CreateTextArea(PathSpace& space,
                    AppRootPathView appRoot,
                    Widgets::TextAreaParams const& params) -> SP::Expected<Widgets::TextAreaPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    auto style = sanitize_text_area_style(params.style);
    auto state = sanitize_text_area_state(params.state, style);

    if (auto status = write_text_area_metadata(space,
                                               widgetRoot->getPath(),
                                               state,
                                               style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_text_area_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_text_area_state_scenes(space,
                                                      appRoot,
                                                      params.name,
                                                      style,
                                                      state);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_text_area_bucket(style, state, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::TextAreaPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
    };
    return paths;
}

auto UpdateButtonState(PathSpace& space,
                       Widgets::ButtonPaths const& paths,
                       Widgets::ButtonState const& new_state) -> SP::Expected<bool> {
    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ButtonState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !button_states_equal(**current, new_state);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ButtonState>(space, statePath, new_state); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ButtonStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_button_bucket(*styleValue, new_state, paths.root.getPath(), *pulsing);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto SetExclusiveButtonFocus(PathSpace& space,
                             std::span<Widgets::ButtonPaths const> buttons,
                             std::optional<std::size_t> focused_index) -> SP::Expected<void> {
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        auto const& paths = buttons[i];
        auto statePath = std::string(paths.state.getPath());
        auto state = read_optional<Widgets::ButtonState>(space, statePath);
        if (!state) {
            return std::unexpected(state.error());
        }
        Widgets::ButtonState desired = state->value_or(Widgets::MakeButtonState().Build());
        bool should_focus = focused_index.has_value() && *focused_index == i;
        if (!should_focus && desired.hovered) {
            desired.hovered = false;
        }
        if (desired.focused == should_focus) {
            continue;
        }
        desired.focused = should_focus;
        auto updated = UpdateButtonState(space, paths, desired);
        if (!updated) {
            return std::unexpected(updated.error());
        }
    }
    return {};
}

auto UpdateToggleState(PathSpace& space,
                       Widgets::TogglePaths const& paths,
                       Widgets::ToggleState const& new_state) -> SP::Expected<bool> {
    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ToggleState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !toggle_states_equal(**current, new_state);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ToggleState>(space, statePath, new_state); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ToggleStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_toggle_bucket(*styleValue, new_state, paths.root.getPath(), *pulsing);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateSliderState(PathSpace& space,
                       Widgets::SliderPaths const& paths,
                       Widgets::SliderState const& new_state) -> SP::Expected<bool> {
    auto rangePath = std::string(paths.range.getPath());
    auto rangeValue = read_optional<Widgets::SliderRange>(space, rangePath);
    if (!rangeValue) {
        return std::unexpected(rangeValue.error());
    }
    Widgets::SliderRange range = rangeValue->value_or(Widgets::SliderRange{});
    if (range.minimum > range.maximum) {
        std::swap(range.minimum, range.maximum);
    }
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }

    auto clamp_value = [&](float v) {
        float clamped = std::clamp(v, range.minimum, range.maximum);
        if (range.step > 0.0f) {
            float steps = std::round((clamped - range.minimum) / range.step);
            clamped = range.minimum + steps * range.step;
            clamped = std::clamp(clamped, range.minimum, range.maximum);
        }
        return clamped;
    };

    Widgets::SliderState sanitized = new_state;
    sanitized.value = clamp_value(new_state.value);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::SliderState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !slider_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::SliderState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::SliderStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_slider_bucket(*styleValue, range, sanitized, paths.root.getPath(), *pulsing);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto BuildButtonPreview(Widgets::ButtonStyle const& style,
                        Widgets::ButtonState const& state,
                        Widgets::ButtonPreviewOptions const& options) -> SceneData::DrawableBucketSnapshot {
    return build_button_bucket(style,
                               state,
                               std::string_view{options.authoring_root},
                               options.pulsing_highlight);
}

auto BuildLabel(LabelBuildParams const& params) -> std::optional<Text::BuildResult> {
    return Text::BuildTextBucket(params.text,
                                 params.origin_x,
                                 params.origin_y,
                                 params.typography,
                                 params.color,
                                 params.drawable_id,
                                 params.authoring_id,
                                 params.z_value);
}

auto LabelBounds(Text::BuildResult const& result) -> std::optional<Input::WidgetBounds> {
    if (result.bucket.bounds_boxes.empty()) {
        return std::nullopt;
    }
    auto const& box = result.bucket.bounds_boxes.front();
    Input::WidgetBounds bounds{box.min[0], box.min[1], box.max[0], box.max[1]};
    bounds.normalize();
    return bounds;
}

auto BuildTogglePreview(Widgets::ToggleStyle const& style,
                        Widgets::ToggleState const& state,
                        Widgets::TogglePreviewOptions const& options) -> SceneData::DrawableBucketSnapshot {
    return build_toggle_bucket(style,
                               state,
                               std::string_view{options.authoring_root},
                               options.pulsing_highlight);
}

auto BuildSliderPreview(Widgets::SliderStyle const& style,
                        Widgets::SliderRange const& range,
                        Widgets::SliderState const& state,
                        Widgets::SliderPreviewOptions const& options) -> SceneData::DrawableBucketSnapshot {
    return build_slider_bucket(style,
                               range,
                               state,
                               std::string_view{options.authoring_root},
                               options.pulsing_highlight);
}

auto Input::BoundsFromRect(Widgets::ListPreviewRect const& rect) -> Input::WidgetBounds {
    Input::WidgetBounds bounds{rect.min_x, rect.min_y, rect.max_x, rect.max_y};
    bounds.normalize();
    return bounds;
}

auto Input::BoundsFromRect(Widgets::TreePreviewRect const& rect) -> Input::WidgetBounds {
    Input::WidgetBounds bounds{rect.min_x, rect.min_y, rect.max_x, rect.max_y};
    bounds.normalize();
    return bounds;
}

auto Input::BoundsFromRect(Widgets::TreePreviewRect const& rect,
                           float dx,
                           float dy) -> Input::WidgetBounds {
    auto bounds = Input::BoundsFromRect(rect);
    bounds.min_x += dx;
    bounds.max_x += dx;
    bounds.min_y += dy;
    bounds.max_y += dy;
    return bounds;
}

auto Input::MakeListLayout(Widgets::ListPreviewLayout const& layout) -> std::optional<Input::ListLayout> {
    if (layout.rows.empty()) {
        return std::nullopt;
    }
    Input::ListLayout out{};
    out.bounds = BoundsFromRect(layout.bounds);
    out.content_top = layout.content_top;
    out.item_height = layout.item_height;
    out.item_bounds.reserve(layout.rows.size());
    for (auto const& row : layout.rows) {
        out.item_bounds.push_back(Input::BoundsFromRect(row.row_bounds));
    }
    return out;
}

auto Input::MakeTreeLayout(Widgets::TreePreviewLayout const& layout) -> std::optional<Input::TreeLayout> {
    if (layout.rows.empty()) {
        return std::nullopt;
    }
    Input::TreeLayout out{};
    out.bounds = Input::BoundsFromRect(layout.bounds);
    out.content_top = layout.content_top;
    out.row_height = layout.row_height;
    out.rows.reserve(layout.rows.size());
    for (auto const& row : layout.rows) {
        Input::TreeRowLayout entry{};
        entry.bounds = Input::BoundsFromRect(row.row_bounds);
        entry.toggle = Input::BoundsFromRect(row.toggle_bounds);
        entry.node_id = row.id;
        entry.label = row.label;
        entry.depth = row.depth;
        entry.expandable = row.expandable;
        entry.expanded = row.expanded;
        entry.loading = row.loading;
        entry.enabled = row.enabled;
        out.rows.push_back(std::move(entry));
    }
    return out;
}

auto Input::ExpandForFocusHighlight(Input::WidgetBounds& bounds) -> void {
    bounds.normalize();
    float expand = Detail::kFocusHighlightExpand + Detail::kFocusHighlightThickness;
    bounds.min_x -= expand;
    bounds.min_y -= expand;
    bounds.max_x += expand;
    bounds.max_y += expand;
    bounds.normalize();
    if (bounds.min_x < 0.0f) {
        bounds.min_x = 0.0f;
    }
    if (bounds.min_y < 0.0f) {
        bounds.min_y = 0.0f;
    }
}

auto Input::FocusHighlightPadding() -> float {
    return Detail::kFocusHighlightExpand + Detail::kFocusHighlightThickness;
}

auto Input::MakeDirtyHint(Input::WidgetBounds const& bounds) -> Builders::DirtyRectHint {
    Input::WidgetBounds normalized = bounds;
    normalized.normalize();
    Builders::DirtyRectHint hint{};
    hint.min_x = normalized.min_x;
    hint.min_y = normalized.min_y;
    hint.max_x = normalized.max_x;
    hint.max_y = normalized.max_y;
    return hint;
}

auto Input::TranslateTreeLayout(TreeLayout& layout, float dx, float dy) -> void {
    layout.bounds.min_x += dx;
    layout.bounds.max_x += dx;
    layout.bounds.min_y += dy;
    layout.bounds.max_y += dy;
    for (auto& row : layout.rows) {
        row.bounds.min_x += dx;
        row.bounds.max_x += dx;
        row.bounds.min_y += dy;
        row.bounds.max_y += dy;
        row.toggle.min_x += dx;
        row.toggle.max_x += dx;
        row.toggle.min_y += dy;
        row.toggle.max_y += dy;
    }
}

auto UpdateListState(PathSpace& space,
                     Widgets::ListPaths const& paths,
                     Widgets::ListState const& new_state) -> SP::Expected<bool> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    auto itemsValue = read_optional<std::vector<Widgets::ListItem>>(space, itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    std::vector<Widgets::ListItem> items;
    if (itemsValue->has_value()) {
        items = **itemsValue;
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ListStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    Widgets::ListStyle style = sanitize_list_style(*styleValue, items.size());
    Widgets::ListState sanitized = sanitize_list_state(new_state, items, style);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ListState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !list_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ListState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_list_bucket(style, items, sanitized, paths.root.getPath(), *pulsing);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto BuildListPreview(Widgets::ListStyle const& style_input,
                      std::span<Widgets::ListItem const> items_input,
                      Widgets::ListState const& state_input,
                      Widgets::ListPreviewOptions const& options) -> Widgets::ListPreviewResult {
    std::vector<Widgets::ListItem> items(items_input.begin(), items_input.end());
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].id.empty()) {
            items[index].id = "item-" + std::to_string(index);
        }
    }

    Widgets::ListStyle style = sanitize_list_style(style_input, items.size());
    Widgets::ListState state = sanitize_list_state(state_input, items, style);

    auto bucket = build_list_bucket(style,
                                    items,
                                    state,
                                    options.authoring_root,
                                    options.pulsing_highlight);

    Widgets::ListPreviewLayout layout{};
    float const item_count = static_cast<float>(std::max<std::size_t>(items.size(), 1u));
    float const height = style.border_thickness * 2.0f + style.item_height * item_count;
    layout.bounds = Widgets::ListPreviewRect{
        0.0f,
        0.0f,
        style.width,
        height,
    };
    layout.content_top = style.border_thickness;
    layout.item_height = style.item_height;
    layout.border_thickness = style.border_thickness;
    float available_inset = std::max(0.0f, (style.width - style.border_thickness * 2.0f) * 0.5f);
    float label_inset = std::clamp(options.label_inset, 0.0f, available_inset);
    layout.label_inset = label_inset;
    layout.style = style;
    layout.state = state;

    layout.rows.reserve(items.size());
    float label_line_height = style.item_typography.line_height;
    for (std::size_t index = 0; index < items.size(); ++index) {
        float row_top = layout.content_top + style.item_height * static_cast<float>(index);
        float row_bottom = row_top + style.item_height;
        float row_min_x = style.border_thickness;
        float row_max_x = style.width - style.border_thickness;

        float label_top = row_top + std::max(0.0f, (style.item_height - label_line_height) * 0.5f);
        float label_baseline = label_top + style.item_typography.baseline_shift;
        float label_min_x = row_min_x + label_inset;
        float label_max_x = std::max(label_min_x, row_max_x - label_inset);

        Widgets::ListPreviewRowLayout row{};
        row.id = items[index].id;
        row.enabled = items[index].enabled;
        row.hovered = (state.hovered_index == static_cast<std::int32_t>(index));
        row.selected = (state.selected_index == static_cast<std::int32_t>(index));
        row.row_bounds = Widgets::ListPreviewRect{
            row_min_x,
            row_top,
            row_max_x,
            row_bottom,
        };
        row.label_bounds = Widgets::ListPreviewRect{
            label_min_x,
            label_top,
            label_max_x,
            label_top + label_line_height,
        };
        row.label_baseline = label_baseline;
        layout.rows.push_back(std::move(row));
    }

    Widgets::ListPreviewResult result{};
    result.bucket = std::move(bucket);
    result.layout = std::move(layout);
    return result;
}

auto BuildTreePreview(Widgets::TreeStyle const& style_input,
                      std::span<Widgets::TreeNode const> nodes_input,
                      Widgets::TreeState const& state_input,
                      Widgets::TreePreviewOptions const& options) -> Widgets::TreePreviewResult {
    std::vector<Widgets::TreeNode> nodes(nodes_input.begin(), nodes_input.end());
    if (auto status = validate_tree_nodes(nodes); status) {
        nodes = finalize_tree_nodes(std::move(nodes));
    } else {
        nodes.clear();
    }

    Widgets::TreeStyle style = sanitize_tree_style(style_input);
    Widgets::TreeState state = sanitize_tree_state(state_input, style, nodes);

    Widgets::TreePreviewLayout layout{};
    auto bucket = build_tree_bucket(style,
                                    nodes,
                                    state,
                                    options.authoring_root,
                                    options.pulsing_highlight,
                                    &layout);

    Widgets::TreePreviewResult result{};
    result.bucket = std::move(bucket);
    result.layout = std::move(layout);
    return result;
}

auto BuildStackPreview(Widgets::StackLayoutStyle const& style_input,
                       Widgets::StackLayoutState const& state_input,
                       Widgets::StackPreviewOptions const& options) -> Widgets::StackPreviewResult {
    Widgets::StackLayoutStyle style = style_input;
    Widgets::StackLayoutState state = state_input;

    float width = std::max(style.width, state.width);
    float height = std::max(style.height, state.height);
    for (auto const& child : state.children) {
        width = std::max(width, child.x + child.width);
        height = std::max(height, child.y + child.height);
    }
    width = std::max(width, 1.0f);
    height = std::max(height, 1.0f);

    Widgets::StackPreviewLayout layout{};
    layout.style = style;
    layout.state = state;
    layout.state.width = width;
    layout.state.height = height;
    layout.bounds = Widgets::StackPreviewRect{0.0f, 0.0f, width, height};
    layout.child_bounds.reserve(state.children.size());
    for (auto const& child : state.children) {
        layout.child_bounds.push_back(Widgets::StackPreviewRect{
            child.x,
            child.y,
            child.x + child.width,
            child.y + child.height,
        });
    }

    SceneData::DrawableBucketSnapshot bucket{};
    std::string const authoring_root = options.authoring_root.empty()
        ? std::string("widgets/stack_preview")
        : options.authoring_root;

    auto push_rect = [&](std::uint64_t drawable_id,
                         Widgets::StackPreviewRect const& bounds,
                         std::array<float, 4> const& color,
                         float z,
                         std::string const& suffix) {
        bucket.drawable_ids.push_back(drawable_id);
        bucket.world_transforms.push_back(make_identity_transform());

        SceneData::BoundingBox box{};
        box.min = {bounds.min_x, bounds.min_y, 0.0f};
        box.max = {bounds.max_x, bounds.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        SceneData::BoundingSphere sphere{};
        sphere.center = {(bounds.min_x + bounds.max_x) * 0.5f,
                         (bounds.min_y + bounds.max_y) * 0.5f,
                         0.0f};
        float half_w = (bounds.max_x - bounds.min_x) * 0.5f;
        float half_h = (bounds.max_y - bounds.min_y) * 0.5f;
        sphere.radius = std::sqrt(half_w * half_w + half_h * half_h);
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(z);
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_counts.push_back(1);
        bucket.opaque_indices.push_back(static_cast<std::uint32_t>(bucket.opaque_indices.size()));
        bucket.clip_head_indices.push_back(-1);

        SceneData::RoundedRectCommand rect{};
        rect.min_x = bounds.min_x;
        rect.min_y = bounds.min_y;
        rect.max_x = bounds.max_x;
        rect.max_y = bounds.max_y;
        rect.radius_top_left = 8.0f;
        rect.radius_top_right = 8.0f;
        rect.radius_bottom_right = 8.0f;
        rect.radius_bottom_left = 8.0f;
        rect.color = color;

        auto payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(SceneData::RoundedRectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &rect,
                    sizeof(SceneData::RoundedRectCommand));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

        bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
            drawable_id,
            make_widget_authoring_id(authoring_root, suffix),
            0,
            0,
        });
        bucket.drawable_fingerprints.push_back(drawable_id);
    };

    push_rect(0x31A00001ull,
              layout.bounds,
              options.background_color,
              0.0f,
              "stack/background");

    auto const child_count = layout.child_bounds.size();
    for (std::size_t index = 0; index < child_count; ++index) {
        auto const& rect = layout.child_bounds[index];
        float t = (child_count <= 1)
            ? 0.5f
            : static_cast<float>(index) / static_cast<float>(child_count - 1);
        float mix_amount = std::clamp(t * options.mix_scale, 0.0f, 1.0f);
        auto color = mix_color(options.child_start_color, options.child_end_color, mix_amount);
        color[3] = options.child_opacity;
        std::string suffix = "stack/child/" + state.children[index].id;
        push_rect(0x31A10000ull + static_cast<std::uint64_t>(index),
                  rect,
                  color,
                  0.05f + 0.01f * static_cast<float>(index),
                  suffix);
    }

    Widgets::StackPreviewResult result{};
    result.bucket = std::move(bucket);
    result.layout = std::move(layout);
    return result;
}

auto UpdateTreeState(PathSpace& space,
                     Widgets::TreePaths const& paths,
                     Widgets::TreeState const& new_state) -> SP::Expected<bool> {
    auto nodesValue = space.read<std::vector<TreeNode>, std::string>(paths.nodes.getPath());
    if (!nodesValue) {
        return std::unexpected(nodesValue.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<TreeStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    TreeStyle style = sanitize_tree_style(*styleValue);
    TreeState sanitized = sanitize_tree_state(new_state, style, *nodesValue);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<TreeState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }

    bool changed = !current->has_value() || !tree_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }

    if (auto status = replace_single<TreeState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_tree_bucket(style, *nodesValue, sanitized, paths.root.getPath(), *pulsing);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateTextFieldState(PathSpace& space,
                          Widgets::TextFieldPaths const& paths,
                          Widgets::TextFieldState const& new_state) -> SP::Expected<bool> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::TextFieldStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    auto style = sanitize_text_field_style(*styleValue);
    auto sanitized = sanitize_text_field_state(new_state, style);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::TextFieldState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }

    bool changed = !current->has_value() || !text_field_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }

    if (auto status = replace_single<Widgets::TextFieldState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_text_field_bucket(style,
                                          sanitized,
                                          paths.root.getPath(),
                                          *pulsing && sanitized.focused);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateTextAreaState(PathSpace& space,
                         Widgets::TextAreaPaths const& paths,
                         Widgets::TextAreaState const& new_state) -> SP::Expected<bool> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::TextAreaStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    auto style = sanitize_text_area_style(*styleValue);
    auto sanitized = sanitize_text_area_state(new_state, style);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::TextAreaState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }

    bool changed = !current->has_value() || !text_area_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }

    if (auto status = replace_single<Widgets::TextAreaState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto pulsing = Focus::PulsingHighlightEnabled(space, appRootView);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }
    auto bucket = build_text_area_bucket(style,
                                         sanitized,
                                         paths.root.getPath(),
                                         *pulsing && sanitized.focused);
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

namespace {

auto make_blue_theme() -> WidgetTheme {
    WidgetTheme theme{};
    theme.button.width = 200.0f;
    theme.button.height = 48.0f;
    theme.button.corner_radius = 8.0f;
    theme.button.background_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.button.text_color = {1.0f, 1.0f, 1.0f, 1.0f};
    theme.button.typography.font_size = 28.0f;
    theme.button.typography.line_height = 28.0f;
    theme.button.typography.letter_spacing = 1.0f;
    theme.button.typography.baseline_shift = 0.0f;

    theme.toggle.width = 56.0f;
    theme.toggle.height = 32.0f;
    theme.toggle.track_off_color = {0.75f, 0.75f, 0.78f, 1.0f};
    theme.toggle.track_on_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.toggle.thumb_color = {1.0f, 1.0f, 1.0f, 1.0f};

    theme.slider.width = 240.0f;
    theme.slider.height = 32.0f;
    theme.slider.track_height = 6.0f;
    theme.slider.thumb_radius = 10.0f;
    theme.slider.track_color = {0.75f, 0.75f, 0.78f, 1.0f};
    theme.slider.fill_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.slider.thumb_color = {1.0f, 1.0f, 1.0f, 1.0f};
    theme.slider.label_color = {0.90f, 0.92f, 0.96f, 1.0f};
    theme.slider.label_typography.font_size = 24.0f;
    theme.slider.label_typography.line_height = 28.0f;
    theme.slider.label_typography.letter_spacing = 1.0f;
    theme.slider.label_typography.baseline_shift = 0.0f;

    theme.list.width = 240.0f;
    theme.list.item_height = 36.0f;
    theme.list.corner_radius = 8.0f;
    theme.list.border_thickness = 1.0f;
    theme.list.background_color = {0.121f, 0.129f, 0.145f, 1.0f};
    theme.list.border_color = {0.239f, 0.247f, 0.266f, 1.0f};
    theme.list.item_color = {0.176f, 0.184f, 0.204f, 1.0f};
    theme.list.item_hover_color = {0.247f, 0.278f, 0.349f, 1.0f};
    theme.list.item_selected_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.list.separator_color = {0.224f, 0.231f, 0.247f, 1.0f};
    theme.list.item_text_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.list.item_typography.font_size = 21.0f;
    theme.list.item_typography.line_height = 24.0f;
    theme.list.item_typography.letter_spacing = 1.0f;
    theme.list.item_typography.baseline_shift = 0.0f;

    theme.tree.width = 280.0f;
    theme.tree.row_height = 32.0f;
    theme.tree.corner_radius = 8.0f;
    theme.tree.border_thickness = 1.0f;
    theme.tree.indent_per_level = 18.0f;
    theme.tree.toggle_icon_size = 12.0f;
    theme.tree.background_color = {0.121f, 0.129f, 0.145f, 1.0f};
    theme.tree.border_color = {0.239f, 0.247f, 0.266f, 1.0f};
    theme.tree.row_color = {0.176f, 0.184f, 0.204f, 1.0f};
    theme.tree.row_hover_color = {0.247f, 0.278f, 0.349f, 1.0f};
    theme.tree.row_selected_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.tree.row_disabled_color = {0.145f, 0.149f, 0.162f, 1.0f};
    theme.tree.connector_color = {0.224f, 0.231f, 0.247f, 1.0f};
    theme.tree.toggle_color = {0.90f, 0.92f, 0.96f, 1.0f};
    theme.tree.text_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.tree.label_typography.font_size = 20.0f;
    theme.tree.label_typography.line_height = 24.0f;
    theme.tree.label_typography.letter_spacing = 0.8f;
    theme.tree.label_typography.baseline_shift = 0.0f;

    theme.text_field.width = 320.0f;
    theme.text_field.height = 48.0f;
    theme.text_field.corner_radius = 6.0f;
    theme.text_field.border_thickness = 1.5f;
    theme.text_field.background_color = {0.121f, 0.129f, 0.145f, 1.0f};
    theme.text_field.border_color = {0.239f, 0.247f, 0.266f, 1.0f};
    theme.text_field.text_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.text_field.placeholder_color = {0.58f, 0.60f, 0.66f, 1.0f};
    theme.text_field.selection_color = {0.247f, 0.278f, 0.349f, 0.65f};
    theme.text_field.composition_color = {0.353f, 0.388f, 0.458f, 0.55f};
    theme.text_field.caret_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.text_field.padding_x = 14.0f;
    theme.text_field.padding_y = 12.0f;
    theme.text_field.typography.font_size = 24.0f;
    theme.text_field.typography.line_height = 28.0f;
    theme.text_field.typography.letter_spacing = 0.6f;
    theme.text_field.typography.baseline_shift = 0.0f;

    theme.text_area.width = 420.0f;
    theme.text_area.height = 220.0f;
    theme.text_area.corner_radius = 6.0f;
    theme.text_area.border_thickness = 1.5f;
    theme.text_area.background_color = {0.102f, 0.112f, 0.128f, 1.0f};
    theme.text_area.border_color = {0.224f, 0.231f, 0.247f, 1.0f};
    theme.text_area.text_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.text_area.placeholder_color = {0.58f, 0.60f, 0.66f, 1.0f};
    theme.text_area.selection_color = {0.247f, 0.278f, 0.349f, 0.65f};
    theme.text_area.composition_color = {0.353f, 0.388f, 0.458f, 0.55f};
    theme.text_area.caret_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.text_area.padding_x = 14.0f;
    theme.text_area.padding_y = 12.0f;
    theme.text_area.min_height = 180.0f;
    theme.text_area.line_spacing = 6.0f;
    theme.text_area.typography.font_size = 22.0f;
    theme.text_area.typography.line_height = 28.0f;
    theme.text_area.typography.letter_spacing = 0.4f;
    theme.text_area.typography.baseline_shift = 0.0f;

    theme.heading.font_size = 32.0f;
    theme.heading.line_height = 36.0f;
    theme.heading.letter_spacing = 1.0f;
    theme.heading.baseline_shift = 0.0f;
    theme.caption.font_size = 24.0f;
    theme.caption.line_height = 28.0f;
    theme.caption.letter_spacing = 1.0f;
    theme.caption.baseline_shift = 0.0f;
    theme.heading_color = {0.93f, 0.95f, 0.98f, 1.0f};
    theme.caption_color = {0.90f, 0.92f, 0.96f, 1.0f};
    theme.accent_text_color = {0.85f, 0.88f, 0.95f, 1.0f};
    theme.muted_text_color = {0.70f, 0.72f, 0.78f, 1.0f};

    return theme;
}

auto make_orange_theme() -> WidgetTheme {
    WidgetTheme theme = make_blue_theme();
    theme.button.background_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.button.text_color = {1.0f, 0.984f, 0.945f, 1.0f};
    theme.toggle.track_on_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.toggle.track_off_color = {0.60f, 0.44f, 0.38f, 1.0f};
    theme.toggle.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.fill_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.slider.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.text_field.background_color = {0.145f, 0.102f, 0.086f, 1.0f};
    theme.text_field.border_color = {0.322f, 0.231f, 0.196f, 1.0f};
    theme.text_field.selection_color = {0.690f, 0.361f, 0.259f, 0.65f};
    theme.text_field.composition_color = {0.784f, 0.427f, 0.302f, 0.55f};
    theme.text_field.caret_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.text_area.background_color = {0.132f, 0.090f, 0.075f, 1.0f};
    theme.text_area.border_color = {0.302f, 0.212f, 0.184f, 1.0f};
    theme.text_area.selection_color = {0.690f, 0.361f, 0.259f, 0.65f};
    theme.text_area.composition_color = {0.784f, 0.427f, 0.302f, 0.55f};
    theme.text_area.caret_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.label_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.list.background_color = {0.215f, 0.128f, 0.102f, 1.0f};
    theme.list.border_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_color = {0.266f, 0.166f, 0.138f, 1.0f};
    theme.list.item_hover_color = {0.422f, 0.248f, 0.198f, 1.0f};
    theme.list.item_selected_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.list.separator_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.tree.background_color = {0.215f, 0.128f, 0.102f, 1.0f};
    theme.tree.border_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.tree.row_color = {0.266f, 0.166f, 0.138f, 1.0f};
    theme.tree.row_hover_color = {0.422f, 0.248f, 0.198f, 1.0f};
    theme.tree.row_selected_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.tree.row_disabled_color = {0.184f, 0.118f, 0.098f, 1.0f};
    theme.tree.connector_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.tree.toggle_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.tree.text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.heading_color = {0.996f, 0.949f, 0.902f, 1.0f};
   theme.caption_color = {0.965f, 0.886f, 0.812f, 1.0f};
   theme.accent_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
   theme.muted_text_color = {0.855f, 0.698f, 0.612f, 1.0f};
    return theme;
}

} // namespace

auto MakeDefaultWidgetTheme() -> WidgetTheme {
    return make_blue_theme();
}

auto MakeSunsetWidgetTheme() -> WidgetTheme {
    return make_orange_theme();
}

auto LoadTheme(PathSpace& space,
              AppRootPathView appRoot,
              std::string_view requested_name) -> ThemeSelection {
    ThemeSelection selection{};

    std::string requested{requested_name};
    if (requested.empty()) {
        if (auto active = Config::Theme::LoadActive(space, appRoot)) {
            requested = *active;
        }
    }

    if (requested.empty()) {
        requested = "sunset";
    }

    std::string normalized = requested;
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    bool recognized = true;
    std::string canonical = normalized;
    WidgetTheme defaults = MakeSunsetWidgetTheme();

    if (normalized == "sunset") {
        canonical = "sunset";
        defaults = MakeSunsetWidgetTheme();
    } else if (normalized == "skylight" || normalized == "default") {
        canonical = "skylight";
        defaults = MakeDefaultWidgetTheme();
    } else {
        recognized = false;
        canonical = Config::Theme::SanitizeName(normalized);
        defaults = MakeDefaultWidgetTheme();
    }

    auto ensured = Config::Theme::Ensure(space, appRoot, canonical, defaults);
    WidgetTheme theme = defaults;
    if (ensured) {
        if (auto loaded = Config::Theme::Load(space, *ensured)) {
            theme = *loaded;
        }
        (void)Config::Theme::SetActive(space, appRoot, canonical);
    }

    selection.theme = std::move(theme);
    selection.canonical_name = canonical;
    selection.recognized = recognized;
    return selection;
}

namespace {

inline auto sanitize_stack_constraints(Widgets::StackChildConstraints constraints)
    -> Widgets::StackChildConstraints {
    constraints.weight = std::max(constraints.weight, 0.0f);
    constraints.margin_main_start = std::max(constraints.margin_main_start, 0.0f);
    constraints.margin_main_end = std::max(constraints.margin_main_end, 0.0f);
    constraints.margin_cross_start = std::max(constraints.margin_cross_start, 0.0f);
    constraints.margin_cross_end = std::max(constraints.margin_cross_end, 0.0f);
    if (constraints.has_min_main && constraints.has_max_main && constraints.max_main < constraints.min_main) {
        std::swap(constraints.min_main, constraints.max_main);
    }
    if (constraints.has_min_cross && constraints.has_max_cross && constraints.max_cross < constraints.min_cross) {
        std::swap(constraints.min_cross, constraints.max_cross);
    }
    return constraints;
}

inline auto resolve_stack_path(AppRootPathView appRoot,
                               std::string const& input) -> SP::Expected<std::string> {
    if (input.empty()) {
        return std::unexpected(make_error("stack child path must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    if (input.front() == '/') {
        return input;
    }
    auto resolved = combine_relative(appRoot, input);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return resolved->getPath();
}

inline auto normalize_stack_children(PathSpace& space,
                                     AppRootPathView appRoot,
                                     std::vector<Widgets::StackChildSpec> const& input)
    -> SP::Expected<std::vector<Widgets::StackChildSpec>> {
    if (input.empty()) {
        return std::unexpected(make_error("stack layout requires at least one child",
                                          SP::Error::Code::InvalidType));
    }

    std::unordered_set<std::string> ids;
    ids.reserve(input.size());

    std::vector<Widgets::StackChildSpec> output;
    output.reserve(input.size());

    for (auto const& spec : input) {
        if (auto status = ensure_identifier(spec.id, "stack child id"); !status) {
            return std::unexpected(status.error());
        }
        if (!ids.insert(spec.id).second) {
            return std::unexpected(make_error("duplicate stack child id: " + spec.id,
                                              SP::Error::Code::InvalidType));
        }

        auto widget_path = resolve_stack_path(appRoot, spec.widget_path);
        if (!widget_path) {
            return std::unexpected(widget_path.error());
        }

        auto scene_path = resolve_stack_path(appRoot, spec.scene_path);
        if (!scene_path) {
            return std::unexpected(scene_path.error());
        }

        Widgets::StackChildSpec normalized;
        normalized.id = spec.id;
        normalized.widget_path = *widget_path;
        normalized.scene_path = *scene_path;
        normalized.constraints = sanitize_stack_constraints(spec.constraints);

        output.push_back(std::move(normalized));
    }
    (void)space;
    return output;
}

inline auto layout_equal(Widgets::StackLayoutState const& lhs,
                         Widgets::StackLayoutState const& rhs) -> bool {
    auto equal_float = [](float a, float b) {
        return std::fabs(a - b) <= 1e-4f;
    };
    if (!equal_float(lhs.width, rhs.width) || !equal_float(lhs.height, rhs.height)) {
        return false;
    }
    if (lhs.children.size() != rhs.children.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.children.size(); ++index) {
        auto const& a = lhs.children[index];
        auto const& b = rhs.children[index];
        if (a.id != b.id) {
            return false;
        }
        if (!equal_float(a.x, b.x) || !equal_float(a.y, b.y)
            || !equal_float(a.width, b.width) || !equal_float(a.height, b.height)) {
            return false;
        }
    }
    return true;
}

inline auto stack_paths_from_root(std::string const& rootPath) -> StackPaths {
    StackPaths paths{};
    paths.root = WidgetPath{rootPath};
    paths.style = ConcretePath{rootPath + "/layout/style"};
    paths.children = ConcretePath{rootPath + "/layout/children"};
    paths.computed = ConcretePath{rootPath + "/layout/computed"};
    return paths;
}

} // namespace

auto CreateStack(PathSpace& space,
                 AppRootPathView appRoot,
                 Widgets::StackLayoutParams const& params) -> SP::Expected<Widgets::StackPaths> {
    if (auto status = ensure_identifier(params.name, "stack name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    auto normalized_children = normalize_stack_children(space, appRoot, params.children);
    if (!normalized_children) {
        return std::unexpected(normalized_children.error());
    }

    Widgets::StackLayoutParams resolved = params;
    resolved.children = *normalized_children;

    auto layout_pair = compute_stack(space, resolved);
    if (!layout_pair) {
        return std::unexpected(layout_pair.error());
    }

    auto& computation = layout_pair->first;
    auto& runtime = layout_pair->second;

    auto bucket = build_stack_bucket(space, computation.state, runtime);
    if (!bucket) {
        return std::unexpected(bucket.error());
    }

    auto scenePath = ensure_stack_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    if (auto status = write_stack_metadata(space,
                                           widgetRoot->getPath(),
                                           resolved.style,
                                           resolved.children,
                                           computation.state); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, *bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::StackPaths paths = stack_paths_from_root(widgetRoot->getPath());
    paths.scene = *scenePath;
    return paths;
}

auto ReadStackLayout(PathSpace const& space,
                     Widgets::StackPaths const& paths) -> SP::Expected<Widgets::StackLayoutState> {
    auto state = space.read<Widgets::StackLayoutState, std::string>(paths.computed.getPath());
    if (!state) {
        return std::unexpected(state.error());
    }
    return *state;
}

auto DescribeStack(PathSpace const& space,
                   Widgets::StackPaths const& paths) -> SP::Expected<Widgets::StackLayoutParams> {
    Widgets::StackLayoutParams params{};

    auto style = space.read<Widgets::StackLayoutStyle, std::string>(paths.style.getPath());
    if (!style) {
        return std::unexpected(style.error());
    }
    params.style = *style;

    auto children = space.read<std::vector<Widgets::StackChildSpec>, std::string>(paths.children.getPath());
    if (!children) {
        return std::unexpected(children.error());
    }
    params.children = *children;

    auto root = std::string(paths.root.getPath());
    auto last_slash = root.find_last_of('/');
    if (last_slash != std::string::npos && last_slash + 1 < root.size()) {
        params.name = root.substr(last_slash + 1);
    } else {
        params.name = root;
    }
    return params;
}

auto UpdateStackLayout(PathSpace& space,
                       Widgets::StackPaths const& paths,
                       Widgets::StackLayoutParams const& params) -> SP::Expected<bool> {
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    AppRootPathView appRoot{appRootPath->getPath()};

    std::vector<Widgets::StackChildSpec> effective_children = params.children;
    if (effective_children.empty()) {
        auto current_children = space.read<std::vector<Widgets::StackChildSpec>, std::string>(paths.children.getPath());
        if (!current_children) {
            return std::unexpected(current_children.error());
        }
        effective_children = *current_children;
    }

    auto normalized_children = normalize_stack_children(space, appRoot, effective_children);
    if (!normalized_children) {
        return std::unexpected(normalized_children.error());
    }

    Widgets::StackLayoutParams resolved = params;
    resolved.children = *normalized_children;

    auto layout_pair = compute_stack(space, resolved);
    if (!layout_pair) {
        return std::unexpected(layout_pair.error());
    }

    auto& computation = layout_pair->first;
    auto& runtime = layout_pair->second;

    auto existing = ReadStackLayout(space, paths);
    if (!existing) {
        return std::unexpected(existing.error());
    }

    bool changed = !layout_equal(*existing, computation.state);

    auto bucket = build_stack_bucket(space, computation.state, runtime);
    if (!bucket) {
        return std::unexpected(bucket.error());
    }

    auto rootPath = std::string(paths.root.getPath());
    if (auto status = write_stack_metadata(space,
                                           rootPath,
                                           resolved.style,
                                           resolved.children,
                                           computation.state); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = publish_scene_snapshot(space, appRoot, paths.scene, *bucket); !status) {
        return std::unexpected(status.error());
    }

    if (changed) {
        if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
            return std::unexpected(mark.error());
        }
    }
    return changed;
}

auto ApplyTheme(WidgetTheme const& theme, ButtonParams& params) -> void {
    params.style = theme.button;
}

auto ApplyTheme(WidgetTheme const& theme, ToggleParams& params) -> void {
    params.style = theme.toggle;
}

auto ApplyTheme(WidgetTheme const& theme, SliderParams& params) -> void {
    params.style = theme.slider;
}

auto ApplyTheme(WidgetTheme const& theme, ListParams& params) -> void {
    params.style = theme.list;
}

auto ApplyTheme(WidgetTheme const& theme, TreeParams& params) -> void {
    params.style = theme.tree;
}

auto ApplyTheme(WidgetTheme const& theme, TextFieldParams& params) -> void {
    params.style = theme.text_field;
}

auto ApplyTheme(WidgetTheme const& theme, TextAreaParams& params) -> void {
    params.style = theme.text_area;
}

namespace {

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
    auto nodesValue = read_optional<std::vector<TreeNode>>(space, nodesPath);
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
    auto stateValue = space.read<TreeState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }

    auto stylePath = widget_root + "/meta/style";
    auto styleValue = space.read<TreeStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    auto nodesPath = widget_root + "/meta/nodes";
    auto nodesValue = space.read<std::vector<TreeNode>, std::string>(nodesPath);
    if (!nodesValue) {
        return std::unexpected(nodesValue.error());
    }

    TreeState desired = *stateValue;
    desired.focused = focused;
    if (focused) {
        if (desired.hovered_id.empty()) {
            if (!desired.selected_id.empty()) {
                desired.hovered_id = desired.selected_id;
            } else {
                auto it = std::find_if(nodesValue->begin(), nodesValue->end(), [](TreeNode const& node) {
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

    TreeStyle style = sanitize_tree_style(*styleValue);
    desired = sanitize_tree_state(desired, style, *nodesValue);

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    TreePaths paths{
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

} // namespace SP::UI::Builders::Widgets
