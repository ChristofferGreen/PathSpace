#include "DescriptorDetail.hpp"

#include "../WidgetDetail.hpp"
#include <pathspace/ui/declarative/Detail.hpp>

#include <pathspace/ui/runtime/TextRuntime.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>

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
namespace Detail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;
namespace ThemeConfig = SP::UI::Declarative::ThemeConfig;

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

auto ApplyThemeToButtonStyle(BuilderWidgets::ButtonStyle style,
                             BuilderWidgets::ButtonStyle const& theme_style)
    -> BuilderWidgets::ButtonStyle {
    using Field = BuilderWidgets::ButtonStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::BackgroundColor)) {
        style.background_color = theme_style.background_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::TextColor)) {
        style.text_color = theme_style.text_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Typography)) {
        style.typography = theme_style.typography;
    }
    return style;
}

auto ApplyThemeToToggleStyle(BuilderWidgets::ToggleStyle style,
                             BuilderWidgets::ToggleStyle const& theme_style)
    -> BuilderWidgets::ToggleStyle {
    using Field = BuilderWidgets::ToggleStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::TrackOff)) {
        style.track_off_color = theme_style.track_off_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::TrackOn)) {
        style.track_on_color = theme_style.track_on_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Thumb)) {
        style.thumb_color = theme_style.thumb_color;
    }
    return style;
}

auto ApplyThemeToSliderStyle(BuilderWidgets::SliderStyle style,
                             BuilderWidgets::SliderStyle const& theme_style)
    -> BuilderWidgets::SliderStyle {
    using Field = BuilderWidgets::SliderStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Track)) {
        style.track_color = theme_style.track_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Fill)) {
        style.fill_color = theme_style.fill_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Thumb)) {
        style.thumb_color = theme_style.thumb_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::LabelColor)) {
        style.label_color = theme_style.label_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::LabelTypography)) {
        style.label_typography = theme_style.label_typography;
    }
    return style;
}

auto ApplyThemeToListStyle(BuilderWidgets::ListStyle style,
                           BuilderWidgets::ListStyle const& theme_style)
    -> BuilderWidgets::ListStyle {
    using Field = BuilderWidgets::ListStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Background)) {
        style.background_color = theme_style.background_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Border)) {
        style.border_color = theme_style.border_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Item)) {
        style.item_color = theme_style.item_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::ItemHover)) {
        style.item_hover_color = theme_style.item_hover_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::ItemSelected)) {
        style.item_selected_color = theme_style.item_selected_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Separator)) {
        style.separator_color = theme_style.separator_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::ItemText)) {
        style.item_text_color = theme_style.item_text_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::ItemTypography)) {
        style.item_typography = theme_style.item_typography;
    }
    return style;
}

auto ApplyThemeToTreeStyle(BuilderWidgets::TreeStyle style,
                           BuilderWidgets::TreeStyle const& theme_style)
    -> BuilderWidgets::TreeStyle {
    using Field = BuilderWidgets::TreeStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Background)) {
        style.background_color = theme_style.background_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Border)) {
        style.border_color = theme_style.border_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Row)) {
        style.row_color = theme_style.row_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::RowHover)) {
        style.row_hover_color = theme_style.row_hover_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::RowSelected)) {
        style.row_selected_color = theme_style.row_selected_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::RowDisabled)) {
        style.row_disabled_color = theme_style.row_disabled_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Connector)) {
        style.connector_color = theme_style.connector_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Toggle)) {
        style.toggle_color = theme_style.toggle_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Text)) {
        style.text_color = theme_style.text_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::LabelTypography)) {
        style.label_typography = theme_style.label_typography;
    }
    return style;
}

auto ApplyThemeToTextFieldStyle(BuilderWidgets::TextFieldStyle style,
                                BuilderWidgets::TextFieldStyle const& theme_style)
    -> BuilderWidgets::TextFieldStyle {
    using Field = BuilderWidgets::TextFieldStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Background)) {
        style.background_color = theme_style.background_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Border)) {
        style.border_color = theme_style.border_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Text)) {
        style.text_color = theme_style.text_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Placeholder)) {
        style.placeholder_color = theme_style.placeholder_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Selection)) {
        style.selection_color = theme_style.selection_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Composition)) {
        style.composition_color = theme_style.composition_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Caret)) {
        style.caret_color = theme_style.caret_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Typography)) {
        style.typography = theme_style.typography;
    }
    return style;
}

auto ApplyThemeToTextAreaStyle(BuilderWidgets::TextAreaStyle style,
                               BuilderWidgets::TextAreaStyle const& theme_style)
    -> BuilderWidgets::TextAreaStyle {
    using Field = BuilderWidgets::TextAreaStyleOverrideField;
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Background)) {
        style.background_color = theme_style.background_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Border)) {
        style.border_color = theme_style.border_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Text)) {
        style.text_color = theme_style.text_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Placeholder)) {
        style.placeholder_color = theme_style.placeholder_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Selection)) {
        style.selection_color = theme_style.selection_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Composition)) {
        style.composition_color = theme_style.composition_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Caret)) {
        style.caret_color = theme_style.caret_color;
    }
    if (!BuilderWidgets::HasStyleOverride(style.overrides, Field::Typography)) {
        style.typography = theme_style.typography;
    }
    return style;
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
    if (raw == "text_area") {
        return WidgetKind::TextArea;
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

auto ReadButtonDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ButtonDescriptor> {
    ButtonDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ButtonStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = ApplyThemeToButtonStyle(*style, theme.button);
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

auto ReadToggleDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ToggleDescriptor> {
    ToggleDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ToggleStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = ApplyThemeToToggleStyle(*style, theme.toggle);
    auto state = ReadRequired<BuilderWidgets::ToggleState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    return descriptor;
}

auto ReadSliderDescriptor(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<SliderDescriptor> {
    SliderDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::SliderStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = ApplyThemeToSliderStyle(*style, theme.slider);
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

auto ReadListDescriptor(PathSpace& space,
                        std::string const& root,
                        BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<ListDescriptor> {
    ListDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::ListStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = ApplyThemeToListStyle(*style, theme.list);
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

auto ReadTreeDescriptor(PathSpace& space,
                        std::string const& root,
                        BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<TreeDescriptor> {
    TreeDescriptor descriptor{};
    auto style = ReadRequired<BuilderWidgets::TreeStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = ApplyThemeToTreeStyle(*style, theme.tree);
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
                           SP::UI::Runtime::WidgetPath const& widget)
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
        auto candidate = ReadThemeOverride(space, current);
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
        auto active_theme = ThemeConfig::LoadActive(space, app_root_view);
        if (active_theme) {
            if (!active_theme->empty()) {
                theme_value = *active_theme;
            }
        } else {
            auto const& error = active_theme.error();
            if (error.code != SP::Error::Code::NoSuchPath
                && error.code != SP::Error::Code::NoObjectFound) {
                return std::unexpected(error);
            }
        }
        if (!theme_value.has_value() || theme_value->empty()) {
            auto system_theme = ThemeConfig::LoadSystemActive(space);
            if (!system_theme) {
                return std::unexpected(system_theme.error());
            }
            theme_value = *system_theme;
        }
    }

    auto sanitized = ThemeConfig::SanitizeName(*theme_value);
    auto resolved = ThemeConfig::Resolve(app_root_view, sanitized);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }    auto loaded = ThemeConfig::Load(space, *resolved);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    ThemeContext context{};
    context.theme = *loaded;
    context.name = sanitized;
    return context;
}

auto ReadInputFieldDescriptor(PathSpace& space,
                              SP::UI::Runtime::WidgetPath const& widget,
                              BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<InputFieldDescriptor> {
    auto root = widget.getPath();
    InputFieldDescriptor descriptor{};
    descriptor.state = BuilderWidgets::TextFieldState{};

    auto style_value = ReadOptionalValue<BuilderWidgets::TextFieldStyle>(space, root + "/meta/style");
    if (!style_value) {
        return std::unexpected(style_value.error());
    }
    if (style_value->has_value()) {
        descriptor.style = ApplyThemeToTextFieldStyle(**style_value, theme.text_field);
    } else {
        descriptor.style = theme.text_field;
    }

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

auto ReadTextAreaDescriptor(PathSpace& space,
                            SP::UI::Runtime::WidgetPath const& widget,
                            BuilderWidgets::WidgetTheme const& theme)
    -> SP::Expected<TextAreaDescriptor> {
    auto root = widget.getPath();
    TextAreaDescriptor descriptor{};

    auto style_value = ReadOptionalValue<BuilderWidgets::TextAreaStyle>(space, root + "/meta/style");
    if (!style_value) {
        return std::unexpected(style_value.error());
    }
    if (style_value->has_value()) {
        descriptor.style = ApplyThemeToTextAreaStyle(**style_value, theme.text_area);
    } else {
        descriptor.style = theme.text_area;
    }

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

    auto read_only = ReadOptionalValue<bool>(space, root + "/state/read_only");
    if (!read_only) {
        return std::unexpected(read_only.error());
    }
    descriptor.state.read_only = read_only->value_or(false);

    auto cursor = ReadOptionalValue<std::uint32_t>(space, root + "/state/cursor");
    if (!cursor) {
        return std::unexpected(cursor.error());
    }
    descriptor.state.cursor = cursor->value_or(
        static_cast<std::uint32_t>(descriptor.state.text.size()));

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

    auto composition_active = ReadOptionalValue<bool>(space, root + "/state/composition_active");
    if (!composition_active) {
        return std::unexpected(composition_active.error());
    }
    descriptor.state.composition_active = composition_active->value_or(false);

    auto composition_text = ReadOptionalValue<std::string>(space, root + "/state/composition_text");
    if (!composition_text) {
        return std::unexpected(composition_text.error());
    }
    descriptor.state.composition_text = composition_text->value_or(std::string{});

    auto composition_start = ReadOptionalValue<std::uint32_t>(space, root + "/state/composition_start");
    if (!composition_start) {
        return std::unexpected(composition_start.error());
    }
    descriptor.state.composition_start = composition_start->value_or(descriptor.state.cursor);

    auto composition_end = ReadOptionalValue<std::uint32_t>(space, root + "/state/composition_end");
    if (!composition_end) {
        return std::unexpected(composition_end.error());
    }
    descriptor.state.composition_end = composition_end->value_or(descriptor.state.composition_start);

    auto scroll_x = ReadOptionalValue<float>(space, root + "/state/scroll_x");
    if (!scroll_x) {
        return std::unexpected(scroll_x.error());
    }
    descriptor.state.scroll_x = scroll_x->value_or(0.0f);

    auto scroll_y = ReadOptionalValue<float>(space, root + "/state/scroll_y");
    if (!scroll_y) {
        return std::unexpected(scroll_y.error());
    }
    descriptor.state.scroll_y = scroll_y->value_or(0.0f);

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
    auto dirty_rects = ReadOptionalValue<std::vector<SP::UI::Runtime::DirtyRectHint>>(space,
                                                                                      root + "/render/buffer/pendingDirty");
    if (!dirty_rects) {
        return std::unexpected(dirty_rects.error());
    }
    descriptor.pending_dirty = dirty_rects->value_or(std::vector<SP::UI::Runtime::DirtyRectHint>{});
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
