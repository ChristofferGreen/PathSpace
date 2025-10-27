struct TreeChildGraph {
    std::unordered_map<std::string, std::size_t> index;
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::size_t> roots;
};

inline auto build_tree_children(std::vector<Widgets::TreeNode> const& nodes) -> TreeChildGraph {
    TreeChildGraph graph{};
    graph.index.reserve(nodes.size());
    graph.children.resize(nodes.size());
    graph.roots.reserve(nodes.size());

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        graph.index.emplace(nodes[i].id, i);
    }

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto const& node = nodes[i];
        if (node.parent_id.empty()) {
            graph.roots.push_back(i);
            continue;
        }
        auto it = graph.index.find(node.parent_id);
        if (it == graph.index.end()) {
            graph.roots.push_back(i);
            continue;
        }
        graph.children[it->second].push_back(i);
    }

    return graph;
}

struct TreeRowSnapshot {
    std::string id;
    std::string label;
    int depth = 0;
    bool enabled = true;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
};

inline auto flatten_tree_rows(std::vector<Widgets::TreeNode> const& nodes,
                              Widgets::TreeState const& state) -> std::vector<TreeRowSnapshot> {
    auto graph = build_tree_children(nodes);

    auto to_set = [](std::vector<std::string> const& values) {
        std::unordered_set<std::string> out;
        out.reserve(values.size());
        out.insert(values.begin(), values.end());
        return out;
    };

    auto expanded = to_set(state.expanded_ids);
    auto loading = to_set(state.loading_ids);

    std::vector<TreeRowSnapshot> rows;
    rows.reserve(nodes.size());

    auto visit = [&](auto&& self, std::size_t index, int depth) -> void {
        auto const& node = nodes[index];
        bool has_children = index < graph.children.size() && !graph.children[index].empty();
        bool expandable = has_children || node.expandable;
        bool expanded_flag = expandable && (expanded.find(node.id) != expanded.end());
        bool loading_flag = loading.find(node.id) != loading.end();

        rows.push_back(TreeRowSnapshot{
            .id = node.id,
            .label = node.label,
            .depth = depth,
            .enabled = node.enabled && state.enabled,
            .expandable = expandable,
            .expanded = expanded_flag,
            .loading = loading_flag,
        });

        if (has_children && expanded_flag) {
            for (auto child_index : graph.children[index]) {
                self(self, child_index, depth + 1);
            }
        }
    };

    if (!graph.roots.empty()) {
        for (auto root_index : graph.roots) {
            if (root_index < nodes.size()) {
                visit(visit, root_index, 0);
            }
        }
    } else if (!nodes.empty()) {
        visit(visit, std::size_t{0}, 0);
    }

    if (rows.empty()) {
        rows.push_back(TreeRowSnapshot{
            .id = {},
            .label = {},
            .depth = 0,
            .enabled = state.enabled,
            .expandable = false,
            .expanded = false,
            .loading = false,
        });
    }

    return rows;
}

inline auto tree_states_equal(Widgets::TreeState const& lhs,
                              Widgets::TreeState const& rhs) -> bool {
    auto equal_float = [](float a, float b) {
        return std::fabs(a - b) <= 1e-6f;
    };
    return lhs.enabled == rhs.enabled
        && lhs.focused == rhs.focused
        && lhs.hovered_id == rhs.hovered_id
        && lhs.selected_id == rhs.selected_id
        && lhs.expanded_ids == rhs.expanded_ids
        && lhs.loading_ids == rhs.loading_ids
        && equal_float(lhs.scroll_offset, rhs.scroll_offset);
}

inline auto build_tree_bucket(Widgets::TreeStyle const& style,
                              std::vector<Widgets::TreeNode> const& nodes,
                              Widgets::TreeState const& state,
                              std::string const& authoring_root = {},
                              bool pulsing_highlight = false) -> SceneData::DrawableBucketSnapshot {
    auto rows = flatten_tree_rows(nodes, state);

    float const row_height = std::max(style.row_height, 1.0f);
    float const width = std::max(style.width, 96.0f);
    std::size_t const visible_rows = std::max<std::size_t>(rows.size(), 1u);
    float const height = style.border_thickness * 2.0f
        + row_height * static_cast<float>(visible_rows);

    SceneData::DrawableBucketSnapshot bucket{};
    std::size_t const max_drawables = 1 + visible_rows * 2;
    bucket.drawable_ids.reserve(max_drawables);
    bucket.world_transforms.reserve(max_drawables);
    bucket.bounds_boxes.reserve(max_drawables);
    bucket.bounds_box_valid.reserve(max_drawables);
    bucket.bounds_spheres.reserve(max_drawables);
    bucket.layers.reserve(max_drawables);
    bucket.z_values.reserve(max_drawables);
    bucket.material_ids.reserve(max_drawables);
    bucket.pipeline_flags.reserve(max_drawables);
    bucket.visibility.reserve(max_drawables);
    bucket.command_offsets.reserve(max_drawables);
    bucket.command_counts.reserve(max_drawables);
    bucket.opaque_indices.reserve(max_drawables);
    bucket.clip_head_indices.reserve(max_drawables);
    bucket.authoring_map.reserve(max_drawables);
    bucket.drawable_fingerprints.reserve(max_drawables);

    auto push_drawable = [&](std::uint64_t id,
                             SceneData::BoundingBox const& box,
                             int layer,
                             float z) {
        SceneData::BoundingSphere sphere{};
        sphere.center = {(box.min[0] + box.max[0]) * 0.5f,
                         (box.min[1] + box.max[1]) * 0.5f,
                         0.0f};
        float half_w = (box.max[0] - box.min[0]) * 0.5f;
        float half_h = (box.max[1] - box.min[1]) * 0.5f;
        sphere.radius = std::sqrt(half_w * half_w + half_h * half_h);

        bucket.drawable_ids.push_back(id);
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

    push_drawable(0x41A00001ull, background_box, 0, 0.0f);
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

    SceneData::RoundedRectCommand background{};
    background.min_x = background_box.min[0];
    background.min_y = background_box.min[1];
    background.max_x = background_box.max[0];
    background.max_y = background_box.max[1];
    background.radius_top_left = style.corner_radius;
    background.radius_top_right = style.corner_radius;
    background.radius_bottom_right = style.corner_radius;
    background.radius_bottom_left = style.corner_radius;
    background.color = style.background_color;

    auto payload_offset = bucket.command_payload.size();
    bucket.command_payload.resize(payload_offset + sizeof(SceneData::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + payload_offset,
                &background,
                sizeof(SceneData::RoundedRectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.back(),
        make_widget_authoring_id(authoring_root, "tree/background"),
        0,
        0});
    bucket.drawable_fingerprints.push_back(0x41A00001ull);

    auto make_row_color = [&](TreeRowSnapshot const& row) -> std::array<float, 4> {
        if (!row.enabled) {
            return style.row_disabled_color;
        }
        if (!state.enabled) {
            return style.row_disabled_color;
        }
        if (!row.id.empty() && row.id == state.selected_id) {
            return style.row_selected_color;
        }
        if (!row.id.empty() && row.id == state.hovered_id) {
            return style.row_hover_color;
        }
        return style.row_color;
    };

    float const content_left = style.border_thickness;
    float const content_right = width - style.border_thickness;
    float const toggle_size = std::clamp(style.toggle_icon_size, 4.0f, row_height - 4.0f);

    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto const& row = rows[i];
        float const row_top = style.border_thickness
            + row_height * static_cast<float>(i)
            - state.scroll_offset;
        float const row_bottom = row_top + row_height;

        SceneData::BoundingBox row_box{};
        row_box.min = {content_left, row_top, 0.0f};
        row_box.max = {content_right, row_bottom, 0.0f};

        std::uint64_t row_id = 0x41A10000ull + static_cast<std::uint64_t>(i);
        push_drawable(row_id, row_box, 1, 0.05f + static_cast<float>(i) * 0.002f);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

        SceneData::RoundedRectCommand row_rect{};
        row_rect.min_x = row_box.min[0];
        row_rect.min_y = row_box.min[1];
        row_rect.max_x = row_box.max[0];
        row_rect.max_y = row_box.max[1];
        row_rect.radius_top_left = 4.0f;
        row_rect.radius_top_right = 4.0f;
        row_rect.radius_bottom_right = 4.0f;
        row_rect.radius_bottom_left = 4.0f;
        row_rect.color = make_row_color(row);

        auto row_payload = bucket.command_payload.size();
        bucket.command_payload.resize(row_payload + sizeof(SceneData::RoundedRectCommand));
        std::memcpy(bucket.command_payload.data() + row_payload,
                    &row_rect,
                    sizeof(SceneData::RoundedRectCommand));

        bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
            row_id,
            make_widget_authoring_id(authoring_root,
                                     std::string("tree/row/") + (row.id.empty() ? "placeholder" : row.id)),
            0,
            0});
        bucket.drawable_fingerprints.push_back(row_id);

        if (row.expandable) {
            float const toggle_origin = content_left + style.indent_per_level * static_cast<float>(row.depth);
            SceneData::BoundingBox toggle_box{};
            toggle_box.min = {toggle_origin, row_top + (row_height - toggle_size) * 0.5f, 0.0f};
            toggle_box.max = {toggle_origin + toggle_size,
                              toggle_box.min[1] + toggle_size,
                              0.0f};

            std::uint64_t toggle_id = 0x41A20000ull + static_cast<std::uint64_t>(i);
            push_drawable(toggle_id, toggle_box, 2, 0.10f + static_cast<float>(i) * 0.002f);
            bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
            bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

            SceneData::RoundedRectCommand toggle_rect{};
            toggle_rect.min_x = toggle_box.min[0];
            toggle_rect.min_y = toggle_box.min[1];
            toggle_rect.max_x = toggle_box.max[0];
            toggle_rect.max_y = toggle_box.max[1];
            toggle_rect.radius_top_left = 2.0f;
            toggle_rect.radius_top_right = 2.0f;
            toggle_rect.radius_bottom_right = 2.0f;
            toggle_rect.radius_bottom_left = 2.0f;
            toggle_rect.color = row.expanded ? style.toggle_color : desaturate_color(style.toggle_color, 0.4f);

            auto toggle_payload = bucket.command_payload.size();
            bucket.command_payload.resize(toggle_payload + sizeof(SceneData::RoundedRectCommand));
            std::memcpy(bucket.command_payload.data() + toggle_payload,
                        &toggle_rect,
                        sizeof(SceneData::RoundedRectCommand));

            bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
                toggle_id,
                make_widget_authoring_id(authoring_root,
                                         std::string("tree/toggle/") + (row.id.empty() ? "placeholder" : row.id)),
                0,
                0});
            bucket.drawable_fingerprints.push_back(toggle_id);
        }
    }

    if (state.focused) {
        auto highlight_color = lighten_color(style.row_selected_color, 0.15f);
        append_focus_highlight(bucket, width, height, authoring_root, pulsing_highlight, highlight_color);
    }

    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    return bucket;
}

inline auto first_enabled_tree(std::vector<TreeRowSnapshot> const& rows) -> std::optional<std::string> {
    for (auto const& row : rows) {
        if (row.enabled && !row.id.empty()) {
            return row.id;
        }
    }
    return std::nullopt;
}

inline auto publish_tree_state_scenes(PathSpace& space,
                                      AppRootPathView appRoot,
                                      std::string_view name,
                                      Widgets::TreeStyle const& style,
                                      std::vector<Widgets::TreeNode> const& nodes,
                                      Widgets::TreeState const& default_state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};

    auto rows = flatten_tree_rows(nodes, default_state);
    auto first_enabled = first_enabled_tree(rows);

    Widgets::TreeState idle = default_state;
    Widgets::TreeState hover = idle;
    if (hover.enabled && hover.hovered_id.empty() && first_enabled) {
        hover.hovered_id = *first_enabled;
    }
    Widgets::TreeState pressed = hover;
    if (pressed.enabled && !pressed.hovered_id.empty()) {
        pressed.selected_id = pressed.hovered_id;
    }
    Widgets::TreeState disabled = idle;
    disabled.enabled = false;
    disabled.hovered_id.clear();
    disabled.selected_id.clear();
    disabled.loading_ids.clear();

    struct Variant {
        std::string_view name;
        Widgets::TreeState* state;
        ScenePath* target;
    };

    Variant variants[] = {
        {"idle", &idle, &scenes.idle},
        {"hover", &hover, &scenes.hover},
        {"pressed", &pressed, &scenes.pressed},
        {"disabled", &disabled, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.name,
                                                   "Widget tree state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_tree_bucket(style, nodes, *variant.state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}
