inline auto ensure_widget_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name,
                         std::string_view description) -> SP::Expected<ScenePath> {
    auto resolved = combine_relative(appRoot, std::string("scenes/widgets/") + std::string(name));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    ScenePath scenePath{resolved->getPath()};
    auto metaNamePath = make_scene_meta(scenePath, "name");
    auto existing = read_optional<std::string>(space, metaNamePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        if (auto status = replace_single<std::string>(space, metaNamePath, std::string{name}); !status) {
            return std::unexpected(status.error());
        }
        auto metaDescPath = make_scene_meta(scenePath, "description");
        if (auto status = replace_single<std::string>(space, metaDescPath, std::string(description)); !status) {
            return std::unexpected(status.error());
        }
    }
    return scenePath;
}

inline auto ensure_slider_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget slider");
}

inline auto ensure_list_scene(PathSpace& space,
                       AppRootPathView appRoot,
                       std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget list");
}

inline auto ensure_tree_scene(PathSpace& space,
                       AppRootPathView appRoot,
                       std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget tree");
}

inline auto ensure_stack_scene(PathSpace& space,
                        AppRootPathView appRoot,
                        std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget stack");
}

inline auto ensure_text_field_scene(PathSpace& space,
                             AppRootPathView appRoot,
                             std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget text field");
}

inline auto ensure_text_area_scene(PathSpace& space,
                            AppRootPathView appRoot,
                            std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget text area");
}

inline auto write_widget_kind(PathSpace& space,
                       std::string const& rootPath,
                       std::string_view kind) -> SP::Expected<void> {
    auto kindPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/kind");
    if (auto status = replace_single<std::string>(space, kindPath, std::string{kind}); !status) {
        return status;
    }
    return {};
}

inline auto write_button_metadata(PathSpace& space,
                           std::string const& rootPath,
                           std::string const& label,
                           Widgets::ButtonState const& state,
                           Widgets::ButtonStyle const& style) -> SP::Expected<void> {
    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::ButtonState>(space, statePath, state); !status) {
        return status;
    }

    auto labelPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/label");
    if (auto status = replace_single<std::string>(space, labelPath, label); !status) {
        return status;
    }

    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::ButtonStyle>(space, stylePath, style); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "button"); !status) {
        return status;
    }

    return {};
}

inline auto write_slider_metadata(PathSpace& space,
                           std::string const& rootPath,
                           Widgets::SliderState const& state,
                           Widgets::SliderStyle const& style,
                           Widgets::SliderRange const& range) -> SP::Expected<void> {
    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::SliderState>(space, statePath, state); !status) {
        return status;
    }

    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::SliderStyle>(space, stylePath, style); !status) {
        return status;
    }

    auto rangePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/range");
    if (auto status = replace_single<Widgets::SliderRange>(space, rangePath, range); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "slider"); !status) {
        return status;
    }

    DirtyRectHint footprint{};
    footprint.min_x = 0.0f;
    footprint.min_y = 0.0f;
    footprint.max_x = std::max(style.width, 0.0f);
    footprint.max_y = std::max(style.height, 0.0f);
    footprint = ensure_valid_hint(footprint);
    if (footprint.max_x > footprint.min_x && footprint.max_y > footprint.min_y) {
        auto footprintPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/footprint");
        if (auto status = replace_single<DirtyRectHint>(space, footprintPath, footprint); !status) {
            return status;
        }
    }

    return {};
}

inline auto write_list_metadata(PathSpace& space,
                         std::string const& rootPath,
                         Widgets::ListState const& state,
                         Widgets::ListStyle const& style,
                         std::vector<Widgets::ListItem> const& items) -> SP::Expected<void> {
    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::ListState>(space, statePath, state); !status) {
        return status;
    }
    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::ListStyle>(space, stylePath, style); !status) {
        return status;
    }
    auto itemsPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/items");
    if (auto status = replace_single<std::vector<Widgets::ListItem>>(space, itemsPath, items); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "list"); !status) {
        return status;
    }
    return {};
}

inline auto write_tree_metadata(PathSpace& space,
                         std::string const& rootPath,
                         Widgets::TreeState const& state,
                         Widgets::TreeStyle const& style,
                         std::vector<Widgets::TreeNode> const& nodes) -> SP::Expected<void> {
    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::TreeState>(space, statePath, state); !status) {
        return status;
    }

    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::TreeStyle>(space, stylePath, style); !status) {
        return status;
    }

    auto nodesPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/nodes");
    if (auto status = replace_single<std::vector<Widgets::TreeNode>>(space, nodesPath, nodes); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "tree"); !status) {
        return status;
    }
    return {};
}

inline auto write_stack_metadata(PathSpace& space,
                          std::string const& rootPath,
                          Widgets::StackLayoutStyle const& style,
                          std::vector<Widgets::StackChildSpec> const& children,
                          Widgets::StackLayoutState const& layout) -> SP::Expected<void> {
    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/layout/style");
    if (auto status = replace_single<Widgets::StackLayoutStyle>(space, stylePath, style); !status) {
        return status;
    }

    auto childrenPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/layout/children");
    if (auto status = replace_single<std::vector<Widgets::StackChildSpec>>(space, childrenPath, children); !status) {
        return status;
    }

    auto computedPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/layout/computed");
    if (auto status = replace_single<Widgets::StackLayoutState>(space, computedPath, layout); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "stack"); !status) {
        return status;
    }
    return {};
}

inline auto write_text_field_metadata(PathSpace& space,
                               std::string const& rootPath,
                               Widgets::TextFieldState const& state,
                               Widgets::TextFieldStyle const& style) -> SP::Expected<void> {
    auto sanitized_style = sanitize_text_field_style(style);
    auto sanitized_state = sanitize_text_field_state(state, sanitized_style);

    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::TextFieldState>(space, statePath, sanitized_state); !status) {
        return status;
    }

    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::TextFieldStyle>(space, stylePath, sanitized_style); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "text_field"); !status) {
        return status;
    }

    auto footprintPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/footprint");
    auto hint = text_input_dirty_hint(sanitized_style);
    if (hint.max_x > hint.min_x && hint.max_y > hint.min_y) {
        if (auto status = replace_single<DirtyRectHint>(space, footprintPath, hint); !status) {
            return status;
        }
    }
    return {};
}

inline auto write_text_area_metadata(PathSpace& space,
                              std::string const& rootPath,
                              Widgets::TextAreaState const& state,
                              Widgets::TextAreaStyle const& style) -> SP::Expected<void> {
    auto sanitized_style = sanitize_text_area_style(style);
    auto sanitized_state = sanitize_text_area_state(state, sanitized_style);

    auto statePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/state");
    if (auto status = replace_single<Widgets::TextAreaState>(space, statePath, sanitized_state); !status) {
        return status;
    }

    auto stylePath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/style");
    if (auto status = replace_single<Widgets::TextAreaStyle>(space, stylePath, sanitized_style); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "text_area"); !status) {
        return status;
    }

    auto footprintPath = SP::UI::Runtime::Widgets::WidgetSpacePath(rootPath, "/meta/footprint");
    auto hint = text_input_dirty_hint(sanitized_style);
    if (hint.max_x > hint.min_x && hint.max_y > hint.min_y) {
        if (auto status = replace_single<DirtyRectHint>(space, footprintPath, hint); !status) {
            return status;
        }
    }
    return {};
}
