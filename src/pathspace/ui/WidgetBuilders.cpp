#include "BuildersDetail.hpp"

namespace SP::UI::Builders::Widgets {

using namespace Detail;

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

    Widgets::ListStyle style = params.style;
    style.width = std::max(style.width, 96.0f);
    style.item_height = std::max(style.item_height, 24.0f);
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, std::min(style.width, style.item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u))) * 0.5f);
    style.border_thickness = std::clamp(style.border_thickness, 0.0f, style.item_height * 0.5f);
    style.item_typography.font_size = std::max(style.item_typography.font_size, 1.0f);
    style.item_typography.line_height = std::max(style.item_typography.line_height, style.item_typography.font_size);
    style.item_typography.letter_spacing = std::max(style.item_typography.letter_spacing, 0.0f);

    auto first_enabled = std::find_if(items.begin(), items.end(), [](auto const& entry) {
        return entry.enabled;
    });

    Widgets::ListState defaultState{};
    defaultState.selected_index = (first_enabled != items.end())
        ? static_cast<std::int32_t>(std::distance(items.begin(), first_enabled))
        : -1;
    defaultState.hovered_index = -1;
    defaultState.scroll_offset = 0.0f;

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
    auto bucket = build_button_bucket(*styleValue, new_state, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
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
    auto bucket = build_toggle_bucket(*styleValue, new_state, paths.root.getPath());
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
    auto bucket = build_slider_bucket(*styleValue, range, sanitized, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
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
        auto is_enabled = [&](std::int32_t candidate) -> bool {
            auto const idx = static_cast<std::size_t>(candidate);
            return idx < items.size() && items[idx].enabled;
        };
        if (is_enabled(index)) {
            return index;
        }
        for (std::int32_t forward = index + 1; forward < static_cast<std::int32_t>(items.size()); ++forward) {
            if (is_enabled(forward)) {
                return forward;
            }
        }
        for (std::int32_t backward = index - 1; backward >= 0; --backward) {
            if (is_enabled(backward)) {
                return backward;
            }
        }
        return -1;
    };

    Widgets::ListState sanitized = new_state;
    sanitized.enabled = new_state.enabled;
    sanitized.hovered_index = sanitize_index(new_state.hovered_index);
    sanitized.selected_index = sanitize_index(new_state.selected_index);
    sanitized.scroll_offset = std::max(new_state.scroll_offset, 0.0f);

    float const content_span = styleValue->item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u));
    float const max_scroll = std::max(0.0f, content_span - styleValue->item_height);
    sanitized.scroll_offset = std::clamp(sanitized.scroll_offset, 0.0f, max_scroll);

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
    auto bucket = build_list_bucket(*styleValue, items, sanitized, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto MakeDefaultWidgetTheme() -> WidgetTheme {
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

auto MakeSunsetWidgetTheme() -> WidgetTheme {
    WidgetTheme theme = MakeDefaultWidgetTheme();
    theme.button.background_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.button.text_color = {1.0f, 0.984f, 0.945f, 1.0f};
    theme.toggle.track_on_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.toggle.track_off_color = {0.60f, 0.44f, 0.38f, 1.0f};
    theme.toggle.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.fill_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.slider.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.label_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.list.background_color = {0.215f, 0.128f, 0.102f, 1.0f};
    theme.list.border_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_color = {0.266f, 0.166f, 0.138f, 1.0f};
    theme.list.item_hover_color = {0.422f, 0.248f, 0.198f, 1.0f};
    theme.list.item_selected_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.list.separator_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.heading_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.caption_color = {0.965f, 0.886f, 0.812f, 1.0f};
    theme.accent_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.muted_text_color = {0.855f, 0.698f, 0.612f, 1.0f};
    return theme;
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
    }
    return std::unexpected(make_error("unknown widget kind", SP::Error::Code::InvalidType));
}

} // namespace

namespace Focus {

auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath {
    return ConcretePath{std::string(appRoot.getPath()) + "/widgets/focus/current"};
}

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target) -> Config {
    Config config{
        .focus_state = FocusStatePath(appRoot),
        .auto_render_target = std::move(auto_render_target),
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
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    auto previous = *current;

    auto apply_focus = update_widget_focus(space, target_path, true);
    if (!apply_focus) {
        return std::unexpected(apply_focus.error());
    }
    bool changed = *apply_focus;

    if (!previous.has_value() || *previous != target_path) {
        if (previous.has_value()) {
            auto clear_prev = update_widget_focus(space, *previous, false);
            if (!clear_prev) {
                return std::unexpected(clear_prev.error());
            }
        }
        if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, target_path); !status) {
            return std::unexpected(status.error());
        }
        changed = true;
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

    bool changed = false;
    auto clear_prev = update_widget_focus(space, **current, false);
    if (!clear_prev) {
        return std::unexpected(clear_prev.error());
    }
    changed = *clear_prev;

    if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, std::string{}); !status) {
        return std::unexpected(status.error());
    }
    changed = true;

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

} // namespace Focus


namespace Bindings {

namespace {

auto compute_ops_queue(WidgetPath const& root) -> ConcretePath {
    return ConcretePath{std::string(root.getPath()) + "/ops/inbox/queue"};
}

auto build_options(WidgetPath const& root,
                   ConcretePathView targetPath,
                   DirtyRectHint hint,
                   bool auto_render) -> BindingOptions {
    BindingOptions options{
        .target = ConcretePath{std::string(targetPath.getPath())},
        .ops_queue = compute_ops_queue(root),
        .dirty_rect = ensure_valid_hint(hint),
        .auto_render = auto_render,
    };
    return options;
}

auto read_frame_index(PathSpace& space, std::string const& target) -> SP::Expected<std::uint64_t> {
    auto frame = read_optional<std::uint64_t>(space, target + "/output/v1/common/frameIndex");
    if (!frame) {
        return std::unexpected(frame.error());
    }
    if (frame->has_value()) {
        return **frame;
    }
    return std::uint64_t{0};
}

auto submit_dirty_hint(PathSpace& space,
                       BindingOptions const& options) -> SP::Expected<void> {
    auto const& rect = options.dirty_rect;
    if (rect.max_x <= rect.min_x || rect.max_y <= rect.min_y) {
        return {};
    }
    std::array<DirtyRectHint, 1> hints{rect};
    return Renderer::SubmitDirtyRects(space,
                                      SP::ConcretePathStringView{options.target.getPath()},
                                      std::span<const DirtyRectHint>(hints.data(), hints.size()));
}

auto schedule_auto_render(PathSpace& space,
                          BindingOptions const& options,
                          std::string_view reason) -> SP::Expected<void> {
    if (!options.auto_render) {
        return {};
    }
    auto frame_index = read_frame_index(space, options.target.getPath());
    if (!frame_index) {
        return std::unexpected(frame_index.error());
    }
    return enqueue_auto_render_event(space,
                                     options.target.getPath(),
                                     reason,
                                     *frame_index);
}

auto enqueue_widget_op(PathSpace& space,
                       BindingOptions const& options,
                       std::string const& widget_path,
                       WidgetOpKind kind,
                       PointerInfo const& pointer,
                       float value) -> SP::Expected<void> {
    WidgetOp op{};
    op.kind = kind;
    op.widget_path = widget_path;
    op.pointer = pointer;
    op.value = value;
    op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

    auto inserted = space.insert(options.ops_queue.getPath(), op);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto read_button_style(PathSpace& space,
                       ButtonPaths const& paths) -> SP::Expected<Widgets::ButtonStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ButtonStyle, std::string>(stylePath);
}

auto read_toggle_style(PathSpace& space,
                       TogglePaths const& paths) -> SP::Expected<Widgets::ToggleStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ToggleStyle, std::string>(stylePath);
}

auto read_slider_style(PathSpace& space,
                       SliderPaths const& paths) -> SP::Expected<Widgets::SliderStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::SliderStyle, std::string>(stylePath);
}

auto read_list_style(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<Widgets::ListStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ListStyle, std::string>(stylePath);
}

auto read_list_items(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<std::vector<Widgets::ListItem>> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    return space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
}

} // namespace

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo {
    PointerInfo info{};
    info.scene_x = hit.position.scene_x;
    info.scene_y = hit.position.scene_y;
    info.inside = hit.hit;
    info.primary = true;
    return info;
}

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ButtonBinding> {
    auto style = read_button_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ButtonBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ToggleBinding> {
    auto style = read_toggle_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ToggleBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<SliderBinding> {
    auto style = read_slider_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    SliderBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateListBinding(PathSpace& space,
                       AppRootPathView,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<ListBinding> {
    auto style = read_list_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    auto items = read_list_items(space, paths);
    if (!items) {
        return std::unexpected(items.error());
    }

    auto item_count = std::max<std::size_t>(items->size(), 1u);
    float height = style->item_height * static_cast<float>(item_count) + style->border_thickness * 2.0f;
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, height));
    ListBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Activate:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for button binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateButtonState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/button"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.pressed ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Toggle:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for toggle binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateToggleState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/toggle"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.checked ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::SliderBegin:
    case WidgetOpKind::SliderUpdate:
    case WidgetOpKind::SliderCommit:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for slider binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateSliderState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto current_state = space.read<SliderState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/slider"); !status) {
            return std::unexpected(status.error());
        }
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        current_state->value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer,
                  std::int32_t item_index,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::ListHover:
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
    case WidgetOpKind::ListScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for list binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    Widgets::ListState desired = new_state;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        desired.hovered_index = item_index;
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        if (item_index >= 0) {
            desired.selected_index = item_index;
        }
        break;
    case WidgetOpKind::ListScroll:
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateListState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space,
                                               binding.options,
                                               "widget/list"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        op_value = static_cast<float>(updated_state->hovered_index);
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        op_value = static_cast<float>(updated_state->selected_index);
        break;
    case WidgetOpKind::ListScroll:
        op_value = updated_state->scroll_offset;
        break;
    default:
        break;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

} // namespace Bindings

namespace Reducers {

namespace {

auto to_widget_action(Bindings::WidgetOp const& op) -> WidgetAction {
    WidgetAction action{};
    action.kind = op.kind;
    action.widget_path = op.widget_path;
    action.pointer = op.pointer;
    action.analog_value = op.value;
    action.sequence = op.sequence;
    action.timestamp_ns = op.timestamp_ns;

    switch (op.kind) {
    case Bindings::WidgetOpKind::ListHover:
    case Bindings::WidgetOpKind::ListSelect:
    case Bindings::WidgetOpKind::ListActivate:
        action.discrete_index = static_cast<std::int32_t>(std::llround(op.value));
        break;
    default:
        action.discrete_index = -1;
        break;
    }

    return action;
}

} // namespace

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/actions/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions) -> SP::Expected<std::vector<WidgetAction>> {
    std::vector<WidgetAction> actions;
    if (max_actions == 0) {
        return actions;
    }

    auto queue_path = std::string(ops_queue.getPath());
    if (queue_path.empty()) {
        return actions;
    }

    for (std::size_t processed = 0; processed < max_actions; ++processed) {
        auto taken = space.take<Bindings::WidgetOp, std::string>(queue_path);
        if (taken) {
            actions.push_back(to_widget_action(*taken));
            continue;
        }

        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }

    return actions;
}

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void> {
    if (actions.empty()) {
        return {};
    }

    auto queue_path = std::string(actions_queue.getPath());
    if (queue_path.empty()) {
        return {};
    }

    for (auto const& action : actions) {
        auto inserted = space.insert(queue_path, action);
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }

    return {};
}

} // namespace Reducers


} // namespace SP::UI::Builders::Widgets
