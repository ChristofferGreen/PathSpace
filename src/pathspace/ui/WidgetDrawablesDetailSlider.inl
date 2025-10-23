struct SliderSnapshotConfig {
    float width = 240.0f;
    float height = 32.0f;
    float track_height = 6.0f;
    float thumb_radius = 10.0f;
    float min = 0.0f;
    float max = 1.0f;
    float value = 0.5f;
    std::array<float, 4> track_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> fill_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

inline auto make_slider_bucket(SliderSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x51D301u, 0x51D302u, 0x51D303u};
    bucket.world_transforms = {make_identity_transform(), make_identity_transform(), make_identity_transform()};

    float const clamped_min = std::min(config.min, config.max);
    float const clamped_max = std::max(config.min, config.max);
    float const range = std::max(clamped_max - clamped_min, 1e-6f);
    float const clamped_value = std::clamp(config.value, clamped_min, clamped_max);
    float progress = (clamped_value - clamped_min) / range;
    progress = std::clamp(progress, 0.0f, 1.0f);

    float const width = std::max(config.width, 1.0f);
    float const height = std::max(config.height, 1.0f);
    float const track_height = std::clamp(config.track_height, 1.0f, height);
    float const thumb_radius = std::clamp(config.thumb_radius, track_height * 0.5f, height * 0.5f);

    float const center_y = height * 0.5f;
    float const track_half = track_height * 0.5f;
    float const track_radius = track_half;
    float const fill_width = std::max(progress * width, 0.0f);
    float thumb_x = progress * width;
    thumb_x = std::clamp(thumb_x, thumb_radius, width - thumb_radius);

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {width * 0.5f, center_y, 0.0f};
    trackSphere.radius = std::sqrt(trackSphere.center[0] * trackSphere.center[0]
                                   + track_half * track_half);

    SceneData::BoundingSphere fillSphere{};
    fillSphere.center = {std::max(fill_width * 0.5f, 0.0f), center_y, 0.0f};
    fillSphere.radius = std::sqrt(fillSphere.center[0] * fillSphere.center[0]
                                  + track_half * track_half);

    SceneData::BoundingSphere thumbSphere{};
    thumbSphere.center = {thumb_x, center_y, 0.0f};
    thumbSphere.radius = thumb_radius;

    bucket.bounds_spheres = {trackSphere, fillSphere, thumbSphere};

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, center_y - track_half, 0.0f};
    trackBox.max = {width, center_y + track_half, 0.0f};

    SceneData::BoundingBox fillBox{};
    fillBox.min = {0.0f, center_y - track_half, 0.0f};
    fillBox.max = {fill_width, center_y + track_half, 0.0f};

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumb_x - thumb_radius, center_y - thumb_radius, 0.0f};
    thumbBox.max = {thumb_x + thumb_radius, center_y + thumb_radius, 0.0f};

    bucket.bounds_boxes = {trackBox, fillBox, thumbBox};
    bucket.bounds_box_valid = {1, 1, 1};
    bucket.layers = {0, 1, 2};
    bucket.z_values = {0.0f, 0.05f, 0.1f};
    bucket.material_ids = {0, 0, 0};
    bucket.pipeline_flags = {0, 0, 0};
    bucket.visibility = {1, 1, 1};
    bucket.command_offsets = {0, 1, 2};
    bucket.command_counts = {1, 1, 1};
    bucket.opaque_indices = {0, 1, 2};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1, -1, -1};
    bucket.authoring_map = {
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[0],
            make_widget_authoring_id(authoring_root, "slider/track"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[1],
            make_widget_authoring_id(authoring_root, "slider/fill"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[2],
            make_widget_authoring_id(authoring_root, "slider/thumb"),
            0,
            0},
    };
    bucket.drawable_fingerprints = {0x51D301u, 0x51D302u, 0x51D303u};

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = center_y - track_half;
    trackRect.max_x = width;
    trackRect.max_y = center_y + track_half;
    trackRect.radius_top_left = track_radius;
    trackRect.radius_top_right = track_radius;
    trackRect.radius_bottom_right = track_radius;
    trackRect.radius_bottom_left = track_radius;
    trackRect.color = config.track_color;

    SceneData::RectCommand fillRect{};
    fillRect.min_x = 0.0f;
    fillRect.min_y = center_y - track_half;
    fillRect.max_x = fill_width;
    fillRect.max_y = center_y + track_half;
    fillRect.color = config.fill_color;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumb_radius;
    thumbRect.radius_top_right = thumb_radius;
    thumbRect.radius_bottom_right = thumb_radius;
    thumbRect.radius_bottom_left = thumb_radius;
    thumbRect.color = config.thumb_color;

    auto payload_track = sizeof(SceneData::RoundedRectCommand);
    auto payload_fill = sizeof(SceneData::RectCommand);
    auto payload_thumb = sizeof(SceneData::RoundedRectCommand);
    bucket.command_payload.resize(payload_track + payload_fill + payload_thumb);
    std::uint8_t* payload_ptr = bucket.command_payload.data();
    std::memcpy(payload_ptr, &trackRect, payload_track);
    std::memcpy(payload_ptr + payload_track, &fillRect, payload_fill);
    std::memcpy(payload_ptr + payload_track + payload_fill, &thumbRect, payload_thumb);

    bucket.command_kinds = {
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
    };

    return bucket;
}

inline auto clamp_slider_value(Widgets::SliderRange const& range, float value) -> float {
    float minimum = std::min(range.minimum, range.maximum);
    float maximum = std::max(range.minimum, range.maximum);
    if (minimum == maximum) {
        maximum = minimum + 1.0f;
    }
    float clamped = std::clamp(value, minimum, maximum);
    if (range.step > 0.0f) {
        float steps = std::round((clamped - minimum) / range.step);
        clamped = minimum + steps * range.step;
        clamped = std::clamp(clamped, minimum, maximum);
    }
    return clamped;
}

inline auto build_slider_bucket(Widgets::SliderStyle const& style,
                         Widgets::SliderRange const& range,
                         Widgets::SliderState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    Widgets::SliderState applied = state;
    applied.value = clamp_slider_value(range, state.value);

    SliderSnapshotConfig config{
        .width = std::max(style.width, 1.0f),
        .height = std::max(style.height, 16.0f),
        .track_height = std::clamp(style.track_height, 1.0f, style.height),
        .thumb_radius = std::clamp(style.thumb_radius,
                                   style.track_height * 0.5f,
                                   style.height * 0.5f),
        .min = range.minimum,
        .max = range.maximum,
        .value = applied.value,
        .track_color = style.track_color,
        .fill_color = style.fill_color,
        .thumb_color = style.thumb_color,
    };

    if (!applied.enabled) {
        config.track_color = scale_alpha(desaturate_color(config.track_color, 0.6f), 0.5f);
        config.fill_color = scale_alpha(desaturate_color(config.fill_color, 0.6f), 0.5f);
        config.thumb_color = scale_alpha(desaturate_color(config.thumb_color, 0.6f), 0.5f);
    } else if (applied.dragging) {
        config.fill_color = lighten_color(config.fill_color, 0.10f);
        config.thumb_color = darken_color(config.thumb_color, 0.12f);
    } else if (applied.hovered) {
        config.fill_color = lighten_color(config.fill_color, 0.08f);
        config.thumb_color = lighten_color(config.thumb_color, 0.06f);
    }

    return make_slider_bucket(config, authoring_root);
}

inline auto build_slider_bucket(Widgets::SliderStyle const& style,
                         Widgets::SliderRange const& range,
                         Widgets::SliderState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_slider_bucket(style, range, state, {});
}

inline auto publish_slider_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::SliderStyle const& style,
                                 Widgets::SliderRange const& range,
                                 Widgets::SliderState const& default_state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::SliderState slider_state;
        ScenePath* target = nullptr;
    };

    Widgets::SliderState idle = default_state;
    Widgets::SliderState hover = idle;
    hover.hovered = true;
    Widgets::SliderState pressed = idle;
    pressed.dragging = true;
    pressed.hovered = true;
    Widgets::SliderState disabled = idle;
    disabled.enabled = false;

    Variant variants[] = {
        {"idle", idle, &scenes.idle},
        {"hover", hover, &scenes.hover},
        {"pressed", pressed, &scenes.pressed},
        {"disabled", disabled, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget slider state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_slider_bucket(style, range, variant.slider_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}
