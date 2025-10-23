struct ButtonSnapshotConfig {
    float width = 200.0f;
    float height = 48.0f;
    float corner_radius = 6.0f;
    std::array<float, 4> color{0.176f, 0.353f, 0.914f, 1.0f};
};

inline auto make_button_bucket(ButtonSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xB17B0001ull};
    bucket.world_transforms = {make_identity_transform()};

    SceneData::BoundingSphere sphere{};
    float center_x = config.width * 0.5f;
    float center_y = config.height * 0.5f;
    sphere.center = {center_x, center_y, 0.0f};
    sphere.radius = std::sqrt(center_x * center_x + center_y * center_y);
    bucket.bounds_spheres = {sphere};

    SceneData::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {config.width, config.height, 0.0f};
    bucket.bounds_boxes = {box};
    bucket.bounds_box_valid = {1};

    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.front(),
        make_widget_authoring_id(authoring_root, "button/background"),
        0,
        0}};
    bucket.drawable_fingerprints = {0xB17B0001ull};

    float radius_limit = std::min(config.width, config.height) * 0.5f;
    float clamped_radius = std::clamp(config.corner_radius, 0.0f, radius_limit);

    if (clamped_radius > 0.0f) {
        SceneData::RoundedRectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = config.width;
        rect.max_y = config.height;
        rect.radius_top_left = clamped_radius;
        rect.radius_top_right = clamped_radius;
        rect.radius_bottom_left = clamped_radius;
        rect.radius_bottom_right = clamped_radius;
        rect.color = config.color;

        bucket.command_payload.resize(sizeof(SceneData::RoundedRectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RoundedRectCommand));
        bucket.command_kinds = {
            static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        };
    } else {
        SceneData::RectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = config.width;
        rect.max_y = config.height;
        rect.color = config.color;

        bucket.command_payload.resize(sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RectCommand));
        bucket.command_kinds = {
            static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect),
        };
    }

    return bucket;
}

inline auto button_background_color(Widgets::ButtonStyle const& style,
                             Widgets::ButtonState const& state) -> std::array<float, 4> {
    auto base = style.background_color;
    if (!state.enabled) {
        return scale_alpha(desaturate_color(base, 0.65f), 0.55f);
    }
    if (state.pressed) {
        return darken_color(base, 0.18f);
    }
    if (state.hovered) {
        return lighten_color(base, 0.12f);
    }
    return base;
}

inline auto build_button_bucket(Widgets::ButtonStyle const& style,
                         Widgets::ButtonState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    float width = std::max(style.width, 1.0f);
    float height = std::max(style.height, 1.0f);
    float radius_limit = std::min(width, height) * 0.5f;
    float corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    ButtonSnapshotConfig config{
        .width = width,
        .height = height,
        .corner_radius = corner_radius,
        .color = button_background_color(style, state),
    };
    return make_button_bucket(config, authoring_root);
}

inline auto build_button_bucket(Widgets::ButtonStyle const& style,
                         Widgets::ButtonState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_button_bucket(style, state, {});
}

inline auto publish_button_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::ButtonStyle const& style) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::ButtonState button_state;
        ScenePath* target = nullptr;
    };

    Variant variants[] = {
        {"idle", Widgets::ButtonState{}, &scenes.idle},
        {"hover", Widgets::ButtonState{.hovered = true}, &scenes.hover},
        {"pressed", Widgets::ButtonState{.pressed = true, .hovered = true}, &scenes.pressed},
        {"disabled", Widgets::ButtonState{.enabled = false}, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget button state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_button_bucket(style, variant.button_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}
