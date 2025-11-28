
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include "widgets/Common.hpp"

#include <algorithm>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;

namespace {

auto parent_path(std::string const& path) -> std::string {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return std::string{"/"};
    }
    return path.substr(0, slash);
}

auto mark_if_widget(PathSpace& space, std::string const& path) -> void {
    auto kind = space.read<std::string, std::string>(path + "/meta/kind");
    if (!kind) {
        return;
    }
    (void)WidgetDetail::mark_render_dirty(space, path);
}

} // namespace

using SP::UI::Runtime::WidgetPath;

auto MountFragment(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   WidgetFragment const& fragment,
                   MountOptions const& options) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    if (auto status = WidgetDetail::ensure_widget_name(name); !status) {
        return std::unexpected(status.error());
    }

    auto base = WidgetDetail::mount_base(parent.getPath(), options);
    auto root = WidgetDetail::make_path(base, name);

    if (auto status = WidgetDetail::write_kind(space, root, fragment.kind); !status) {
        return std::unexpected(status.error());
    }

    FragmentContext ctx{space, root};
    if (fragment.populate) {
        if (auto populated = fragment.populate(ctx); !populated) {
            return std::unexpected(populated.error());
        }
    }

    if (auto handlers = WidgetDetail::write_fragment_handlers(space, root, fragment.handlers);
        !handlers) {
        return std::unexpected(handlers.error());
    }

    for (auto const& child : fragment.children) {
        if (auto mounted = MountFragment(space,
                                         SP::App::ConcretePathView{root},
                                         child.first,
                                         child.second,
                                         MountOptions{.policy = MountPolicy::WidgetChildren});
            !mounted) {
            return std::unexpected(mounted.error());
        }
    }

    if (fragment.finalize) {
        if (auto status = fragment.finalize(ctx); !status) {
            return std::unexpected(status.error());
        }
    }

    return SP::UI::Runtime::WidgetPath{root};
}

auto Widgets::Mount(PathSpace& space,
                    SP::App::ConcretePathView parent,
                    std::string_view name,
                    WidgetFragment const& fragment,
                    MountOptions const& options) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    return MountFragment(space, parent, name, fragment, options);
}

auto Remove(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget) -> SP::Expected<void> {
    if (auto status = WidgetDetail::write_value(space,
                                                widget.getPath() + "/state/removed",
                                                true);
        !status) {
        return status;
    }
    (void)WidgetDetail::mark_render_dirty(space, widget.getPath());
    WidgetDetail::clear_handlers(widget.getPath());
    return {};
}

auto Move(PathSpace& space,
          SP::UI::Runtime::WidgetPath const& widget,
          SP::App::ConcretePathView new_parent,
          std::string_view new_name,
          MountOptions const& options) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    if (auto status = WidgetDetail::ensure_widget_name(new_name); !status) {
        return std::unexpected(status.error());
    }

    auto base = WidgetDetail::mount_base(new_parent.getPath(), options);
    auto destination_root = WidgetDetail::make_path(base, new_name);
    auto const& source_root = widget.getPath();
    if (destination_root == source_root) {
        return SP::UI::Runtime::WidgetPath{destination_root};
    }

    auto destination_parent = parent_path(destination_root);
    auto final_component = std::string(new_name);
    auto siblings = space.listChildren(SP::ConcretePathStringView{destination_parent});
    if (std::find(siblings.begin(), siblings.end(), final_component) != siblings.end()) {
        return std::unexpected(WidgetDetail::make_error("destination widget already exists",
                                                       SP::Error::Code::InvalidPath));
    }

    auto relocated = space.relocateSubtree(source_root, destination_root);
    if (!relocated) {
        return std::unexpected(relocated.error());
    }

    if (auto rebind = WidgetDetail::rebind_handlers(space, source_root, destination_root); !rebind) {
        return std::unexpected(rebind.error());
    }

    (void)WidgetDetail::mark_render_dirty(space, destination_root);
    mark_if_widget(space, parent_path(source_root));
    mark_if_widget(space, destination_parent);

    return SP::UI::Runtime::WidgetPath{destination_root};
}

namespace Handlers {

auto Read(PathSpace& space,
          WidgetPath const& widget,
          std::string_view event) -> SP::Expected<std::optional<HandlerVariant>> {
    auto binding = WidgetDetail::read_handler_binding(space, widget.getPath(), event);
    if (!binding) {
        return std::unexpected(binding.error());
    }
    if (!binding->has_value()) {
        return std::optional<HandlerVariant>{};
    }
    auto handler = WidgetDetail::resolve_handler(binding->value().registry_key);
    if (!handler) {
        return std::optional<HandlerVariant>{};
    }
    return std::optional<HandlerVariant>{*handler};
}

auto Replace(PathSpace& space,
             WidgetPath const& widget,
             std::string_view event,
             HandlerKind kind,
             HandlerVariant handler) -> SP::Expected<HandlerOverrideToken> {
    HandlerOverrideToken token{
        .widget_path = widget.getPath(),
        .event = std::string(event),
        .kind = kind,
    };

    auto current_binding = WidgetDetail::read_handler_binding(space, widget.getPath(), event);
    if (!current_binding) {
        return std::unexpected(current_binding.error());
    }
    if (current_binding->has_value()) {
        token.had_previous = true;
        auto resolved = WidgetDetail::resolve_handler(current_binding->value().registry_key);
        if (resolved) {
            token.previous_handler = *resolved;
        }
    }

    if (auto status = WidgetDetail::write_handler(space,
                                                  widget.getPath(),
                                                  event,
                                                  kind,
                                                  std::move(handler));
        !status) {
        return std::unexpected(status.error());
    }

    return token;
}

auto Wrap(PathSpace& space,
          WidgetPath const& widget,
          std::string_view event,
          HandlerKind kind,
          HandlerTransformer const& transformer) -> SP::Expected<HandlerOverrideToken> {
    auto existing = Read(space, widget, event);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    HandlerVariant const empty{};
    auto const& current = existing->has_value() ? existing->value() : empty;
    auto next = transformer(current);
    return Replace(space, widget, event, kind, std::move(next));
}

auto Restore(PathSpace& space, HandlerOverrideToken const& token) -> SP::Expected<void> {
    WidgetPath widget{token.widget_path};
    if (token.previous_handler.has_value()) {
        if (auto status = WidgetDetail::write_handler(space,
                                                      widget.getPath(),
                                                      token.event,
                                                      token.kind,
                                                      *token.previous_handler);
            !status) {
            return std::unexpected(status.error());
        }
        return {};
    }
    return WidgetDetail::clear_handler_binding(space, widget.getPath(), token.event);
}

} // namespace Handlers

} // namespace SP::UI::Declarative
