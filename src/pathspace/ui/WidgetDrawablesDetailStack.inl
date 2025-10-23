struct StackWidgetSize {
    float width = 0.0f;
    float height = 0.0f;
};

struct StackRuntimeChild {
    Widgets::StackChildSpec spec;
    StackWidgetSize preferred_size{};
};

struct StackLayoutComputation {
    Widgets::StackLayoutState state;
    DirtyRectHint dirty;
    std::vector<float> main_sizes;
};

inline auto clamp_dimension(float value,
                            float min_value,
                            bool has_min,
                            float max_value,
                            bool has_max) -> float {
    if (has_min) {
        value = std::max(value, min_value);
    }
    if (has_max) {
        value = std::min(value, max_value);
    }
    return value;
}

inline auto measure_button(PathSpace& space,
                           std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto stylePath = root + "/meta/style";
    auto style = space.read<Widgets::ButtonStyle, std::string>(stylePath);
    if (!style) {
        return std::unexpected(style.error());
    }
    return StackWidgetSize{std::max(style->width, 0.0f), std::max(style->height, 0.0f)};
}

inline auto measure_toggle(PathSpace& space,
                           std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto stylePath = root + "/meta/style";
    auto style = space.read<Widgets::ToggleStyle, std::string>(stylePath);
    if (!style) {
        return std::unexpected(style.error());
    }
    return StackWidgetSize{std::max(style->width, 0.0f), std::max(style->height, 0.0f)};
}

inline auto measure_slider(PathSpace& space,
                           std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto stylePath = root + "/meta/style";
    auto style = space.read<Widgets::SliderStyle, std::string>(stylePath);
    if (!style) {
        return std::unexpected(style.error());
    }
    return StackWidgetSize{std::max(style->width, 0.0f), std::max(style->height, 0.0f)};
}

inline auto measure_list(PathSpace& space,
                         std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto stylePath = root + "/meta/style";
    auto itemsPath = root + "/meta/items";
    auto style = space.read<Widgets::ListStyle, std::string>(stylePath);
    if (!style) {
        return std::unexpected(style.error());
    }
    auto items = space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
    if (!items) {
        return std::unexpected(items.error());
    }
    auto item_count = std::max<std::size_t>(items->size(), 1u);
    float height = style->item_height * static_cast<float>(item_count) + style->border_thickness * 2.0f;
    return StackWidgetSize{std::max(style->width, 0.0f), std::max(height, 0.0f)};
}

inline auto measure_widget(PathSpace& space,
                           std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto kindPath = root + "/meta/kind";
    auto kind = space.read<std::string, std::string>(kindPath);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == "button") {
        return measure_button(space, root);
    }
    if (*kind == "toggle") {
        return measure_toggle(space, root);
    }
    if (*kind == "slider") {
        return measure_slider(space, root);
    }
    if (*kind == "list") {
        return measure_list(space, root);
    }
    return std::unexpected(make_error("Unsupported widget kind for stack layout: " + *kind,
                                      SP::Error::Code::InvalidType));
}

inline auto prepare_runtime_children(PathSpace& space,
                                     std::vector<Widgets::StackChildSpec> const& specs)
    -> SP::Expected<std::vector<StackRuntimeChild>> {
    std::vector<StackRuntimeChild> runtime;
    runtime.reserve(specs.size());
    for (auto const& spec : specs) {
        auto size = measure_widget(space, spec.widget_path);
        if (!size) {
            return std::unexpected(size.error());
        }
        StackRuntimeChild child{
            .spec = spec,
            .preferred_size = *size,
        };
        runtime.push_back(std::move(child));
    }
    return runtime;
}

inline auto compute_stack_layout(Widgets::StackLayoutStyle const& style,
                                 std::vector<StackRuntimeChild> const& runtime)
    -> StackLayoutComputation {
    StackLayoutComputation result{};
    auto& state = result.state;
    state.children.reserve(runtime.size());

    auto axis = style.axis;
    float spacing = std::max(style.spacing, 0.0f);
    float spacing_total = runtime.empty() ? 0.0f : spacing * static_cast<float>(runtime.size() - 1);

    float padding_main = style.padding_main_start + style.padding_main_end;
    float padding_cross = style.padding_cross_start + style.padding_cross_end;

    float total_fixed = 0.0f;
    float total_weight_base = 0.0f;
    float total_weight = 0.0f;
    float max_cross_extent = 0.0f;

    result.main_sizes.resize(runtime.size(), 0.0f);

    for (std::size_t index = 0; index < runtime.size(); ++index) {
        auto const& child = runtime[index];
        auto const& constraints = child.spec.constraints;
        float preferred_main = axis == Widgets::StackAxis::Horizontal ? child.preferred_size.width : child.preferred_size.height;
        float preferred_cross = axis == Widgets::StackAxis::Horizontal ? child.preferred_size.height : child.preferred_size.width;

        float base_main = clamp_dimension(preferred_main,
                                          constraints.min_main,
                                          constraints.has_min_main,
                                          constraints.max_main,
                                          constraints.has_max_main);
        float base_cross = clamp_dimension(preferred_cross,
                                           constraints.min_cross,
                                           constraints.has_min_cross,
                                           constraints.max_cross,
                                           constraints.has_max_cross);

        float main_with_margin = base_main + constraints.margin_main_start + constraints.margin_main_end;
        float cross_with_margin = base_cross + constraints.margin_cross_start + constraints.margin_cross_end;

        result.main_sizes[index] = base_main;

        if (constraints.weight <= 0.0f) {
            total_fixed += main_with_margin;
        } else {
            total_weight += constraints.weight;
            total_weight_base += main_with_margin;
        }

        max_cross_extent = std::max(max_cross_extent, cross_with_margin);
    }

    float base_main_sum = total_fixed + total_weight_base;

    float container_main = axis == Widgets::StackAxis::Horizontal ? style.width : style.height;
    if (container_main <= 0.0f) {
        container_main = padding_main + base_main_sum + spacing_total;
    }
    container_main = std::max(container_main, padding_main + base_main_sum + spacing_total);

    float container_cross = axis == Widgets::StackAxis::Horizontal ? style.height : style.width;
    if (container_cross <= 0.0f) {
        container_cross = padding_cross + max_cross_extent;
    }
    container_cross = std::max(container_cross, padding_cross + max_cross_extent);

    state.width = axis == Widgets::StackAxis::Horizontal ? container_main : container_cross;
    state.height = axis == Widgets::StackAxis::Horizontal ? container_cross : container_main;

    float available_main = container_main - padding_main - spacing_total - total_fixed - total_weight_base;
    available_main = std::max(available_main, 0.0f);

    std::vector<bool> saturated(runtime.size(), false);

    float remaining = available_main;
    while (remaining > 1e-3f && total_weight > 0.0f) {
        float active_weight = 0.0f;
        for (std::size_t index = 0; index < runtime.size(); ++index) {
            auto const& constraints = runtime[index].spec.constraints;
            if (constraints.weight <= 0.0f || saturated[index]) {
                continue;
            }
            active_weight += constraints.weight;
        }
        if (active_weight <= 0.0f) {
            break;
        }

        bool any_saturated = false;
        float consumed = 0.0f;
        for (std::size_t index = 0; index < runtime.size(); ++index) {
            auto const& constraints = runtime[index].spec.constraints;
            if (constraints.weight <= 0.0f || saturated[index]) {
                continue;
            }
            float share = remaining * (constraints.weight / active_weight);
            float capacity = std::numeric_limits<float>::infinity();
            if (constraints.has_max_main) {
                capacity = std::max(0.0f, constraints.max_main - result.main_sizes[index]);
            }
            float delta = share;
            if (delta > capacity) {
                delta = capacity;
                saturated[index] = true;
                any_saturated = true;
            }
            result.main_sizes[index] += delta;
            consumed += delta;
        }

        if (!any_saturated || consumed <= 1e-5f) {
            break;
        }
        remaining = std::max(0.0f, remaining - consumed);
    }

    float total_children_main = padding_main + spacing_total;
    for (std::size_t index = 0; index < runtime.size(); ++index) {
        auto const& constraints = runtime[index].spec.constraints;
        float main = clamp_dimension(result.main_sizes[index],
                                     constraints.min_main,
                                     constraints.has_min_main,
                                     constraints.max_main,
                                     constraints.has_max_main);
        result.main_sizes[index] = main;
        total_children_main += main + constraints.margin_main_start + constraints.margin_main_end;
    }

    float offset_main = style.padding_main_start;
    if (container_main > total_children_main) {
        float slack = container_main - total_children_main;
        switch (style.align_main) {
        case Widgets::StackAlignMain::Start:
            break;
        case Widgets::StackAlignMain::Center:
            offset_main += slack * 0.5f;
            break;
        case Widgets::StackAlignMain::End:
            offset_main += slack;
            break;
        }
    }

    float cross_available = container_cross - padding_cross;
    if (cross_available < 0.0f) {
        cross_available = 0.0f;
    }

    for (std::size_t index = 0; index < runtime.size(); ++index) {
        auto const& child = runtime[index];
        auto const& constraints = child.spec.constraints;

        Widgets::StackLayoutComputedChild computed{};
        computed.id = child.spec.id;

        float main = result.main_sizes[index];
        float cross_pref = axis == Widgets::StackAxis::Horizontal ? child.preferred_size.height : child.preferred_size.width;
        float cross_size = clamp_dimension(cross_pref,
                                           constraints.min_cross,
                                           constraints.has_min_cross,
                                           constraints.max_cross,
                                           constraints.has_max_cross);

        float cross_space = cross_available - constraints.margin_cross_start - constraints.margin_cross_end;
        cross_space = std::max(cross_space, 0.0f);
        if (style.align_cross == Widgets::StackAlignCross::Stretch) {
            cross_size = clamp_dimension(cross_space,
                                         constraints.min_cross,
                                         constraints.has_min_cross,
                                         constraints.max_cross,
                                         constraints.has_max_cross);
        }

        float pos_main = offset_main + constraints.margin_main_start;
        offset_main += main + constraints.margin_main_start + constraints.margin_main_end + spacing;

        float cross_offset = style.padding_cross_start + constraints.margin_cross_start;
        switch (style.align_cross) {
        case Widgets::StackAlignCross::Start:
            break;
        case Widgets::StackAlignCross::Center: {
            float slack = cross_space - cross_size;
            if (slack > 0.0f) {
                cross_offset += slack * 0.5f;
            }
            break;
        }
        case Widgets::StackAlignCross::End: {
            float slack = cross_space - cross_size;
            if (slack > 0.0f) {
                cross_offset += slack;
            }
            break;
        }
        case Widgets::StackAlignCross::Stretch:
            break;
        }

        if (axis == Widgets::StackAxis::Horizontal) {
            computed.x = pos_main;
            computed.y = cross_offset;
            computed.width = main;
            computed.height = cross_size;
        } else {
            computed.x = cross_offset;
            computed.y = pos_main;
            computed.width = cross_size;
            computed.height = main;
        }

        state.children.push_back(std::move(computed));
    }

    result.dirty = make_default_dirty_rect(state.width, state.height);
    return result;
}

template <typename Cmd>
inline auto read_command(std::vector<std::uint8_t> const& payload, std::size_t offset) -> Cmd {
    Cmd cmd{};
    std::memcpy(&cmd, payload.data() + offset, sizeof(Cmd));
    return cmd;
}

template <typename Cmd>
inline auto write_command(std::vector<std::uint8_t>& payload, std::size_t offset, Cmd const& cmd) -> void {
    std::memcpy(payload.data() + offset, &cmd, sizeof(Cmd));
}

inline auto translate_bucket(SceneData::DrawableBucketSnapshot& bucket,
                             float dx,
                             float dy) -> void {
    for (auto& sphere : bucket.bounds_spheres) {
        sphere.center[0] += dx;
        sphere.center[1] += dy;
    }
    for (auto& box : bucket.bounds_boxes) {
        box.min[0] += dx;
        box.max[0] += dx;
        box.min[1] += dy;
        box.max[1] += dy;
    }

    std::size_t offset = 0;
    for (auto kind_value : bucket.command_kinds) {
        auto kind = static_cast<SceneData::DrawCommandKind>(kind_value);
        switch (kind) {
        case SceneData::DrawCommandKind::Rect: {
            auto cmd = read_command<SceneData::RectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case SceneData::DrawCommandKind::RoundedRect: {
            auto cmd = read_command<SceneData::RoundedRectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case SceneData::DrawCommandKind::TextGlyphs: {
            auto cmd = read_command<SceneData::TextGlyphsCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        default:
            break;
        }
        offset += SceneData::payload_size_bytes(kind);
    }
}

inline auto append_bucket(SceneData::DrawableBucketSnapshot& dest,
                          SceneData::DrawableBucketSnapshot const& src) -> void {
    if (src.drawable_ids.empty()) {
        return;
    }

    auto drawable_base = static_cast<std::uint32_t>(dest.drawable_ids.size());
    auto command_base = static_cast<std::uint32_t>(dest.command_kinds.size());
    auto clip_base = static_cast<std::int32_t>(dest.clip_nodes.size());

    dest.drawable_ids.insert(dest.drawable_ids.end(), src.drawable_ids.begin(), src.drawable_ids.end());
    dest.world_transforms.insert(dest.world_transforms.end(), src.world_transforms.begin(), src.world_transforms.end());
    dest.bounds_spheres.insert(dest.bounds_spheres.end(), src.bounds_spheres.begin(), src.bounds_spheres.end());
    dest.bounds_boxes.insert(dest.bounds_boxes.end(), src.bounds_boxes.begin(), src.bounds_boxes.end());
    dest.bounds_box_valid.insert(dest.bounds_box_valid.end(), src.bounds_box_valid.begin(), src.bounds_box_valid.end());
    dest.layers.insert(dest.layers.end(), src.layers.begin(), src.layers.end());
    dest.z_values.insert(dest.z_values.end(), src.z_values.begin(), src.z_values.end());
    dest.material_ids.insert(dest.material_ids.end(), src.material_ids.begin(), src.material_ids.end());
    dest.pipeline_flags.insert(dest.pipeline_flags.end(), src.pipeline_flags.begin(), src.pipeline_flags.end());
    dest.visibility.insert(dest.visibility.end(), src.visibility.begin(), src.visibility.end());

    for (auto offset : src.command_offsets) {
        dest.command_offsets.push_back(offset + command_base);
    }
    dest.command_counts.insert(dest.command_counts.end(), src.command_counts.begin(), src.command_counts.end());

    dest.command_kinds.insert(dest.command_kinds.end(), src.command_kinds.begin(), src.command_kinds.end());
    dest.command_payload.insert(dest.command_payload.end(), src.command_payload.begin(), src.command_payload.end());

    for (auto index : src.opaque_indices) {
        dest.opaque_indices.push_back(index + drawable_base);
    }
    for (auto index : src.alpha_indices) {
        dest.alpha_indices.push_back(index + drawable_base);
    }

    for (auto const& entry : src.layer_indices) {
        SceneData::LayerIndices adjusted{entry.layer, {}};
        adjusted.indices.reserve(entry.indices.size());
        for (auto idx : entry.indices) {
            adjusted.indices.push_back(idx + drawable_base);
        }
        dest.layer_indices.push_back(std::move(adjusted));
    }

    dest.stroke_points.insert(dest.stroke_points.end(), src.stroke_points.begin(), src.stroke_points.end());

    if (!src.clip_nodes.empty()) {
        for (auto node : src.clip_nodes) {
            node.next = (node.next < 0) ? -1 : node.next + clip_base;
            dest.clip_nodes.push_back(node);
        }
        for (auto head : src.clip_head_indices) {
            dest.clip_head_indices.push_back(head < 0 ? -1 : head + clip_base);
        }
    } else {
        dest.clip_head_indices.insert(dest.clip_head_indices.end(), src.clip_head_indices.begin(), src.clip_head_indices.end());
    }

    dest.authoring_map.insert(dest.authoring_map.end(), src.authoring_map.begin(), src.authoring_map.end());
    dest.drawable_fingerprints.insert(dest.drawable_fingerprints.end(), src.drawable_fingerprints.begin(), src.drawable_fingerprints.end());
}

inline auto load_child_bucket(PathSpace& space,
                              Widgets::StackChildSpec const& child) -> SP::Expected<SceneData::DrawableBucketSnapshot> {
    ScenePath scenePath{child.scene_path};
    auto revision = Scene::ReadCurrentRevision(space, scenePath);
    if (!revision) {
        return std::unexpected(revision.error());
    }
    auto revisionStr = format_revision(revision->revision);
    auto revisionBase = make_revision_base(scenePath, revisionStr);
    return SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revisionBase);
}

inline auto build_stack_bucket(PathSpace& space,
                               Widgets::StackLayoutState const& state,
                               std::vector<StackRuntimeChild> const& runtime)
    -> SP::Expected<SceneData::DrawableBucketSnapshot> {
    SceneData::DrawableBucketSnapshot bucket{};
    for (std::size_t index = 0; index < runtime.size(); ++index) {
        auto const& child = runtime[index];
        if (index >= state.children.size()) {
            break;
        }
        auto bucketResult = load_child_bucket(space, child.spec);
        if (!bucketResult) {
            return std::unexpected(bucketResult.error());
        }
        auto translated = *bucketResult;
        auto const& computed = state.children[index];
        translate_bucket(translated, computed.x, computed.y);
        append_bucket(bucket, translated);
    }
    return bucket;
}

inline auto compute_stack(PathSpace& space,
                          Widgets::StackLayoutParams const& params)
    -> SP::Expected<std::pair<StackLayoutComputation, std::vector<StackRuntimeChild>>> {
    auto runtime = prepare_runtime_children(space, params.children);
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    auto layout = compute_stack_layout(params.style, *runtime);
    return std::make_pair(std::move(layout), std::move(*runtime));
}
