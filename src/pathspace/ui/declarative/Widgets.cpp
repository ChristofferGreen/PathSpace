
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

using SP::UI::Builders::WidgetPath;

auto MountFragment(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   WidgetFragment const& fragment,
                   MountOptions const& options) -> SP::Expected<SP::UI::Builders::WidgetPath> {
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

    return SP::UI::Builders::WidgetPath{root};
}

auto Widgets::Mount(PathSpace& space,
                    SP::App::ConcretePathView parent,
                    std::string_view name,
                    WidgetFragment const& fragment,
                    MountOptions const& options) -> SP::Expected<SP::UI::Builders::WidgetPath> {
    return MountFragment(space, parent, name, fragment, options);
}

auto Remove(PathSpace& space, SP::UI::Builders::WidgetPath const& widget) -> SP::Expected<void> {
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
          SP::UI::Builders::WidgetPath const& widget,
          SP::App::ConcretePathView new_parent,
          std::string_view new_name,
          MountOptions const& options) -> SP::Expected<SP::UI::Builders::WidgetPath> {
    if (auto status = WidgetDetail::ensure_widget_name(new_name); !status) {
        return std::unexpected(status.error());
    }

    auto base = WidgetDetail::mount_base(new_parent.getPath(), options);
    auto destination_root = WidgetDetail::make_path(base, new_name);
    auto const& source_root = widget.getPath();
    if (destination_root == source_root) {
        return SP::UI::Builders::WidgetPath{destination_root};
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

    return SP::UI::Builders::WidgetPath{destination_root};
}

} // namespace SP::UI::Declarative
