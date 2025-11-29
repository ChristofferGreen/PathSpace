#pragma once

#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Declarative::Detail {

namespace BuilderWidgets = SP::UI::Runtime::Widgets;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;

using DeclarativeDetail::make_error;

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
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::ToggleStyle const& style)
    -> BuilderWidgets::ToggleStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::SliderStyle const& style)
    -> BuilderWidgets::SliderStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::ListStyle const& style)
    -> BuilderWidgets::ListStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TreeStyle const& style)
    -> BuilderWidgets::TreeStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TextFieldStyle const& style)
    -> BuilderWidgets::TextFieldStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

inline auto prepare_style_for_serialization(BuilderWidgets::TextAreaStyle const& style)
    -> BuilderWidgets::TextAreaStyle {
    auto prepared = style;
    BuilderWidgets::UpdateOverrides(prepared);
    return prepared;
}

template <typename T>
inline auto write_value(PathSpace& space,
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
    return write_value(space, root + "/state", state);
}

template <typename Style>
inline auto write_style(PathSpace& space,
                        std::string const& root,
                        Style const& style,
                        bool track_overrides = true) -> SP::Expected<void> {
    auto serialized = track_overrides ? prepare_style_for_serialization(style) : style;
    return write_value(space, root + "/meta/style", serialized);
}

auto write_kind(PathSpace& space,
                std::string const& root,
                std::string const& kind) -> SP::Expected<void>;

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void>;

auto mark_render_dirty(PathSpace& space,
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
