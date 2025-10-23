#include "WidgetDetail.hpp"

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

} // namespace SP::UI::Builders::Widgets
