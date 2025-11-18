#include <pathspace/ui/declarative/Descriptor.hpp>

#include "../BuildersDetail.hpp"
#include "../WidgetDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/SceneUtilities.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace SP::UI::Declarative {
namespace {

namespace Detail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

auto make_descriptor_error(std::string message,
                           SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return Detail::make_error(std::move(message), code);
}

auto kind_from_string(std::string_view raw) -> std::optional<WidgetKind> {
    if (raw == "button") {
        return WidgetKind::Button;
    }
    if (raw == "toggle") {
        return WidgetKind::Toggle;
    }
    if (raw == "slider") {
        return WidgetKind::Slider;
    }
    if (raw == "list") {
        return WidgetKind::List;
    }
    if (raw == "tree") {
        return WidgetKind::Tree;
    }
    if (raw == "stack") {
        return WidgetKind::Stack;
    }
    if (raw == "label") {
        return WidgetKind::Label;
    }
    if (raw == "input_field") {
        return WidgetKind::InputField;
    }
    if (raw == "paint_surface") {
        return WidgetKind::PaintSurface;
    }
    return std::nullopt;
}

template <typename T>
auto read_required(PathSpace& space, std::string const& path) -> SP::Expected<T> {
    auto value = space.read<T, std::string>(path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return *value;
}

auto read_label_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<LabelDescriptor> {
    LabelDescriptor descriptor{};
    auto text = read_required<std::string>(space, root + "/state/text");
    if (!text) {
        return std::unexpected(text.error());
    }
    descriptor.text = *text;
    auto typography = read_required<BuilderWidgets::TypographyStyle>(space, root + "/meta/typography");
    if (!typography) {
        return std::unexpected(typography.error());
    }
    descriptor.typography = *typography;
    auto color = read_required<std::array<float, 4>>(space, root + "/meta/color");
    if (!color) {
        return std::unexpected(color.error());
    }
    descriptor.color = *color;
    return descriptor;
}

auto read_button_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ButtonDescriptor> {
    ButtonDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ButtonStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ButtonState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto label = space.read<std::string, std::string>(root + "/meta/label");
    if (label) {
        descriptor.label = *label;
    } else {
        auto const& err = label.error();
        if (err.code != SP::Error::Code::NoSuchPath
            && err.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(err);
        }
    }
    return descriptor;
}

auto read_toggle_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ToggleDescriptor> {
    ToggleDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ToggleStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ToggleState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    return descriptor;
}

auto read_slider_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<SliderDescriptor> {
    SliderDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::SliderStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::SliderState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto range = read_required<BuilderWidgets::SliderRange>(space, root + "/meta/range");
    if (!range) {
        return std::unexpected(range.error());
    }
    descriptor.range = *range;
    return descriptor;
}

auto read_list_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ListDescriptor> {
    ListDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ListStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ListState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto items = space.read<std::vector<BuilderWidgets::ListItem>, std::string>(root + "/meta/items");
    if (items) {
        descriptor.items = *items;
    } else {
        auto const& err = items.error();
        if (err.code != SP::Error::Code::NoSuchPath
            && err.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(err);
        }
    }
    return descriptor;
}

auto read_tree_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<TreeDescriptor> {
    TreeDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::TreeStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::TreeState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto nodes = space.read<std::vector<BuilderWidgets::TreeNode>, std::string>(root + "/meta/nodes");
    if (nodes) {
        descriptor.nodes = *nodes;
    } else {
        return std::unexpected(nodes.error());
    }
    return descriptor;
}

template <typename T>
auto read_optional_value(PathSpace& space, std::string const& path) -> SP::Expected<std::optional<T>> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const code = value.error().code;
    if (code == SP::Error::Code::NoSuchPath || code == SP::Error::Code::NoObjectFound) {
        return std::optional<T>{};
    }
    return std::unexpected(value.error());
}

auto read_theme_override(PathSpace& space, std::string const& base) -> SP::Expected<std::optional<std::string>> {
    auto theme_path = base + "/style/theme";
    auto value = read_optional_value<std::string>(space, theme_path);
    if (!value) {
        return value;
    }
    if (value->has_value()) {
        auto trimmed = value->value();
        if (!trimmed.empty()) {
            return value;
        }
    }
    return std::optional<std::string>{};
}

struct ThemeContext {
    BuilderWidgets::WidgetTheme theme{};
    std::string name;
};

constexpr std::size_t kMaxThemeInheritanceDepth = 16;

auto load_theme_with_inheritance(PathSpace& space,
                                 SP::App::AppRootPathView app_root,
                                 std::string_view requested)
    -> SP::Expected<BuilderWidgets::WidgetTheme> {
    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(requested);
    std::unordered_set<std::string> visited;
    visited.reserve(4);

    for (std::size_t depth = 0; depth < kMaxThemeInheritanceDepth; ++depth) {
        if (!visited.insert(sanitized).second) {
            return std::unexpected(
                make_descriptor_error("theme inheritance cycle detected at '" + sanitized + "'",
                                      SP::Error::Code::InvalidType));
        }

        auto resolved = SP::UI::Builders::Config::Theme::Resolve(app_root, sanitized);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        auto loaded = SP::UI::Builders::Config::Theme::Load(space, *resolved);
        if (loaded) {
            return *loaded;
        }

        auto const code = loaded.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(loaded.error());
        }

        auto inherits_path = resolved->root.getPath() + "/style/inherits";
        auto inherits_value = read_optional_value<std::string>(space, inherits_path);
        if (!inherits_value) {
            return std::unexpected(inherits_value.error());
        }
        if (!inherits_value->has_value() || inherits_value->value().empty()) {
            return std::unexpected(make_descriptor_error(
                "theme '" + sanitized + "' missing value and inherits",
                SP::Error::Code::NoSuchPath));
        }
        sanitized = SP::UI::Builders::Config::Theme::SanitizeName(**inherits_value);
    }

    return std::unexpected(
        make_descriptor_error("theme inheritance depth exceeded",
                              SP::Error::Code::CapacityExceeded));
}

auto resolve_theme_for_widget(PathSpace& space,
                              SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<ThemeContext> {
    auto widget_root = widget.getPath();
    auto app_root = Detail::derive_app_root_for(SP::App::ConcretePathView{widget_root});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    SP::App::AppRootPathView app_root_view{app_root->getPath()};
    std::optional<std::string> theme_value;

    std::string current = widget_root;
    auto const& app_root_raw = app_root->getPath();
    while (!current.empty()) {
        auto candidate = read_theme_override(space, current);
        if (!candidate) {
            return std::unexpected(candidate.error());
        }
        if (candidate->has_value()) {
            theme_value = **candidate;
            break;
        }
        if (current == app_root_raw) {
            break;
        }
        auto slash = current.find_last_of('/');
        if (slash == std::string::npos) {
            break;
        }
        if (slash == 0) {
            current = "/";
        } else {
            current = current.substr(0, slash);
        }
    }

    if (!theme_value.has_value() || theme_value->empty()) {
        auto default_theme = read_optional_value<std::string>(space, app_root_raw + "/themes/default");
        if (!default_theme) {
            return std::unexpected(default_theme.error());
        }
        if (default_theme->has_value()) {
            theme_value = **default_theme;
        } else {
            theme_value = std::string{"default"};
        }
    }

    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(*theme_value);
    auto loaded = load_theme_with_inheritance(space, app_root_view, sanitized);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    ThemeContext context{};
    context.theme = *loaded;
    context.name = sanitized;
    return context;
}

auto read_stack_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<StackDescriptor> {
    StackDescriptor descriptor{};
    auto active_panel = read_optional_value<std::string>(space, root + "/state/active_panel");
    if (!active_panel) {
        return std::unexpected(active_panel.error());
    }
    descriptor.active_panel = active_panel->value_or(std::string{});

    auto panels_root = root + "/panels";
    auto panel_names = space.listChildren(SP::ConcretePathStringView{panels_root});
    for (auto const& panel_name : panel_names) {
        auto target_path = read_optional_value<std::string>(space, panels_root + "/" + panel_name + "/target");
        if (!target_path) {
            return std::unexpected(target_path.error());
        }
        if (!target_path->has_value()) {
            continue;
        }
        descriptor.panels.push_back(StackPanelDescriptor{
            .id = panel_name,
            .target = **target_path,
        });
    }
    return descriptor;
}

auto read_input_field_descriptor(PathSpace& space,
                                 SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<InputFieldDescriptor> {
    auto theme = resolve_theme_for_widget(space, widget);
    if (!theme) {
        return std::unexpected(theme.error());
    }

    InputFieldDescriptor descriptor{};
    descriptor.style = theme->theme.text_field;
    descriptor.state = BuilderWidgets::TextFieldState{};
    descriptor.state.enabled = true;
    descriptor.state.cursor = 0;
    descriptor.state.selection_start = 0;
    descriptor.state.selection_end = 0;

    auto root = widget.getPath();
    auto text = read_optional_value<std::string>(space, root + "/state/text");
    if (!text) {
        return std::unexpected(text.error());
    }
    descriptor.state.text = text->value_or(std::string{});
    descriptor.state.cursor = static_cast<std::uint32_t>(descriptor.state.text.size());
    descriptor.state.selection_start = descriptor.state.cursor;
    descriptor.state.selection_end = descriptor.state.cursor;

    auto placeholder = read_optional_value<std::string>(space, root + "/state/placeholder");
    if (!placeholder) {
        return std::unexpected(placeholder.error());
    }
    descriptor.state.placeholder = placeholder->value_or(std::string{});

    auto focused = read_optional_value<bool>(space, root + "/state/focused");
    if (!focused) {
        return std::unexpected(focused.error());
    }
    descriptor.state.focused = focused->value_or(false);

    auto hovered = read_optional_value<bool>(space, root + "/state/hovered");
    if (!hovered) {
        return std::unexpected(hovered.error());
    }
    descriptor.state.hovered = hovered->value_or(false);

    auto enabled = read_optional_value<bool>(space, root + "/state/enabled");
    if (!enabled) {
        return std::unexpected(enabled.error());
    }
    if (enabled->has_value()) {
        descriptor.state.enabled = **enabled;
    }

    auto read_cursor = read_optional_value<std::uint32_t>(space, root + "/state/cursor");
    if (!read_cursor) {
        return std::unexpected(read_cursor.error());
    }
    if (read_cursor->has_value()) {
        descriptor.state.cursor = **read_cursor;
    }

    auto selection_start = read_optional_value<std::uint32_t>(space, root + "/state/selection_start");
    if (!selection_start) {
        return std::unexpected(selection_start.error());
    }
    if (selection_start->has_value()) {
        descriptor.state.selection_start = **selection_start;
    }

    auto selection_end = read_optional_value<std::uint32_t>(space, root + "/state/selection_end");
    if (!selection_end) {
        return std::unexpected(selection_end.error());
    }
    if (selection_end->has_value()) {
        descriptor.state.selection_end = **selection_end;
    }

    auto composition_active = read_optional_value<bool>(space, root + "/state/composition_active");
    if (!composition_active) {
        return std::unexpected(composition_active.error());
    }
    descriptor.state.composition_active = composition_active->value_or(false);

    auto composition_text = read_optional_value<std::string>(space, root + "/state/composition_text");
    if (!composition_text) {
        return std::unexpected(composition_text.error());
    }
    descriptor.state.composition_text = composition_text->value_or(std::string{});

    auto composition_start = read_optional_value<std::uint32_t>(space, root + "/state/composition_start");
    if (!composition_start) {
        return std::unexpected(composition_start.error());
    }
    descriptor.state.composition_start = composition_start->value_or(0u);

    auto composition_end = read_optional_value<std::uint32_t>(space, root + "/state/composition_end");
    if (!composition_end) {
        return std::unexpected(composition_end.error());
    }
    descriptor.state.composition_end = composition_end->value_or(descriptor.state.composition_start);

    return descriptor;
}

auto read_paint_surface_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<PaintSurfaceDescriptor> {
    PaintSurfaceDescriptor descriptor{};

    auto size_value = read_optional_value<float>(space, root + "/state/brush/size");
    if (!size_value) {
        return std::unexpected(size_value.error());
    }
    descriptor.brush_size = size_value->value_or(6.0f);

    auto color_value = read_optional_value<std::array<float, 4>>(space, root + "/state/brush/color");
    if (!color_value) {
        return std::unexpected(color_value.error());
    }
    descriptor.brush_color = color_value->value_or(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});

    auto gpu_flag = read_optional_value<bool>(space, root + "/render/gpu/enabled");
    if (!gpu_flag) {
        return std::unexpected(gpu_flag.error());
    }
    descriptor.gpu_enabled = gpu_flag->value_or(false);

    auto gpu_state_value = read_optional_value<std::string>(space, root + "/render/gpu/state");
    if (!gpu_state_value) {
        return std::unexpected(gpu_state_value.error());
    }
    descriptor.gpu_ready = PaintGpuStateFromString(gpu_state_value->value_or("Idle")) == PaintGpuState::Ready;

    auto dirty_batch = read_optional_value<std::vector<SP::UI::Builders::DirtyRectHint>>(space,
                                                                                        root + "/render/buffer/pendingDirty");
    if (!dirty_batch) {
        return std::unexpected(dirty_batch.error());
    }
    descriptor.pending_dirty = dirty_batch->value_or(std::vector<SP::UI::Builders::DirtyRectHint>{});

    auto gpu_stats = read_optional_value<PaintGpuStats>(space, root + "/render/gpu/stats");
    if (!gpu_stats) {
        return std::unexpected(gpu_stats.error());
    }
    if (gpu_stats->has_value()) {
        descriptor.gpu_stats = **gpu_stats;
    }

    auto texture_payload = read_optional_value<PaintTexturePayload>(space, root + "/assets/texture");
    if (!texture_payload) {
        return std::unexpected(texture_payload.error());
    }
    if (texture_payload->has_value()) {
        PaintTexturePayload payload = **texture_payload;
        payload.pixels.clear();
        descriptor.texture = std::move(payload);
    }

    auto buffer_metrics = PaintRuntime::ReadBufferMetrics(space, root);
    if (!buffer_metrics) {
        return std::unexpected(buffer_metrics.error());
    }
    descriptor.buffer = *buffer_metrics;

    auto strokes = PaintRuntime::LoadStrokeRecords(space, root);
    if (!strokes) {
        return std::unexpected(strokes.error());
    }

    descriptor.strokes.reserve(strokes->size());
    for (auto& record : *strokes) {
        PaintSurfaceStrokeDescriptor stroke{};
        stroke.id = record.id;
        stroke.meta = record.meta;
        stroke.points = record.points;
        descriptor.strokes.push_back(std::move(stroke));
    }

    return descriptor;
}

struct BucketVisitor {
    DescriptorBucketOptions options;
    std::string authoring_root;

    auto operator()(ButtonDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ButtonPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildButtonPreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(ToggleDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TogglePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildTogglePreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(SliderDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::SliderPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildSliderPreview(descriptor.style,
                                           descriptor.range,
                                           descriptor.state,
                                           preview);
    }

    auto operator()(ListDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ListPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildListPreview(descriptor.style,
                                                std::span<BuilderWidgets::ListItem const>{descriptor.items},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(TreeDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TreePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildTreePreview(descriptor.style,
                                                std::span<BuilderWidgets::TreeNode const>{descriptor.nodes},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(LabelDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        auto drawable_id = std::hash<std::string>{}(authoring_root);
        auto built = SP::UI::Builders::Text::BuildTextBucket(descriptor.text,
                                                              0.0f,
                                                              descriptor.typography.line_height,
                                                              descriptor.typography,
                                                              descriptor.color,
                                                              drawable_id,
                                                              authoring_root + "/label",
                                                              0.0f);
        if (!built) {
            return std::unexpected(make_descriptor_error("Failed to build label bucket"));
        }
        return built->bucket;
    }

    auto operator()(StackDescriptor const&) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        return SP::UI::Scene::DrawableBucketSnapshot{};
    }

    auto operator()(InputFieldDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        auto bucket = Detail::build_text_field_bucket(descriptor.style,
                                                      descriptor.state,
                                                      authoring_root,
                                                      options.pulsing_highlight);
        return bucket;
    }

    auto operator()(PaintSurfaceDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        SP::UI::Scene::DrawableBucketSnapshot bucket{};
        std::size_t total_points = 0;
        for (auto const& stroke : descriptor.strokes) {
            total_points += stroke.points.size();
        }
        if (total_points == 0) {
            return bucket;
        }

        bucket.stroke_points.reserve(total_points);
        std::vector<std::uint32_t> opaque_indices;
        std::vector<std::uint32_t> alpha_indices;
        auto identity = SP::UI::Scene::MakeIdentityTransform();
        auto base_id = std::hash<std::string>{}(authoring_root);

        for (auto const& stroke : descriptor.strokes) {
            if (stroke.points.empty()) {
                continue;
            }

            auto command_index = static_cast<std::uint32_t>(bucket.command_kinds.size());
            auto point_offset = static_cast<std::uint32_t>(bucket.stroke_points.size());

            float min_x = std::numeric_limits<float>::max();
            float min_y = std::numeric_limits<float>::max();
            float max_x = std::numeric_limits<float>::lowest();
            float max_y = std::numeric_limits<float>::lowest();
            for (auto const& point : stroke.points) {
                SP::UI::Scene::StrokePoint scene_point{point.x, point.y};
                bucket.stroke_points.push_back(scene_point);
                min_x = std::min(min_x, point.x);
                min_y = std::min(min_y, point.y);
                max_x = std::max(max_x, point.x);
                max_y = std::max(max_y, point.y);
            }

            if (min_x > max_x) {
                min_x = 0.0f;
                min_y = 0.0f;
                max_x = 0.0f;
                max_y = 0.0f;
            }

            SP::UI::Scene::StrokeCommand command{};
            command.min_x = min_x;
            command.min_y = min_y;
            command.max_x = max_x;
            command.max_y = max_y;
            command.thickness = std::max(stroke.meta.brush_size, 0.5f);
            command.point_offset = point_offset;
            command.point_count = static_cast<std::uint32_t>(stroke.points.size());
            command.color = stroke.meta.color;

            auto payload_offset = bucket.command_payload.size();
            bucket.command_payload.resize(payload_offset + sizeof(command));
            std::memcpy(bucket.command_payload.data() + payload_offset, &command, sizeof(command));
            bucket.command_kinds.push_back(static_cast<std::uint32_t>(SP::UI::Scene::DrawCommandKind::Stroke));

            bucket.command_offsets.push_back(command_index);
            bucket.command_counts.push_back(1);

            auto drawable_id = base_id ^ stroke.id;
            bucket.drawable_ids.push_back(drawable_id);
            bucket.drawable_fingerprints.push_back(drawable_id);
            bucket.world_transforms.push_back(identity);

            SP::UI::Scene::BoundingBox box{};
            box.min = {min_x, min_y, 0.0f};
            box.max = {max_x, max_y, 0.0f};
            bucket.bounds_boxes.push_back(box);
            bucket.bounds_box_valid.push_back(1);

            auto center_x = (min_x + max_x) * 0.5f;
            auto center_y = (min_y + max_y) * 0.5f;
            auto radius = std::hypot(max_x - center_x, max_y - center_y);
            SP::UI::Scene::BoundingSphere sphere{};
            sphere.center = {center_x, center_y, 0.0f};
            sphere.radius = radius;
            bucket.bounds_spheres.push_back(sphere);

            bucket.layers.push_back(0);
            bucket.z_values.push_back(static_cast<float>(bucket.z_values.size()));
            bucket.material_ids.push_back(0);
            bucket.pipeline_flags.push_back(0);
            bucket.visibility.push_back(1);
            bucket.clip_head_indices.push_back(-1);

            if (stroke.meta.color[3] < 0.999f) {
                alpha_indices.push_back(static_cast<std::uint32_t>(bucket.drawable_ids.size() - 1));
            } else {
                opaque_indices.push_back(static_cast<std::uint32_t>(bucket.drawable_ids.size() - 1));
            }
        }

        bucket.opaque_indices = std::move(opaque_indices);
        bucket.alpha_indices = std::move(alpha_indices);
        return bucket;
    }
};

} // namespace

auto LoadWidgetDescriptor(PathSpace& space,
                          SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<WidgetDescriptor> {
    auto root = widget.getPath();
    auto removed = read_optional_value<bool>(space, root + "/state/removed");
    if (!removed) {
        return std::unexpected(removed.error());
    }
    if (removed->value_or(false)) {
        return std::unexpected(make_descriptor_error("Widget removed",
                                                     SP::Error::Code::NoObjectFound));
    }
    auto kind_value = space.read<std::string, std::string>(root + "/meta/kind");
    if (!kind_value) {
        return std::unexpected(kind_value.error());
    }
    auto kind = kind_from_string(*kind_value);
    if (!kind) {
        return std::unexpected(make_descriptor_error("Unsupported declarative widget kind: " + *kind_value,
                                                     SP::Error::Code::NotSupported));
    }

    WidgetDescriptor descriptor{};
    descriptor.kind = *kind;
    descriptor.widget = widget;

    switch (*kind) {
    case WidgetKind::Button: {
        auto loaded = read_button_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Toggle: {
        auto loaded = read_toggle_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Slider: {
        auto loaded = read_slider_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::List: {
        auto loaded = read_list_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Tree: {
        auto loaded = read_tree_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Label: {
        auto loaded = read_label_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Stack: {
        auto loaded = read_stack_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::InputField: {
        auto loaded = read_input_field_descriptor(space, widget);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::PaintSurface: {
        auto loaded = read_paint_surface_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    }

    return descriptor;
}

auto BuildWidgetBucket(WidgetDescriptor const& descriptor,
                       DescriptorBucketOptions const& options)
    -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
    BucketVisitor visitor{
        .options = options,
        .authoring_root = descriptor.widget.getPath(),
    };
    return std::visit(visitor, descriptor.data);
}

} // namespace SP::UI::Declarative
