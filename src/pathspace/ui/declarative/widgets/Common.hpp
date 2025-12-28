#pragma once

#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

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

auto ensure_widget_space_root(PathSpaceBase& space, std::string const& path)
    -> SP::Expected<void>;

template <typename T>
inline auto write_value(PathSpaceBase& space,
                        std::string const& path,
                        T const& value) -> SP::Expected<void> {
    if (auto ensured = ensure_widget_space_root(space, path); !ensured) {
        return std::unexpected(ensured.error());
    }
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

auto mirror_button_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::ButtonState const& state,
                           BuilderWidgets::ButtonStyle const& style,
                           std::string const& label,
                           bool has_press_handler) -> SP::Expected<void>;

auto mirror_toggle_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::ToggleState const& state,
                           BuilderWidgets::ToggleStyle const& style,
                           bool has_toggle_handler) -> SP::Expected<void>;

auto mirror_label_capsule(PathSpace& space,
                          std::string const& root,
                          std::string const& text,
                          BuilderWidgets::TypographyStyle const& typography,
                          std::array<float, 4> const& color,
                          bool has_activate_handler) -> SP::Expected<void>;

auto mirror_slider_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::SliderState const& state,
                           BuilderWidgets::SliderStyle const& style,
                           BuilderWidgets::SliderRange const& range,
                           bool has_change_handler) -> SP::Expected<void>;

auto mirror_list_capsule(PathSpace& space,
                         std::string const& root,
                         BuilderWidgets::ListState const& state,
                         BuilderWidgets::ListStyle const& style,
                         std::vector<BuilderWidgets::ListItem> const& items,
                         bool has_child_handler) -> SP::Expected<void>;

auto mirror_tree_capsule(PathSpace& space,
                         std::string const& root,
                         BuilderWidgets::TreeState const& state,
                         BuilderWidgets::TreeStyle const& style,
                         std::vector<BuilderWidgets::TreeNode> const& nodes,
                         bool has_node_handler) -> SP::Expected<void>;

auto mirror_stack_capsule(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::StackLayoutStyle const& style,
                          std::vector<std::string> const& panel_ids,
                          std::string const& active_panel,
                          bool has_select_handler) -> SP::Expected<void>;

auto mirror_input_capsule(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::TextFieldState const& state,
                          BuilderWidgets::TextFieldStyle const& style,
                          bool has_change_handler,
                          bool has_submit_handler) -> SP::Expected<void>;

auto update_button_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::ButtonState const& state) -> SP::Expected<void>;

auto update_button_capsule_label(PathSpace& space,
                                 std::string const& root,
                                 std::string const& label) -> SP::Expected<void>;

auto update_toggle_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::ToggleState const& state) -> SP::Expected<void>;

auto update_label_capsule_text(PathSpace& space,
                               std::string const& root,
                               std::string const& text) -> SP::Expected<void>;

auto update_slider_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::SliderState const& state) -> SP::Expected<void>;

auto update_list_capsule_state(PathSpace& space,
                               std::string const& root,
                               BuilderWidgets::ListState const& state) -> SP::Expected<void>;

auto update_list_capsule_items(PathSpace& space,
                               std::string const& root,
                               std::vector<BuilderWidgets::ListItem> const& items)
    -> SP::Expected<void>;

auto update_tree_capsule_state(PathSpace& space,
                               std::string const& root,
                               BuilderWidgets::TreeState const& state) -> SP::Expected<void>;

auto update_tree_capsule_nodes(PathSpace& space,
                               std::string const& root,
                               std::vector<BuilderWidgets::TreeNode> const& nodes)
    -> SP::Expected<void>;

auto update_stack_capsule_state(PathSpace& space,
                                std::string const& root,
                                std::string const& active_panel) -> SP::Expected<void>;

auto update_input_capsule_state(PathSpace& space,
                                std::string const& root,
                                BuilderWidgets::TextFieldState const& state)
    -> SP::Expected<void>;

auto mirror_paint_surface_capsule(PathSpace& space,
                                  std::string const& root,
                                  float brush_size,
                                  std::array<float, 4> const& brush_color,
                                  std::uint32_t buffer_width,
                                  std::uint32_t buffer_height,
                                  float buffer_dpi,
                                  bool gpu_enabled) -> SP::Expected<void>;

void record_capsule_render_invocation(PathSpace& space,
                                      std::string const& widget_root,
                                      WidgetKind kind);

void record_capsule_mailbox_event(
    PathSpace& space,
    std::string const& widget_root,
    SP::UI::Runtime::Widgets::Bindings::WidgetOpKind op_kind,
    std::string_view target_id = {},
    std::uint64_t dispatch_ns = 0,
    std::uint64_t sequence = 0);

void record_capsule_mailbox_failure(PathSpace& space, std::string const& widget_root);

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
