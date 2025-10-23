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

inline auto write_widget_kind(PathSpace& space,
                       std::string const& rootPath,
                       std::string_view kind) -> SP::Expected<void> {
    auto kindPath = rootPath + "/meta/kind";
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
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ButtonState>(space, statePath, state); !status) {
        return status;
    }

    auto labelPath = rootPath + "/meta/label";
    if (auto status = replace_single<std::string>(space, labelPath, label); !status) {
        return status;
    }

    auto stylePath = rootPath + "/meta/style";
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
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::SliderState>(space, statePath, state); !status) {
        return status;
    }

    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::SliderStyle>(space, stylePath, style); !status) {
        return status;
    }

    auto rangePath = rootPath + "/meta/range";
    if (auto status = replace_single<Widgets::SliderRange>(space, rangePath, range); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "slider"); !status) {
        return status;
    }

    return {};
}

inline auto write_list_metadata(PathSpace& space,
                         std::string const& rootPath,
                         Widgets::ListState const& state,
                         Widgets::ListStyle const& style,
                         std::vector<Widgets::ListItem> const& items) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ListState>(space, statePath, state); !status) {
        return status;
    }
    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::ListStyle>(space, stylePath, style); !status) {
        return status;
    }
    auto itemsPath = rootPath + "/meta/items";
    if (auto status = replace_single<std::vector<Widgets::ListItem>>(space, itemsPath, items); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "list"); !status) {
        return status;
    }
    return {};
}
