#include "DescriptorDetail.hpp"

#include "../BuildersDetail.hpp"
#include "../WidgetDetail.hpp"

#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/LegacyBuildersDeprecation.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <charconv>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace SP::UI::Declarative::DescriptorDetail {
namespace Detail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

namespace {

template <typename T>
auto ReadRequired(PathSpace& space, std::string const& path) -> SP::Expected<T> {
    auto value = space.read<T, std::string>(path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return *value;
}

template <typename T>
auto ReadOptionalValue(PathSpace& space, std::string const& path) -> SP::Expected<std::optional<T>> {
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

auto parse_stroke_id(std::string const& id) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    auto result = std::from_chars(id.data(), id.data() + id.size(), value);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) {
        return std::nullopt;
    }
    return value;
}

auto ReadThemeOverride(PathSpace& space, std::string const& base)
    -> SP::Expected<std::optional<std::string>> {
    auto theme_path = base + "/style/theme";
    auto value = ReadOptionalValue<std::string>(space, theme_path);
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

} // namespace

auto MakeDescriptorError(std::string message, SP::Error::Code code) -> SP::Error {
    return Detail::make_error(std::move(message), code);
}

auto KindFromString(std::string_view raw) -> std::optional<WidgetKind> {
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

auto ReadLabelDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<LabelDescriptor> {
    LabelDescriptor descriptor{};
    auto text = ReadRequired<std::string>(space, root + "/state/text");
    if (!text) {
        return std::unexpected(text.error());
    }
    descriptor.text = *text;
    auto typography = ReadRequired<BuilderWidgets::TypographyStyle>(space, root + "/meta/typography");
    if (!typography) {
        return std::unexpected(typography.error());
    }
    descriptor.typography = *typography;
    auto color = ReadRequired<std::array<float, 4>>(space, root + "/meta/color");
    if (!color) {
        return std::unexpected(color.error());
    }
    descriptor.color = *color;
    return descriptor;
}

auto ReadButtonDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ButtonDescriptor> {
    ButtonDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ButtonStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = ReadRequired<BuilderWidgets::ButtonState>(space, root + "/state");
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

auto ReadToggleDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ToggleDescriptor> {
    ToggleDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ToggleStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = ReadRequired<BuilderWidgets::ToggleState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    return descriptor;
}

auto ReadSliderDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<SliderDescriptor> {
    SliderDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::SliderStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = ReadRequired<BuilderWidgets::SliderState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto range = ReadRequired<BuilderWidgets::SliderRange>(space, root + "/meta/range");
    if (!range) {
        return std::unexpected(range.error());
    }
    descriptor.range = *range;
    return descriptor;
}

auto ReadListDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ListDescriptor> {
    ListDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ListStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = ReadRequired<BuilderWidgets::ListState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto items = ReadRequired<std::vector<BuilderWidgets::ListItem>>(space, root + "/meta/items");
    if (!items) {
        return std::unexpected(items.error());
    }
    descriptor.items = *items;
    return descriptor;
}

auto ReadTreeDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<TreeDescriptor> {
    TreeDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::TreeStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = ReadRequired<BuilderWidgets::TreeState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto nodes = ReadRequired<std::vector<BuilderWidgets::TreeNode>>(space, root + "/meta/nodes");
    if (!nodes) {
        return std::unexpected(nodes.error());
    }
    descriptor.nodes = *nodes;
    return descriptor;
}

auto ReadStackDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<StackDescriptor> {
    StackDescriptor descriptor{};
    auto active_panel = ReadOptionalValue<std::string>(space, root + "/state/active_panel");
    if (!active_panel) {
        return std::unexpected(active_panel.error());
    }
    descriptor.active_panel = active_panel->value_or("");
    auto style = ReadOptionalValue<BuilderWidgets::StackLayoutStyle>(space, root + "/layout/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = style->value_or(BuilderWidgets::StackLayoutStyle{});

    auto layout_children = ReadOptionalValue<std::vector<BuilderWidgets::StackChildSpec>>(space,
                                                                                         root + "/layout/children");
    if (!layout_children) {
        return std::unexpected(layout_children.error());
    }
    descriptor.children = layout_children->value_or(std::vector<BuilderWidgets::StackChildSpec>{});

    auto layout_state = ReadOptionalValue<BuilderWidgets::StackLayoutState>(space, root + "/layout/computed");
    if (!layout_state) {
        return std::unexpected(layout_state.error());
    }
    descriptor.layout = layout_state->value_or(BuilderWidgets::StackLayoutState{});

    auto panels_root = root + "/panels";
    auto panels = space.listChildren(SP::ConcretePathStringView{panels_root});
    struct PanelRecord {
        StackPanelDescriptor panel;
        std::uint32_t order = 0;
    };
    std::vector<PanelRecord> ordered;
    ordered.reserve(panels.size());
    for (auto const& panel_name : panels) {
        PanelRecord record{};
        record.panel.id = panel_name;
        auto panel_root = panels_root + "/" + panel_name;
        auto order_value = space.read<std::uint32_t, std::string>(panel_root + "/order");
        if (order_value) {
            record.order = *order_value;
        }
        auto target_path = ReadOptionalValue<std::string>(space, panel_root + "/target");
        if (!target_path) {
            return std::unexpected(target_path.error());
        }
        if (target_path->has_value()) {
            record.panel.target = **target_path;
        }
        auto visible = ReadOptionalValue<bool>(space, panel_root + "/visible");
        if (!visible) {
            return std::unexpected(visible.error());
        }
        record.panel.visible = visible->value_or(false);
        ordered.push_back(std::move(record));
    }

    if (ordered.empty()) {
        auto children_root = root + "/children";
        auto children = space.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& panel_name : children) {
            StackPanelDescriptor panel{};
            panel.id = panel_name;
            auto target_path = ReadOptionalValue<std::string>(space, children_root + "/" + panel_name + "/target");
            if (target_path && target_path->has_value()) {
                panel.target = **target_path;
            }
            panel.visible = panel_name == descriptor.active_panel;
            ordered.push_back(PanelRecord{std::move(panel), static_cast<std::uint32_t>(ordered.size())});
        }
    }

    std::sort(ordered.begin(), ordered.end(), [](PanelRecord const& lhs, PanelRecord const& rhs) {
        if (lhs.order == rhs.order) {
            return lhs.panel.id < rhs.panel.id;
        }
        return lhs.order < rhs.order;
    });
    for (auto& record : ordered) {
        descriptor.panels.push_back(std::move(record.panel));
    }
    return descriptor;
}

auto ResolveThemeForWidget(PathSpace& space,
                           SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<ThemeContext> {
    auto widget_root = widget.getPath();
    auto app_root = Detail::derive_app_root_for(SP::App::ConcretePathView{widget_root});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    SP::App::AppRootPathView app_root_view{app_root->getPath()};
    std::optional<std::string> theme_value;
    bool found_override = false;

    std::string current = widget_root;
    auto const& app_root_raw = app_root->getPath();
    while (!current.empty()) {
        auto candidate = ReadThemeOverride(space, current);
        if (!candidate) {
            return std::unexpected(candidate.error());
        }
        if (candidate->has_value()) {
            theme_value = **candidate;
            found_override = current.find("/widgets/") != std::string::npos;
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
        auto default_theme = ReadOptionalValue<std::string>(space, app_root_raw + "/themes/default");
        if (!default_theme) {
            return std::unexpected(default_theme.error());
        }
        if (default_theme->has_value()) {
            theme_value = **default_theme;
        } else {
            theme_value = std::string{"default"};
        }
        found_override = false;
    }

    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(*theme_value);
    auto resolved = SP::UI::Builders::Config::Theme::Resolve(app_root_view, sanitized);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    SP::UI::LegacyBuilders::ScopedAllow theme_allow{};
    auto loaded = SP::UI::Builders::Config::Theme::Load(space, *resolved);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    ThemeContext context{};
    context.theme = *loaded;
    context.name = sanitized;
    context.has_override = found_override;
    return context;
}

void ApplyThemeOverride(ThemeContext const& theme, WidgetDescriptor& descriptor) {
    if (!theme.has_override) {
        return;
    }

    switch (descriptor.kind) {
    case WidgetKind::Button: {
        auto& data = std::get<ButtonDescriptor>(descriptor.data);
        data.style = theme.theme.button;
        break;
    }
    case WidgetKind::Toggle: {
        auto& data = std::get<ToggleDescriptor>(descriptor.data);
        data.style = theme.theme.toggle;
        break;
    }
    case WidgetKind::Slider: {
        auto& data = std::get<SliderDescriptor>(descriptor.data);
        data.style = theme.theme.slider;
        break;
    }
    case WidgetKind::List: {
        auto& data = std::get<ListDescriptor>(descriptor.data);
        data.style = theme.theme.list;
        break;
    }
    case WidgetKind::Tree: {
        auto& data = std::get<TreeDescriptor>(descriptor.data);
        data.style = theme.theme.tree;
        break;
    }
    default:
        break;
    }
}

auto ReadInputFieldDescriptor(PathSpace& space,
                              SP::UI::Builders::WidgetPath const& widget,
                              BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<InputFieldDescriptor> {
    auto root = widget.getPath();
    InputFieldDescriptor descriptor{};
    descriptor.style = theme.text_field;
    descriptor.state = BuilderWidgets::TextFieldState{};

    auto text = ReadOptionalValue<std::string>(space, root + "/state/text");
    if (!text) {
        return std::unexpected(text.error());
    }
    descriptor.state.text = text->value_or(std::string{});

    auto placeholder = ReadOptionalValue<std::string>(space, root + "/state/placeholder");
    if (!placeholder) {
        return std::unexpected(placeholder.error());
    }
    descriptor.state.placeholder = placeholder->value_or(std::string{});

    auto focused = ReadOptionalValue<bool>(space, root + "/state/focused");
    if (!focused) {
        return std::unexpected(focused.error());
    }
    descriptor.state.focused = focused->value_or(false);

    auto hovered = ReadOptionalValue<bool>(space, root + "/state/hovered");
    if (!hovered) {
        return std::unexpected(hovered.error());
    }
    descriptor.state.hovered = hovered->value_or(false);

    auto enabled = ReadOptionalValue<bool>(space, root + "/state/enabled");
    if (!enabled) {
        return std::unexpected(enabled.error());
    }
    descriptor.state.enabled = enabled->value_or(true);

    auto cursor = ReadOptionalValue<std::uint32_t>(space, root + "/state/cursor");
    if (!cursor) {
        return std::unexpected(cursor.error());
    }
    descriptor.state.cursor = cursor->value_or(static_cast<std::uint32_t>(descriptor.state.text.size()));

    auto selection_start = ReadOptionalValue<std::uint32_t>(space, root + "/state/selection_start");
    if (!selection_start) {
        return std::unexpected(selection_start.error());
    }
    descriptor.state.selection_start = selection_start->value_or(descriptor.state.cursor);

    auto selection_end = ReadOptionalValue<std::uint32_t>(space, root + "/state/selection_end");
    if (!selection_end) {
        return std::unexpected(selection_end.error());
    }
    descriptor.state.selection_end = selection_end->value_or(descriptor.state.selection_start);

    return descriptor;
}

auto ReadPaintSurfaceDescriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<PaintSurfaceDescriptor> {
    PaintSurfaceDescriptor descriptor{};
    auto brush_size = ReadOptionalValue<float>(space, root + "/state/brush/size");
    if (!brush_size) {
        return std::unexpected(brush_size.error());
    }
    descriptor.brush_size = brush_size->value_or(0.0f);
    auto brush_color = ReadOptionalValue<std::array<float, 4>>(space, root + "/state/brush/color");
    if (!brush_color) {
        return std::unexpected(brush_color.error());
    }
    descriptor.brush_color = brush_color->value_or(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
    auto gpu_flag = ReadOptionalValue<bool>(space, root + "/render/gpu/enabled");
    if (!gpu_flag) {
        return std::unexpected(gpu_flag.error());
    }
    descriptor.gpu_enabled = gpu_flag->value_or(false);
    auto gpu_state_value = ReadOptionalValue<std::string>(space, root + "/render/gpu/state");
    if (!gpu_state_value) {
        return std::unexpected(gpu_state_value.error());
    }
    descriptor.gpu_ready = gpu_state_value->value_or("Idle") == "Ready";
    auto buffer_metrics = ReadOptionalValue<PaintBufferMetrics>(space, root + "/render/buffer/metrics");
    if (!buffer_metrics) {
        return std::unexpected(buffer_metrics.error());
    }
    descriptor.buffer = buffer_metrics->value_or(PaintBufferMetrics{});
    auto dirty_rects = ReadOptionalValue<std::vector<SP::UI::Builders::DirtyRectHint>>(space,
                                                                                      root + "/render/buffer/pendingDirty");
    if (!dirty_rects) {
        return std::unexpected(dirty_rects.error());
    }
    descriptor.pending_dirty = dirty_rects->value_or(std::vector<SP::UI::Builders::DirtyRectHint>{});
    auto viewport = ReadOptionalValue<PaintBufferViewport>(space, root + "/render/buffer/viewport");
    if (!viewport) {
        return std::unexpected(viewport.error());
    }
    descriptor.viewport = viewport->value_or(PaintBufferViewport{});
    auto buffer_revision = ReadOptionalValue<std::uint64_t>(space, root + "/render/buffer/revision");
    if (!buffer_revision) {
        return std::unexpected(buffer_revision.error());
    }
    descriptor.buffer_revision = buffer_revision->value_or(0);
    auto texture_payload = ReadOptionalValue<PaintTexturePayload>(space, root + "/assets/texture");
    if (!texture_payload) {
        return std::unexpected(texture_payload.error());
    }
    if (texture_payload->has_value()) {
        descriptor.texture = **texture_payload;
    } else {
        descriptor.texture.reset();
    }
    auto gpu_stats = ReadOptionalValue<PaintGpuStats>(space, root + "/render/gpu/stats");
    if (!gpu_stats) {
        return std::unexpected(gpu_stats.error());
    }
    descriptor.gpu_stats = gpu_stats->value_or(PaintGpuStats{});

    auto strokes_root = root + "/state/history";
    auto stroke_ids = space.listChildren(SP::ConcretePathStringView{strokes_root});
    for (auto const& id : stroke_ids) {
        auto parsed = parse_stroke_id(id);
        if (!parsed) {
            continue;
        }
        auto stroke_root = strokes_root + "/" + id;
        auto meta = ReadRequired<PaintStrokeMeta>(space, stroke_root + "/meta");
        if (!meta) {
            return std::unexpected(meta.error());
        }
        PaintSurfaceStrokeDescriptor stroke{};
        stroke.id = *parsed;
        stroke.meta = *meta;
        auto points = PaintRuntime::ReadStrokePointsConsistent(space, root, *parsed);
        if (!points) {
            return std::unexpected(points.error());
        }
        stroke.points = std::move(*points);
        descriptor.strokes.push_back(std::move(stroke));
    }

    return descriptor;
}

} // namespace SP::UI::Declarative::DescriptorDetail
