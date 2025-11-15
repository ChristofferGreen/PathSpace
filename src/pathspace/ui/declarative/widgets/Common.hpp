#pragma once

#include <pathspace/ui/declarative/Widgets.hpp>

#include "../../BuildersDetail.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Declarative::Detail {

namespace BuilderDetail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;

auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error;

auto ensure_widget_name(std::string_view name) -> SP::Expected<void>;
auto ensure_child_name(std::string_view name) -> SP::Expected<void>;

auto make_path(std::string base, std::string_view component) -> std::string;
auto mount_base(std::string_view parent, MountOptions const& options) -> std::string;

template <typename T>
inline auto write_value(PathSpace& space,
                        std::string const& path,
                        T const& value) -> SP::Expected<void> {
    if (auto status = BuilderDetail::replace_single<T>(space, path, value); !status) {
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
                        Style const& style) -> SP::Expected<void> {
    return write_value(space, root + "/meta/style", style);
}

auto write_kind(PathSpace& space,
                std::string const& root,
                std::string const& kind) -> SP::Expected<void>;

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void>;

auto write_handler(PathSpace& space,
                   std::string const& root,
                   std::string_view event,
                   HandlerKind kind,
                   HandlerVariant handler) -> SP::Expected<void>;

auto clear_handlers(std::string const& widget_root) -> void;

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

    auto build() -> WidgetFragment {
        return std::move(fragment);
    }
};

} // namespace SP::UI::Declarative::Detail
