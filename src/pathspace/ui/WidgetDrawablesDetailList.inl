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
                       std::string_view authoring_root,
                       bool pulsing_highlight = false) -> SceneData::DrawableBucketSnapshot {
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

    float highlight_width = config.width;
    float highlight_height = config.border_thickness * 2.0f
        + config.item_height * static_cast<float>(std::max<std::size_t>(config.item_count, 1u));

    auto bucket = make_list_bucket(config, authoring_root);
    if (state.focused) {
        auto highlight_color = lighten_color(style.item_selected_color, 0.18f);
        append_focus_highlight(bucket, highlight_width, highlight_height, authoring_root, pulsing_highlight, highlight_color);
    }
    return bucket;
}

inline auto build_list_bucket(Widgets::ListStyle const& style,
                       std::vector<Widgets::ListItem> const& items,
                       Widgets::ListState const& state,
                       bool pulsing_highlight = false) -> SceneData::DrawableBucketSnapshot {
    return build_list_bucket(style, items, state, {}, pulsing_highlight);
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
