#include <cstdio>
#include <cstdlib>

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

inline auto measure_tree(PathSpace& space,
                         std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto stylePath = root + "/meta/style";
    auto nodesPath = root + "/meta/nodes";
    auto style = space.read<Widgets::TreeStyle, std::string>(stylePath);
    if (!style) {
        return std::unexpected(style.error());
    }
    auto nodes = space.read<std::vector<Widgets::TreeNode>, std::string>(nodesPath);
    if (!nodes) {
        return std::unexpected(nodes.error());
    }

    std::size_t node_count = std::max<std::size_t>(nodes->size(), 1u);
    float row_height = std::max(style->row_height, 20.0f);
    float height = style->border_thickness * 2.0f + row_height * static_cast<float>(node_count);
    return StackWidgetSize{std::max(style->width, 0.0f), std::max(height, row_height)};
}

inline auto measure_stack(PathSpace& space,
                          std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto computedPath = root + "/layout/computed";
    auto layout = space.read<Widgets::StackLayoutState, std::string>(computedPath);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    return StackWidgetSize{std::max(layout->width, 0.0f), std::max(layout->height, 0.0f)};
}

inline auto measure_paint_surface(PathSpace& space,
                                  std::string const& root) -> SP::Expected<StackWidgetSize> {
    auto metrics = SP::UI::Declarative::PaintRuntime::ReadBufferMetrics(space, root);
    if (!metrics) {
        return std::unexpected(metrics.error());
    }
    auto width = static_cast<float>(metrics->width);
    auto height = static_cast<float>(metrics->height);
    return StackWidgetSize{std::max(width, 0.0f), std::max(height, 0.0f)};
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
    if (*kind == "label") {
        // Labels do not publish layout metadata today; estimate a reasonable
        // footprint so stack layout can proceed.
        return StackWidgetSize{120.0f, 24.0f};
    }
    if (*kind == "tree") {
        return measure_tree(space, root);
    }
    if (*kind == "stack") {
        return measure_stack(space, root);
    }
    if (*kind == "paint_surface") {
        return measure_paint_surface(space, root);
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
    float container_cross = axis == Widgets::StackAxis::Horizontal ? style.height : style.width;

    if (container_main <= 0.0f || container_cross <= 0.0f) {
        int win_w = 0;
        int win_h = 0;
        SP::UI::GetLocalWindowContentSize(&win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            if (container_main <= 0.0f) {
                container_main = axis == Widgets::StackAxis::Horizontal ? static_cast<float>(win_w)
                                                                        : static_cast<float>(win_h);
            }
            if (container_cross <= 0.0f) {
                container_cross = axis == Widgets::StackAxis::Horizontal ? static_cast<float>(win_h)
                                                                         : static_cast<float>(win_w);
            }
        }
    }

    if (container_main <= 0.0f) {
        container_main = padding_main + base_main_sum + spacing_total;
    }
    container_main = std::max(container_main, padding_main + base_main_sum + spacing_total);

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

    if (std::getenv("PATHSPACE_DEBUG_LAYOUT")) {
        std::fprintf(stderr,
                     "[stack-layout] axis=%s size=(%.1f x %.1f) children=%zu\n",
                     axis == Widgets::StackAxis::Horizontal ? "horizontal" : "vertical",
                     state.width,
                     state.height,
                     state.children.size());
        for (std::size_t index = 0; index < state.children.size(); ++index) {
            auto const& child = state.children[index];
            std::fprintf(stderr,
                         "  child[%zu] id=%s pos=(%.1f, %.1f) size=(%.1f x %.1f)\n",
                         index,
                         child.id.c_str(),
                         child.x,
                         child.y,
                         child.width,
                         child.height);
        }
    }

    return result;
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
