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

inline auto ensure_widget_root(PathSpace& /*space*/,
                        AppRootPathView appRoot) -> SP::Expected<ConcretePath> {
    return combine_relative(appRoot, "widgets");
}

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

struct ListSnapshotConfig {
    float width = 240.0f;
    float item_height = 36.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    std::size_t item_count = 0;
    std::int32_t selected_index = -1;
    std::int32_t hovered_index = -1;
    std::array<float, 4> background_color{};
    std::array<float, 4> border_color{};
    std::array<float, 4> item_color{};
    std::array<float, 4> item_hover_color{};
    std::array<float, 4> item_selected_color{};
    std::array<float, 4> separator_color{};
};

inline auto make_list_bucket(ListSnapshotConfig const& config,
                      std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    auto const item_count = static_cast<std::size_t>(std::max<std::int32_t>(static_cast<std::int32_t>(config.item_count), 0));
    float const base_height = std::max(config.item_height * static_cast<float>(std::max<std::size_t>(item_count, 1u)),
                                       config.item_height);
    float const height = base_height + config.border_thickness * 2.0f;
    float const width = std::max(config.width, 1.0f);

    SceneData::DrawableBucketSnapshot bucket{};
    // background + per-item rectangles
    std::size_t drawable_count = 1 + std::max<std::size_t>(item_count, 1u);
    bucket.drawable_ids.reserve(drawable_count);
    bucket.world_transforms.reserve(drawable_count);
    bucket.bounds_spheres.reserve(drawable_count);
    bucket.bounds_boxes.reserve(drawable_count);
    bucket.bounds_box_valid.reserve(drawable_count);
    bucket.layers.reserve(drawable_count);
    bucket.z_values.reserve(drawable_count);
    bucket.material_ids.reserve(drawable_count);
    bucket.pipeline_flags.reserve(drawable_count);
    bucket.visibility.reserve(drawable_count);
    bucket.command_offsets.reserve(drawable_count);
    bucket.command_counts.reserve(drawable_count);
    bucket.opaque_indices.reserve(drawable_count);
    bucket.clip_head_indices.reserve(drawable_count);
    bucket.authoring_map.reserve(drawable_count);
    bucket.drawable_fingerprints.reserve(drawable_count);

    auto push_common = [&](std::uint64_t drawable_id,
                           SceneData::BoundingBox const& box,
                           SceneData::BoundingSphere const& sphere,
                           int layer,
                           float z) {
        bucket.drawable_ids.push_back(drawable_id);
        bucket.world_transforms.push_back(make_identity_transform());
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);
        bucket.bounds_spheres.push_back(sphere);
        bucket.layers.push_back(layer);
        bucket.z_values.push_back(z);
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_counts.push_back(1);
        bucket.opaque_indices.push_back(static_cast<std::uint32_t>(bucket.opaque_indices.size()));
        bucket.clip_head_indices.push_back(-1);
    };

    // Background rounded rect.
    SceneData::BoundingBox background_box{};
    background_box.min = {0.0f, 0.0f, 0.0f};
    background_box.max = {width, height, 0.0f};

    SceneData::BoundingSphere background_sphere{};
    background_sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    background_sphere.radius = std::sqrt(background_sphere.center[0] * background_sphere.center[0]
                                         + background_sphere.center[1] * background_sphere.center[1]);

    push_common(0x11570001ull, background_box, background_sphere, 0, 0.0f);
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

    SceneData::RoundedRectCommand background{};
    background.min_x = 0.0f;
    background.min_y = 0.0f;
    background.max_x = width;
    background.max_y = height;
    background.radius_top_left = config.corner_radius;
    background.radius_top_right = config.corner_radius;
    background.radius_bottom_right = config.corner_radius;
    background.radius_bottom_left = config.corner_radius;
    background.color = config.background_color;

    auto const background_offset = bucket.command_payload.size();
    bucket.command_payload.resize(background_offset + sizeof(SceneData::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + background_offset,
                &background,
                sizeof(SceneData::RoundedRectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.back(),
        make_widget_authoring_id(authoring_root, "list/background"),
        0,
        0});
    bucket.drawable_fingerprints.push_back(0x11570001ull);

    // Item rows.
    std::size_t const rows = std::max<std::size_t>(item_count, 1u);
    float const content_top = config.border_thickness;
    for (std::size_t index = 0; index < rows; ++index) {
        float const top = content_top + config.item_height * static_cast<float>(index);
        float const bottom = top + config.item_height;
        SceneData::BoundingBox row_box{};
        row_box.min = {config.border_thickness, top, 0.0f};
        row_box.max = {width - config.border_thickness, bottom, 0.0f};

        SceneData::BoundingSphere row_sphere{};
        row_sphere.center = {(row_box.min[0] + row_box.max[0]) * 0.5f,
                             (row_box.min[1] + row_box.max[1]) * 0.5f,
                             0.0f};
        row_sphere.radius = std::sqrt(std::pow(row_box.max[0] - row_sphere.center[0], 2.0f)
                                      + std::pow(row_box.max[1] - row_sphere.center[1], 2.0f));

        std::uint64_t drawable_id = 0x11570010ull + static_cast<std::uint64_t>(index);
        push_common(drawable_id, row_box, row_sphere, 1, 0.05f + static_cast<float>(index) * 0.001f);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));

        std::array<float, 4> color = config.item_color;
        if (static_cast<std::int32_t>(index) == config.selected_index) {
            color = config.item_selected_color;
        } else if (static_cast<std::int32_t>(index) == config.hovered_index) {
            color = config.item_hover_color;
        }

        SceneData::RectCommand row_rect{};
        row_rect.min_x = row_box.min[0];
        row_rect.min_y = row_box.min[1];
        row_rect.max_x = row_box.max[0];
        row_rect.max_y = row_box.max[1];
        row_rect.color = color;

        auto const payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &row_rect,
                    sizeof(SceneData::RectCommand));

        auto label = make_widget_authoring_id(authoring_root,
                                              std::string("list/item/") + std::to_string(index));
        bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
            drawable_id, std::move(label), 0, 0});
        bucket.drawable_fingerprints.push_back(drawable_id);
    }

    bucket.alpha_indices.clear();
   bucket.layer_indices.clear();
    return bucket;
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

inline auto first_enabled_index(std::vector<Widgets::ListItem> const& items) -> std::int32_t {
    auto it = std::find_if(items.begin(), items.end(), [](auto const& item) {
        return item.enabled;
    });
    if (it == items.end()) {
        return -1;
    }
    return static_cast<std::int32_t>(std::distance(items.begin(), it));
}

inline auto build_list_bucket(Widgets::ListStyle const& style,
                       std::vector<Widgets::ListItem> const& items,
                       Widgets::ListState const& state,
                       std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    Widgets::ListStyle appliedStyle = style;
    Widgets::ListState appliedState = state;
    if (!appliedState.enabled) {
        appliedStyle.background_color = scale_alpha(desaturate_color(appliedStyle.background_color, 0.6f), 0.6f);
        appliedStyle.border_color = scale_alpha(desaturate_color(appliedStyle.border_color, 0.6f), 0.6f);
        appliedStyle.item_color = scale_alpha(desaturate_color(appliedStyle.item_color, 0.6f), 0.6f);
        appliedStyle.item_hover_color = scale_alpha(desaturate_color(appliedStyle.item_hover_color, 0.6f), 0.6f);
        appliedStyle.item_selected_color = scale_alpha(desaturate_color(appliedStyle.item_selected_color, 0.6f), 0.6f);
        appliedStyle.separator_color = scale_alpha(desaturate_color(appliedStyle.separator_color, 0.6f), 0.6f);
        appliedStyle.item_text_color = scale_alpha(desaturate_color(appliedStyle.item_text_color, 0.6f), 0.6f);
        appliedState.hovered_index = -1;
        appliedState.selected_index = -1;
    }

    ListSnapshotConfig config{
        .width = std::max(appliedStyle.width, 96.0f),
        .item_height = std::max(appliedStyle.item_height, 24.0f),
        .corner_radius = std::clamp(appliedStyle.corner_radius,
                                    0.0f,
                                    std::min(appliedStyle.width,
                                             appliedStyle.item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u))) * 0.5f),
        .border_thickness = std::clamp(appliedStyle.border_thickness,
                                       0.0f,
                                       appliedStyle.item_height * 0.5f),
        .item_count = items.size(),
        .selected_index = appliedState.selected_index,
        .hovered_index = appliedState.hovered_index,
        .background_color = appliedStyle.background_color,
        .border_color = appliedStyle.border_color,
        .item_color = appliedStyle.item_color,
        .item_hover_color = appliedStyle.item_hover_color,
        .item_selected_color = appliedStyle.item_selected_color,
        .separator_color = appliedStyle.separator_color,
    };

    return make_list_bucket(config);
}

inline auto build_list_bucket(Widgets::ListStyle const& style,
                       std::vector<Widgets::ListItem> const& items,
                       Widgets::ListState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_list_bucket(style, items, state, {});
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

inline auto publish_list_state_scenes(PathSpace& space,
                               AppRootPathView appRoot,
                               std::string_view name,
                               Widgets::ListStyle const& style,
                               std::vector<Widgets::ListItem> const& items,
                               Widgets::ListState const& default_state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};

    auto normalize_index = [&](std::int32_t index) -> std::int32_t {
        if (index < 0) {
            return -1;
        }
        if (index >= static_cast<std::int32_t>(items.size())) {
            return items.empty() ? -1 : static_cast<std::int32_t>(items.size()) - 1;
        }
        if (!items[static_cast<std::size_t>(index)].enabled) {
            return first_enabled_index(items);
        }
        return index;
    };

    Widgets::ListState idle = default_state;
    idle.selected_index = normalize_index(idle.selected_index);
    Widgets::ListState hover = idle;
    if (hover.selected_index < 0) {
        hover.hovered_index = normalize_index(0);
    } else {
        hover.hovered_index = hover.selected_index;
    }
    Widgets::ListState pressed = idle;
    if (pressed.selected_index < 0) {
        pressed.selected_index = normalize_index(0);
    }
    Widgets::ListState disabled = idle;
    disabled.enabled = false;
    disabled.selected_index = -1;
    disabled.hovered_index = -1;

    struct Variant {
        std::string_view state;
        Widgets::ListState list_state;
        ScenePath* target = nullptr;
    };

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
                                                   "Widget list state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_list_bucket(style, items, variant.list_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

