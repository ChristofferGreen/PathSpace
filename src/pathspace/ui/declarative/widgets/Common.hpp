#pragma once

#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Declarative::Detail {

namespace BuilderWidgets = SP::UI::Runtime::Widgets;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::Widgets::WidgetSpaceRoot;
using SP::PathSpaceBase;

using DeclarativeDetail::make_error;

inline auto inherit_color_value() -> std::array<float, 4> {
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

inline auto inherit_typography_value() -> BuilderWidgets::TypographyStyle {
    BuilderWidgets::TypographyStyle typography{};
    typography.font_size = 0.0f;
    typography.line_height = 0.0f;
    typography.letter_spacing = 0.0f;
    typography.baseline_shift = 0.0f;
    typography.font_family.clear();
    typography.font_style.clear();
    typography.font_weight.clear();
    typography.language.clear();
    typography.direction.clear();
    typography.fallback_families.clear();
    typography.font_features.clear();
    typography.font_resource_root.clear();
    typography.font_active_revision = 0;
    typography.font_asset_fingerprint = 0;
    return typography;
}

template <typename Style, typename Enum>
inline auto scrub_color_if_inherited(Style& style,
                                     Enum field,
                                     std::array<float, 4>& slot) -> void {
    if (!BuilderWidgets::HasStyleOverride(style.overrides, field)) {
        slot = inherit_color_value();
    }
}

template <typename Style, typename Enum>
inline auto scrub_typography_if_inherited(Style& style,
                                          Enum field,
                                          BuilderWidgets::TypographyStyle& slot) -> void {
    if (!BuilderWidgets::HasStyleOverride(style.overrides, field)) {
        slot = inherit_typography_value();
    }
}

inline auto ensure_widget_name(std::string_view name) -> SP::Expected<void> {
    return DeclarativeDetail::ensure_identifier(name, "widget name");
}
inline auto ensure_child_name(std::string_view name) -> SP::Expected<void> {
    return DeclarativeDetail::ensure_identifier(name, "child name");
}

auto make_path(std::string base, std::string_view component) -> std::string;
auto mount_base(std::string_view parent, MountOptions const& options) -> std::string;

template <typename Style>
inline auto prepare_style_for_serialization(Style const& style) -> Style {
    return style;
}

inline auto prepare_style_for_serialization(BuilderWidgets::ButtonStyle const& style)
    -> BuilderWidgets::ButtonStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ButtonStyleOverrideField::BackgroundColor,
                             prepared.background_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ButtonStyleOverrideField::TextColor,
                             prepared.text_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::ButtonStyleOverrideField::Typography,
                                  prepared.typography);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::ToggleStyle const& style)
    -> BuilderWidgets::ToggleStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ToggleStyleOverrideField::TrackOff,
                             prepared.track_off_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ToggleStyleOverrideField::TrackOn,
                             prepared.track_on_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ToggleStyleOverrideField::Thumb,
                             prepared.thumb_color);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::SliderStyle const& style)
    -> BuilderWidgets::SliderStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::SliderStyleOverrideField::Track,
                             prepared.track_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::SliderStyleOverrideField::Fill,
                             prepared.fill_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::SliderStyleOverrideField::Thumb,
                             prepared.thumb_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::SliderStyleOverrideField::LabelColor,
                             prepared.label_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::SliderStyleOverrideField::LabelTypography,
                                  prepared.label_typography);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::ListStyle const& style)
    -> BuilderWidgets::ListStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::Background,
                             prepared.background_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::Border,
                             prepared.border_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::Item,
                             prepared.item_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::ItemHover,
                             prepared.item_hover_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::ItemSelected,
                             prepared.item_selected_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::Separator,
                             prepared.separator_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::ListStyleOverrideField::ItemText,
                             prepared.item_text_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::ListStyleOverrideField::ItemTypography,
                                  prepared.item_typography);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TreeStyle const& style)
    -> BuilderWidgets::TreeStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Background,
                             prepared.background_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Border,
                             prepared.border_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Row,
                             prepared.row_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::RowHover,
                             prepared.row_hover_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::RowSelected,
                             prepared.row_selected_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::RowDisabled,
                             prepared.row_disabled_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Connector,
                             prepared.connector_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Toggle,
                             prepared.toggle_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TreeStyleOverrideField::Text,
                             prepared.text_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::TreeStyleOverrideField::LabelTypography,
                                  prepared.label_typography);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TextFieldStyle const& style)
    -> BuilderWidgets::TextFieldStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Background,
                             prepared.background_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Border,
                             prepared.border_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Text,
                             prepared.text_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Placeholder,
                             prepared.placeholder_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Selection,
                             prepared.selection_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Composition,
                             prepared.composition_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextFieldStyleOverrideField::Caret,
                             prepared.caret_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::TextFieldStyleOverrideField::Typography,
                                  prepared.typography);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TextAreaStyle const& style)
    -> BuilderWidgets::TextAreaStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Background,
                             prepared.background_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Border,
                             prepared.border_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Text,
                             prepared.text_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Placeholder,
                             prepared.placeholder_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Selection,
                             prepared.selection_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Composition,
                             prepared.composition_color);
    scrub_color_if_inherited(prepared,
                             BuilderWidgets::TextAreaStyleOverrideField::Caret,
                             prepared.caret_color);
    scrub_typography_if_inherited(prepared,
                                  BuilderWidgets::TextAreaStyleOverrideField::Typography,
                                  prepared.typography);
    return prepared;
}

template <typename T>
inline auto write_value(PathSpaceBase& space,
                        std::string const& path,
                        T const& value) -> SP::Expected<void> {
    if (auto status = DeclarativeDetail::replace_single<T>(space, path, value); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

template <typename State>
inline auto write_state(PathSpace& space,
                        std::string const& root,
                        State const& state) -> SP::Expected<void> {
    return write_value(space, WidgetSpacePath(root, "/state"), state);
}

template <typename Style>
inline auto write_style(PathSpace& space,
                        std::string const& root,
                        Style const& style,
                        bool track_overrides = true) -> SP::Expected<void> {
    auto serialized = track_overrides ? prepare_style_for_serialization(style) : style;
    return write_value(space, WidgetSpacePath(root, "/meta/style"), serialized);
}

auto write_kind(PathSpace& space,
                std::string const& root,
                std::string const& kind) -> SP::Expected<void>;

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void>;

auto mark_render_dirty(PathSpaceBase& space,
                       std::string const& root) -> SP::Expected<void>;

auto reset_widget_space(PathSpace& space,
                        std::string const& root) -> SP::Expected<void>;

auto write_handler(PathSpace& space,
                   std::string const& root,
                   std::string_view event,
                   HandlerKind kind,
                   HandlerVariant handler) -> SP::Expected<void>;

auto write_fragment_handlers(PathSpace& space,
                             std::string const& root,
                             std::vector<FragmentHandler> const& handlers) -> SP::Expected<void>;

auto clear_handlers(std::string const& widget_root) -> void;

auto rebind_handlers(PathSpace& space,
                     std::string const& old_root,
                     std::string const& new_root) -> SP::Expected<void>;

auto resolve_handler(std::string const& registry_key) -> std::optional<HandlerVariant>;

auto read_handler_binding(PathSpace& space,
                          std::string const& root,
                          std::string_view event)
    -> SP::Expected<std::optional<HandlerBinding>>;

auto clear_handler_binding(PathSpace& space,
                           std::string const& root,
                           std::string_view event) -> SP::Expected<void>;

struct FragmentBuilder {
    WidgetFragment fragment;

    template <typename Fn>
    FragmentBuilder(std::string kind, Fn&& fn) {
        fragment.kind = std::move(kind);
        fragment.populate = std::forward<Fn>(fn);
    }

    FragmentBuilder& with_children(std::vector<std::pair<std::string, WidgetFragment>> children) {
        fragment.children = std::move(children);
        return *this;
    }

    FragmentBuilder& with_handler(std::string event,
                                  HandlerKind kind,
                                  HandlerVariant handler) {
        fragment.handlers.emplace_back(FragmentHandler{
            .event = std::move(event),
            .kind = kind,
            .handler = std::move(handler),
        });
        return *this;
    }

    auto build() -> WidgetFragment {
        return std::move(fragment);
    }

    template <typename Fn>
    FragmentBuilder& with_finalize(Fn&& fn) {
        fragment.finalize = std::forward<Fn>(fn);
        return *this;
    }
};

} // namespace SP::UI::Declarative::Detail
