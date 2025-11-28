struct TextLineSpan {
    std::size_t start = 0;
    std::size_t end = 0;
};

inline auto clamp_range(std::uint32_t& first,
                        std::uint32_t& second,
                        std::size_t length) -> void {
    auto clamp_index = [length](std::uint32_t value) -> std::uint32_t {
        if (length == 0) {
            return 0;
        }
        if (value > length) {
            return static_cast<std::uint32_t>(length);
        }
        return value;
    };

    first = clamp_index(first);
    second = clamp_index(second);
    if (first > second) {
        std::swap(first, second);
    }
}

inline auto split_lines(std::string const& text) -> std::vector<TextLineSpan> {
    std::vector<TextLineSpan> spans;
    spans.reserve(std::max<std::size_t>(1u, text.size() / 32u + 1u));
    std::size_t line_start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\n') {
            spans.push_back(TextLineSpan{line_start, index});
            line_start = index + 1;
        }
    }
    spans.push_back(TextLineSpan{line_start, text.size()});
    return spans;
}

template <typename Style>
inline auto sanitize_text_input_style(Style style) -> Style {
    style.typography.font_size = std::max(style.typography.font_size, 1.0f);
    style.typography.line_height = std::max(style.typography.line_height, style.typography.font_size);
    style.typography.letter_spacing = std::max(style.typography.letter_spacing, 0.0f);
    style.width = std::max(style.width, 96.0f);
    style.height = std::max(style.height,
                            style.typography.line_height + style.padding_y * 2.0f + 4.0f);
    auto radius_limit = std::min(style.width, style.height) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.border_thickness = std::clamp(style.border_thickness, 0.0f, radius_limit);
    style.padding_x = std::max(style.padding_x, 0.0f);
    style.padding_y = std::max(style.padding_y, 0.0f);
    return style;
}

inline auto sanitize_text_field_style(Widgets::TextFieldStyle style) -> Widgets::TextFieldStyle {
    return sanitize_text_input_style(style);
}

inline auto sanitize_text_area_style(Widgets::TextAreaStyle style) -> Widgets::TextAreaStyle {
    Widgets::TextAreaStyle sanitized = style;
    sanitized = sanitize_text_input_style(sanitized);
    sanitized.min_height = std::max(sanitized.min_height,
                                   sanitized.typography.line_height * 2.0f + sanitized.padding_y * 2.0f);
    sanitized.height = std::max(sanitized.height, sanitized.min_height);
    sanitized.line_spacing = std::max(sanitized.line_spacing, 0.0f);
    return sanitized;
}

inline auto sanitize_text_field_state(Widgets::TextFieldState state,
                                      Widgets::TextFieldStyle const& style) -> Widgets::TextFieldState {
    auto const length = state.text.size();
    clamp_range(state.cursor, state.cursor, length);
    clamp_range(state.selection_start, state.selection_end, length);
    clamp_range(state.composition_start, state.composition_end, length);
    if (!state.composition_active) {
        state.composition_text.clear();
        state.composition_start = state.cursor;
        state.composition_end = state.cursor;
    }
    if (!state.enabled) {
        state.focused = false;
        state.hovered = false;
        state.submit_pending = false;
    }
    if (state.read_only) {
        state.submit_pending = false;
    }
    (void)style;
    return state;
}

inline auto sanitize_text_area_state(Widgets::TextAreaState state,
                                     Widgets::TextAreaStyle const& style) -> Widgets::TextAreaState {
    auto const length = state.text.size();
    clamp_range(state.cursor, state.cursor, length);
    clamp_range(state.selection_start, state.selection_end, length);
    clamp_range(state.composition_start, state.composition_end, length);
    if (!state.composition_active) {
        state.composition_text.clear();
        state.composition_start = state.cursor;
        state.composition_end = state.cursor;
    }
    if (!state.enabled) {
        state.focused = false;
        state.hovered = false;
    }

    state.scroll_x = std::max(state.scroll_x, 0.0f);
    state.scroll_y = std::max(state.scroll_y, 0.0f);
    (void)style;
    return state;
}

template <typename State>
inline auto text_input_states_equal(State const& lhs,
                                    State const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.read_only == rhs.read_only
        && lhs.hovered == rhs.hovered
        && lhs.focused == rhs.focused
        && lhs.text == rhs.text
        && lhs.placeholder == rhs.placeholder
        && lhs.cursor == rhs.cursor
        && lhs.selection_start == rhs.selection_start
        && lhs.selection_end == rhs.selection_end
        && lhs.composition_active == rhs.composition_active
        && lhs.composition_text == rhs.composition_text
        && lhs.composition_start == rhs.composition_start
        && lhs.composition_end == rhs.composition_end;
}

inline auto text_field_states_equal(Widgets::TextFieldState const& lhs,
                                    Widgets::TextFieldState const& rhs) -> bool {
    return text_input_states_equal(lhs, rhs)
        && lhs.submit_pending == rhs.submit_pending;
}

inline auto text_area_states_equal(Widgets::TextAreaState const& lhs,
                                   Widgets::TextAreaState const& rhs) -> bool {
    return text_input_states_equal(lhs, rhs)
        && std::fabs(lhs.scroll_x - rhs.scroll_x) <= 1e-6f
        && std::fabs(lhs.scroll_y - rhs.scroll_y) <= 1e-6f;
}

template <typename Style, typename State>
inline auto make_text_color(Style const& style,
                            State const& state) -> std::array<float, 4> {
    auto color = style.text_color;
    if (!state.enabled) {
        color = desaturate_color(color, 0.35f);
        color = scale_alpha(color, 0.6f);
    }
    return color;
}

template <typename Style, typename State>
inline auto make_placeholder_color(Style const& style,
                                   State const& state) -> std::array<float, 4> {
    auto color = style.placeholder_color;
    if (!state.enabled) {
        color = desaturate_color(color, 0.4f);
        color = scale_alpha(color, 0.5f);
    }
    return color;
}

template <typename Style, typename State>
inline auto make_background_color(Style const& style,
                                  State const& state) -> std::array<float, 4> {
    auto color = style.background_color;
    if (!state.enabled) {
        color = desaturate_color(color, 0.3f);
    } else if (state.focused) {
        color = lighten_color(color, 0.12f);
    } else if (state.hovered) {
        color = lighten_color(color, 0.07f);
    }
    return color;
}

template <typename Style, typename State>
inline auto make_border_color(Style const& style,
                              State const& state) -> std::array<float, 4> {
    auto color = style.border_color;
    if (!state.enabled) {
        color = desaturate_color(color, 0.35f);
        color = scale_alpha(color, 0.75f);
    } else if (state.focused) {
        color = lighten_color(color, 0.18f);
    } else if (state.hovered) {
        color = lighten_color(color, 0.10f);
    }
    return color;
}

inline auto add_rect(SceneData::DrawableBucketSnapshot& bucket,
                     float min_x,
                     float min_y,
                     float max_x,
                     float max_y,
                     std::array<float, 4> color,
                     std::uint64_t drawable_id,
                     int layer,
                     float z,
                     std::string_view authoring_id) -> void {
    if (max_x <= min_x || max_y <= min_y) {
        return;
    }

    SceneData::BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};

    SceneData::BoundingSphere sphere{};
    sphere.center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f, 0.0f};
    float dx = max_x - sphere.center[0];
    float dy = max_y - sphere.center[1];
    sphere.radius = std::sqrt(dx * dx + dy * dy);

    auto drawable_index = static_cast<std::uint32_t>(bucket.drawable_ids.size());
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
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_counts.push_back(1);
    bucket.opaque_indices.push_back(drawable_index);
    bucket.clip_head_indices.push_back(-1);
    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        drawable_id,
        std::string(authoring_id),
        drawable_index,
        0});
    bucket.drawable_fingerprints.push_back(drawable_id);

    SceneData::RectCommand rect{};
    rect.min_x = min_x;
    rect.min_y = min_y;
    rect.max_x = max_x;
    rect.max_y = max_y;
    rect.color = color;
    auto payload_offset = bucket.command_payload.size();
    bucket.command_payload.resize(payload_offset + sizeof(SceneData::RectCommand));
    std::memcpy(bucket.command_payload.data() + payload_offset,
                &rect,
                sizeof(SceneData::RectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));
}

template <typename State>
inline auto highlight_color(std::array<float, 4> base,
                            State const& state) -> std::array<float, 4> {
    if (!state.enabled) {
        return scale_alpha(base, 0.5f);
    }
    return base;
}

template <typename State>
inline auto caret_color(std::array<float, 4> base,
                        State const& state) -> std::array<float, 4> {
    if (!state.enabled) {
        return scale_alpha(base, 0.4f);
    }
    return base;
}

inline auto measure_text(std::string_view text,
                         Widgets::TypographyStyle const& typography) -> float {
    if (text.empty()) {
        return 0.0f;
    }
    return Text::MeasureTextWidth(text, typography);
}

template <typename Style, typename State>
inline auto build_text_input_bucket(Style const& style,
                                    State const& state,
                                    std::string_view authoring_root,
                                    bool pulsing_highlight,
                                    bool multiline,
                                    float scroll_x,
                                    float scroll_y,
                                    float line_spacing) -> SceneData::DrawableBucketSnapshot {
    auto background = make_background_color(style, state);
    auto border = make_border_color(style, state);

    SceneData::DrawableBucketSnapshot bucket{};
    float width = std::max(style.width, 1.0f);
    float height = std::max(style.height, 1.0f);

    if (style.border_thickness > 0.0f) {
        add_rect(bucket,
                 0.0f,
                 0.0f,
                 width,
                 height,
                 border,
                 0x17E70001ull,
                 0,
                 0.0f,
                 make_widget_authoring_id(authoring_root, "text_input/border"));
        add_rect(bucket,
                 style.border_thickness,
                 style.border_thickness,
                 width - style.border_thickness,
                 height - style.border_thickness,
                 background,
                 0x17E70002ull,
                 0,
                 0.0f,
                 make_widget_authoring_id(authoring_root, "text_input/background"));
    } else {
        add_rect(bucket,
                 0.0f,
                 0.0f,
                 width,
                 height,
                 background,
                 0x17E70002ull,
                 0,
                 0.0f,
                 make_widget_authoring_id(authoring_root, "text_input/background"));
    }

    float content_min_x = style.border_thickness + style.padding_x;
    float content_min_y = style.border_thickness + style.padding_y;
    float content_max_x = width - style.border_thickness - style.padding_x;
    float content_max_y = height - style.border_thickness - style.padding_y;
    float available_height = std::max(0.0f, content_max_y - content_min_y);

    auto display_text = state.text;
    bool show_placeholder = display_text.empty() && !state.placeholder.empty() && !state.focused;
    if (show_placeholder) {
        display_text = state.placeholder;
    }

    auto lines = multiline ? split_lines(display_text) : std::vector<TextLineSpan>{TextLineSpan{0, display_text.size()}};
    float line_height = style.typography.line_height;
    float baseline_top = content_min_y;
    if (!multiline) {
        float text_block_height = line_height;
        if (available_height > text_block_height) {
            baseline_top = content_min_y + (available_height - text_block_height) * 0.5f;
        }
    }

    auto text_color_value = show_placeholder
        ? make_placeholder_color(style, state)
        : make_text_color(style, state);

    std::uint32_t selection_start = state.selection_start;
    std::uint32_t selection_end = state.selection_end;
    std::uint32_t composition_start = state.composition_start;
    std::uint32_t composition_end = state.composition_end;
    clamp_range(selection_start, selection_end, state.text.size());
    clamp_range(composition_start, composition_end, state.text.size());

    auto highlight = highlight_color(style.selection_color, state);
    auto composition_highlight = highlight_color(style.composition_color, state);

    auto draw_highlight = [&](std::uint32_t range_start,
                              std::uint32_t range_end,
                              std::array<float, 4> color) {
        if (range_start >= range_end) {
            return;
        }
        std::size_t consumed = 0;
        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            auto const& span = lines[line_index];
            std::size_t line_length = span.end - span.start;
            if (line_length == 0 && range_start <= consumed && range_end == consumed) {
                float line_top = baseline_top + (line_height + line_spacing) * static_cast<float>(line_index) - scroll_y;
                add_rect(bucket,
                         content_min_x - scroll_x,
                         line_top,
                         content_min_x + 2.0f - scroll_x,
                         line_top + line_height,
                         color,
                         0x17E70003ull,
                         2,
                         0.5f,
                         make_widget_authoring_id(authoring_root, "text_input/selection"));
            }

            std::size_t line_begin_index = consumed;
            std::size_t line_end_index = consumed + line_length;
            std::size_t highlight_begin = std::clamp<std::size_t>(range_start, line_begin_index, line_end_index);
            std::size_t highlight_end = std::clamp<std::size_t>(range_end, line_begin_index, line_end_index);
            if (highlight_begin < highlight_end) {
                auto local_start = highlight_begin - line_begin_index;
                auto local_end = highlight_end - line_begin_index;
                std::string_view line_view(display_text.data() + span.start, line_length);
                std::string_view prefix = line_view.substr(0, local_start);
                std::string_view selection = line_view.substr(local_start, local_end - local_start);
                float prefix_width = measure_text(prefix, style.typography);
                float selection_width = measure_text(selection, style.typography);
                float line_top = baseline_top + (line_height + line_spacing) * static_cast<float>(line_index) - scroll_y;
                float min_x = content_min_x + prefix_width - scroll_x;
                float max_x = min_x + selection_width;
                min_x = std::max(min_x, content_min_x - scroll_x);
                max_x = std::min(max_x, content_max_x - scroll_x);
                add_rect(bucket,
                         min_x,
                         line_top,
                         max_x,
                         line_top + line_height,
                         color,
                         0x17E70003ull,
                         1,
                         0.25f,
                         make_widget_authoring_id(authoring_root, "text_input/selection"));
            }
            consumed += line_length;
            if (line_index + 1 < lines.size()) {
                consumed += 1; // newline
            }
        }
    };

    if (!show_placeholder) {
        draw_highlight(selection_start, selection_end, highlight);
        if (state.composition_active) {
            draw_highlight(composition_start, composition_end, composition_highlight);
        }
    }

    auto draw_text_line = [&](TextLineSpan const& span,
                              std::size_t line_index,
                              std::array<float, 4> color) {
        std::string_view line_view(display_text.data() + span.start, span.end - span.start);
        float line_baseline = baseline_top + (line_height + line_spacing) * static_cast<float>(line_index) - scroll_y;
        float origin_x = content_min_x - scroll_x;
        auto drawable_id = 0x17E70010ull + static_cast<std::uint64_t>(line_index);
        auto bucket_id = make_widget_authoring_id(authoring_root,
                                                  multiline ? "text_input/text_line" : "text_input/text");
        auto build = Text::BuildTextBucket(line_view,
                                           origin_x,
                                           line_baseline,
                                           style.typography,
                                           color,
                                           drawable_id,
                                           std::string(bucket_id),
                                           2.0f + static_cast<float>(line_index) * 0.01f);
        if (build) {
            append_bucket(bucket, build->bucket);
        }
    };

    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        draw_text_line(lines[line_index], line_index, text_color_value);
    }

    if (!show_placeholder && state.focused && (selection_start == selection_end)) {
        std::size_t caret_index = selection_end;
        std::size_t consumed = 0;
        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            auto const& span = lines[line_index];
            std::size_t line_length = span.end - span.start;
            std::size_t line_begin_index = consumed;
            std::size_t line_end_index = consumed + line_length;
            if (caret_index >= line_begin_index && caret_index <= line_end_index) {
                std::size_t local_index = caret_index - line_begin_index;
                std::string_view line_view(display_text.data() + span.start, line_length);
                std::string_view prefix = line_view.substr(0, local_index);
                float prefix_width = measure_text(prefix, style.typography);
                float line_top = baseline_top + (line_height + line_spacing) * static_cast<float>(line_index) - scroll_y;
                float caret_x = content_min_x + prefix_width - scroll_x;
                float caret_thickness = 1.5f;
                auto caret_col = caret_color(style.caret_color, state);
                auto authoring_id = make_widget_authoring_id(authoring_root, "text_input/caret");
                add_rect(bucket,
                         caret_x,
                         line_top,
                         caret_x + caret_thickness,
                         line_top + line_height,
                         caret_col,
                         0x17E70020ull,
                         6,
                         0.75f,
                         authoring_id);
                break;
            }
            consumed += line_length;
            if (line_index + 1 < lines.size()) {
                consumed += 1;
            }
        }
    }

    if (state.focused) {
        append_focus_highlight(bucket,
                               width,
                               height,
                               authoring_root,
                               pulsing_highlight,
                               lighten_color(border, 0.12f));
    }

    return bucket;
}

inline auto build_text_field_bucket(Widgets::TextFieldStyle const& style,
                                    Widgets::TextFieldState const& state,
                                    std::string_view authoring_root,
                                    bool pulsing_highlight = false) -> SceneData::DrawableBucketSnapshot {
    auto sanitized_style = sanitize_text_field_style(style);
    auto sanitized_state = sanitize_text_field_state(state, sanitized_style);
    return build_text_input_bucket(sanitized_style,
                                   sanitized_state,
                                   authoring_root,
                                   pulsing_highlight,
                                   false,
                                   0.0f,
                                   0.0f,
                                   0.0f);
}

inline auto build_text_area_bucket(Widgets::TextAreaStyle const& style,
                                   Widgets::TextAreaState const& state,
                                   std::string_view authoring_root,
                                   bool pulsing_highlight = false) -> SceneData::DrawableBucketSnapshot {
    auto sanitized_style = sanitize_text_area_style(style);
    auto sanitized_state = sanitize_text_area_state(state, sanitized_style);
    return build_text_input_bucket(sanitized_style,
                                   sanitized_state,
                                   authoring_root,
                                   pulsing_highlight,
                                   true,
                                   sanitized_state.scroll_x,
                                   sanitized_state.scroll_y,
                                   sanitized_style.line_spacing);
}

inline auto publish_text_field_state_scenes(PathSpace& space,
                                     AppRootPathView appRoot,
                                     std::string_view name,
                                     Widgets::TextFieldStyle const& style,
                                     Widgets::TextFieldState const& state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    Widgets::WidgetStateScenes scenes{};
    SP::UI::Runtime::Text::ScopedShapingContext shaping_ctx(space, appRoot);
    struct Variant {
        std::string_view suffix;
        Widgets::TextFieldState state;
        ScenePath* target = nullptr;
    };

    auto base = sanitize_text_field_state(state, sanitize_text_field_style(style));

    Variant variants[]{
        {"idle", Widgets::TextFieldState{base}, &scenes.idle},
        {"hover", Widgets::TextFieldState{base}, &scenes.hover},
        {"focused", Widgets::TextFieldState{base}, &scenes.pressed},
        {"disabled", Widgets::TextFieldState{base}, &scenes.disabled},
    };

    variants[1].state.hovered = true;
    variants[2].state.focused = true;
    variants[3].state.enabled = false;
    variants[3].state.focused = false;
    variants[3].state.hovered = false;

    auto const& authoring_root = widgetRoot->getPath();
    auto pulsing = Widgets::Focus::PulsingHighlightEnabled(space, appRoot);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.suffix,
                                                   "Widget text field state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_text_field_bucket(style,
                                              variant.state,
                                              authoring_root,
                                              *pulsing && variant.state.focused);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }

    return scenes;
}

inline auto publish_text_area_state_scenes(PathSpace& space,
                                    AppRootPathView appRoot,
                                    std::string_view name,
                                    Widgets::TextAreaStyle const& style,
                                    Widgets::TextAreaState const& state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    Widgets::WidgetStateScenes scenes{};
    SP::UI::Runtime::Text::ScopedShapingContext shaping_ctx(space, appRoot);
    auto base = sanitize_text_area_state(state, sanitize_text_area_style(style));

    struct Variant {
        std::string_view suffix;
        Widgets::TextAreaState state;
        ScenePath* target = nullptr;
    };

    Variant variants[]{
        {"idle", Widgets::TextAreaState{base}, &scenes.idle},
        {"hover", Widgets::TextAreaState{base}, &scenes.hover},
        {"focused", Widgets::TextAreaState{base}, &scenes.pressed},
        {"disabled", Widgets::TextAreaState{base}, &scenes.disabled},
    };

    variants[1].state.hovered = true;
    variants[2].state.focused = true;
    variants[3].state.enabled = false;
    variants[3].state.focused = false;
    variants[3].state.hovered = false;

    auto const& authoring_root = widgetRoot->getPath();
    auto pulsing = Widgets::Focus::PulsingHighlightEnabled(space, appRoot);
    if (!pulsing) {
        return std::unexpected(pulsing.error());
    }

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.suffix,
                                                   "Widget text area state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_text_area_bucket(style,
                                             variant.state,
                                             authoring_root,
                                             *pulsing && variant.state.focused);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

template <typename Style>
inline auto text_input_dirty_hint(Style const& style) -> DirtyRectHint {
    return ensure_valid_hint(make_default_dirty_rect(style.width, style.height));
}
