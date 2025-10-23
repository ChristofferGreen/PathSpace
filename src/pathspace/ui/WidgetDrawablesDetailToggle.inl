struct ToggleSnapshotConfig {
    float width = 56.0f;
    float height = 32.0f;
    bool checked = false;
    std::array<float, 4> track_off_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> track_on_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

inline auto make_toggle_bucket(ToggleSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x701701u, 0x701702u};
    bucket.world_transforms = {make_identity_transform(), make_identity_transform()};

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {config.width * 0.5f, config.height * 0.5f, 0.0f};
    trackSphere.radius = std::sqrt(trackSphere.center[0] * trackSphere.center[0]
                                   + trackSphere.center[1] * trackSphere.center[1]);

    SceneData::BoundingSphere thumbSphere{};
    float thumbRadius = config.height * 0.5f - 2.0f;
    float thumbCenterX = config.checked ? (config.width - thumbRadius - 2.0f) : (thumbRadius + 2.0f);
    thumbSphere.center = {thumbCenterX, config.height * 0.5f, 0.0f};
    thumbSphere.radius = thumbRadius;

    bucket.bounds_spheres = {trackSphere, thumbSphere};

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, 0.0f, 0.0f};
    trackBox.max = {config.width, config.height, 0.0f};

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumbCenterX - thumbRadius, config.height * 0.5f - thumbRadius, 0.0f};
    thumbBox.max = {thumbCenterX + thumbRadius, config.height * 0.5f + thumbRadius, 0.0f};

    bucket.bounds_boxes = {trackBox, thumbBox};
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 1};
    bucket.z_values = {0.0f, 0.1f};
    bucket.material_ids = {0, 0};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0, 1};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[0],
            make_widget_authoring_id(authoring_root, "toggle/track"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[1],
            make_widget_authoring_id(authoring_root, "toggle/thumb"),
            0,
            0},
    };
    bucket.drawable_fingerprints = {0x701701u, 0x701702u};

    auto trackColor = config.checked ? config.track_on_color : config.track_off_color;

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = 0.0f;
    trackRect.max_x = config.width;
    trackRect.max_y = config.height;
    trackRect.radius_top_left = config.height * 0.5f;
    trackRect.radius_top_right = config.height * 0.5f;
    trackRect.radius_bottom_right = config.height * 0.5f;
    trackRect.radius_bottom_left = config.height * 0.5f;
    trackRect.color = trackColor;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumbRadius;
    thumbRect.radius_top_right = thumbRadius;
    thumbRect.radius_bottom_right = thumbRadius;
    thumbRect.radius_bottom_left = thumbRadius;
    thumbRect.color = config.thumb_color;

    auto payload_track = sizeof(SceneData::RoundedRectCommand);
    auto payload_thumb = sizeof(SceneData::RoundedRectCommand);
    bucket.command_payload.resize(payload_track + payload_thumb);
    std::memcpy(bucket.command_payload.data(), &trackRect, payload_track);
    std::memcpy(bucket.command_payload.data() + payload_track, &thumbRect, payload_thumb);
    bucket.command_kinds = {
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
    };
    return bucket;
}

inline auto build_toggle_bucket(Widgets::ToggleStyle const& style,
                         Widgets::ToggleState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    ToggleSnapshotConfig config{
        .width = std::max(style.width, 1.0f),
        .height = std::max(style.height, 1.0f),
        .checked = state.checked,
        .track_off_color = style.track_off_color,
        .track_on_color = style.track_on_color,
        .thumb_color = style.thumb_color,
    };

    if (!state.enabled) {
        config.track_off_color = scale_alpha(desaturate_color(config.track_off_color, 0.6f), 0.5f);
        config.track_on_color = scale_alpha(desaturate_color(config.track_on_color, 0.6f), 0.5f);
        config.thumb_color = scale_alpha(desaturate_color(config.thumb_color, 0.6f), 0.5f);
    } else if (state.hovered) {
        config.track_off_color = lighten_color(config.track_off_color, 0.12f);
        config.track_on_color = lighten_color(config.track_on_color, 0.10f);
        config.thumb_color = lighten_color(config.thumb_color, 0.08f);
    }
    if (state.checked && state.hovered) {
        config.track_on_color = lighten_color(config.track_on_color, 0.08f);
    }

    return make_toggle_bucket(config, authoring_root);
}

inline auto build_toggle_bucket(Widgets::ToggleStyle const& style,
                         Widgets::ToggleState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_toggle_bucket(style, state, {});
}

inline auto publish_toggle_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::ToggleStyle const& style) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::ToggleState toggle_state;
        ScenePath* target = nullptr;
    };

    Variant variants[] = {
        {"idle", Widgets::ToggleState{}, &scenes.idle},
        {"hover", Widgets::ToggleState{.hovered = true}, &scenes.hover},
        {"pressed", Widgets::ToggleState{.checked = true, .hovered = true}, &scenes.pressed},
        {"disabled", Widgets::ToggleState{.enabled = false}, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget toggle state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_toggle_bucket(style, variant.toggle_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}
