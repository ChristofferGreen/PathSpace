#include "Common.hpp"

#include <utility>
#include <vector>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Builders::WidgetPath;

namespace Stack {

auto Fragment(Args args) -> WidgetFragment {
    std::vector<std::string> panel_ids;
    panel_ids.reserve(args.panels.size());
    for (auto const& panel : args.panels) {
        panel_ids.push_back(panel.id);
    }
    std::vector<std::pair<std::string, WidgetFragment>> child_fragments;
    child_fragments.reserve(args.panels.size());
    for (auto& panel : args.panels) {
        child_fragments.emplace_back(panel.id, std::move(panel.fragment));
    }

    auto on_select = std::move(args.on_select);
    auto builder = WidgetDetail::FragmentBuilder{"stack",
                                   [panel_ids = std::move(panel_ids),
                                    active_panel = std::move(args.active_panel)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/active_panel",
                                                                                active_panel);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Stack);
                                               !status) {
                                               return status;
                                           }
                                           for (auto const& panel_id : panel_ids) {
                                               if (auto status = WidgetDetail::ensure_child_name(panel_id); !status) {
                                                   return status;
                                               }
                                               auto panel_root = ctx.root + "/panels/" + panel_id;
                                               auto target = ctx.root + "/children/" + panel_id;
                                               if (auto write = WidgetDetail::write_value(ctx.space,
                                                                                   panel_root + "/target",
                                                                                   target);
                                                   !write) {
                                                   return write;
                                               }
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .with_children(std::move(child_fragments));

    if (on_select) {
        HandlerVariant handler = StackPanelHandler{std::move(*on_select)};
        builder.with_handler("panel_select", HandlerKind::StackPanel, std::move(handler));
    }

    return builder.build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetActivePanel(PathSpace& space,
                    WidgetPath const& widget,
                    std::string_view panel_id) -> SP::Expected<void> {
    return WidgetDetail::write_value(space,
                                     widget.getPath() + "/state/active_panel",
                                     std::string(panel_id));
}

} // namespace Stack

} // namespace SP::UI::Declarative
