inline auto button_states_equal(Widgets::ButtonState const& lhs,
                         Widgets::ButtonState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.pressed == rhs.pressed
        && lhs.hovered == rhs.hovered;
}

inline auto toggle_states_equal(Widgets::ToggleState const& lhs,
                         Widgets::ToggleState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.hovered == rhs.hovered
        && lhs.checked == rhs.checked;
}

inline auto slider_states_equal(Widgets::SliderState const& lhs,
                         Widgets::SliderState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.hovered == rhs.hovered
        && lhs.dragging == rhs.dragging
        && lhs.value == rhs.value;
}

inline auto list_states_equal(Widgets::ListState const& lhs,
                       Widgets::ListState const& rhs) -> bool {
    auto equal_float = [](float a, float b) {
        return std::fabs(a - b) <= 1e-6f;
    };
    return lhs.enabled == rhs.enabled
        && lhs.hovered_index == rhs.hovered_index
        && lhs.selected_index == rhs.selected_index
        && equal_float(lhs.scroll_offset, rhs.scroll_offset);
}

inline auto make_default_dirty_rect(float width, float height) -> DirtyRectHint {
    DirtyRectHint hint{};
    hint.min_x = 0.0f;
    hint.min_y = 0.0f;
    hint.max_x = std::max(width, 1.0f);
    hint.max_y = std::max(height, 1.0f);
    return hint;
}

inline auto ensure_valid_hint(DirtyRectHint hint) -> DirtyRectHint {
    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return DirtyRectHint{0.0f, 0.0f, 0.0f, 0.0f};
    }
    return hint;
}

inline auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

inline auto mix_color(std::array<float, 4> base,
               std::array<float, 4> target,
               float amount) -> std::array<float, 4> {
    amount = clamp_unit(amount);
    std::array<float, 4> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = clamp_unit(base[i] * (1.0f - amount) + target[i] * amount);
    }
    out[3] = clamp_unit(base[3] * (1.0f - amount) + target[3] * amount);
    return out;
}

inline auto lighten_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

inline auto darken_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {0.0f, 0.0f, 0.0f, color[3]}, amount);
}

inline auto desaturate_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    auto gray = std::array<float, 4>{0.5f, 0.5f, 0.5f, color[3]};
    return mix_color(color, gray, amount);
}

inline auto scale_alpha(std::array<float, 4> color, float factor) -> std::array<float, 4> {
    color[3] = clamp_unit(color[3] * factor);
    return color;
}

inline auto make_identity_transform() -> SceneData::Transform {
    SceneData::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

inline auto make_widget_authoring_id(std::string_view base_path,
                               std::string_view suffix) -> std::string {
    if (base_path.empty()) {
        std::string id = "widget/";
        id.append(suffix);
        return id;
    }
    std::string id;
    id.reserve(base_path.size() + std::size_t{11} + suffix.size());
    id.append(base_path);
    if (!base_path.empty() && base_path.back() != '/') {
        id.push_back('/');
    }
    id.append("authoring/");
    id.append(suffix);
    return id;
}

inline auto ensure_widget_root(PathSpace& /*space*/,
                        AppRootPathView appRoot) -> SP::Expected<ConcretePath> {
    return combine_relative(appRoot, "widgets");
}

inline auto publish_scene_snapshot(PathSpace& space,
                            AppRootPathView appRoot,
                            ScenePath const& scenePath,
                            SceneData::DrawableBucketSnapshot const& bucket,
                            std::string_view author = "widgets",
                            std::string_view tool_version = "widgets-toolkit") -> SP::Expected<void> {
    SceneData::SceneSnapshotBuilder builder{space, appRoot, scenePath};
    SceneData::SnapshotPublishOptions options{};
    options.metadata.author = std::string(author);
    options.metadata.tool_version = std::string(tool_version);
    options.metadata.created_at = std::chrono::system_clock::now();
    options.metadata.drawable_count = bucket.drawable_ids.size();
    options.metadata.command_count = bucket.command_kinds.size();

    auto revision = builder.publish(options, bucket);
    if (!revision) {
        return std::unexpected(revision.error());
    }

    auto ready = Scene::WaitUntilReady(space, scenePath, std::chrono::milliseconds{50});
    if (!ready) {
        return std::unexpected(ready.error());
    }
    return {};
}

inline auto ensure_widget_state_scene(PathSpace& space,
                               AppRootPathView appRoot,
                               std::string_view name,
                               std::string_view state,
                               std::string_view description_prefix) -> SP::Expected<ScenePath> {
    auto spec = std::string("scenes/widgets/") + std::string(name) + "/states/" + std::string(state);
    auto resolved = combine_relative(appRoot, spec);
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
        if (auto status = replace_single<std::string>(space, metaNamePath, std::string(state)); !status) {
            return std::unexpected(status.error());
        }
        auto metaDescPath = make_scene_meta(scenePath, "description");
        auto description = std::string(description_prefix) + " (" + std::string(state) + ")";
        if (auto status = replace_single<std::string>(space, metaDescPath, description); !status) {
            return std::unexpected(status.error());
        }
    }
    return scenePath;
}
