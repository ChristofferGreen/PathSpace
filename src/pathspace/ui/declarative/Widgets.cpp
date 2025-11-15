#include <pathspace/ui/declarative/Widgets.hpp>

#include "widgets/Common.hpp"

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
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
    WidgetDetail::clear_handlers(widget.getPath());
    return {};
}

auto Move(PathSpace&,
          SP::UI::Builders::WidgetPath const&,
          SP::App::ConcretePathView,
          std::string_view,
          MountOptions const&) -> SP::Expected<SP::UI::Builders::WidgetPath> {
    return std::unexpected(WidgetDetail::make_error("Declarative widget move is not implemented yet",
                                                    SP::Error::Code::NotSupported));
}

} // namespace SP::UI::Declarative
