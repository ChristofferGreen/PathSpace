#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
using SP::UI::Builders::WidgetPath;

namespace Tree {

auto Fragment(Args args) -> WidgetFragment {
    return WidgetDetail::FragmentBuilder{"tree",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           BuilderWidgets::TreeState state{};
                                           if (auto status = WidgetDetail::write_state(ctx.space,
                                                                                ctx.root,
                                                                                state);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_style(ctx.space,
                                                                                ctx.root,
                                                                                args.style);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/meta/nodes",
                                                                                args.nodes);
                                               !status) {
                                               return status;
                                           }
                                           if (args.on_node_event) {
                                               HandlerVariant handler = TreeNodeHandler{*args.on_node_event};
                                               if (auto status = WidgetDetail::write_handler(ctx.space,
                                                                                      ctx.root,
                                                                                      "node_event",
                                                                                      HandlerKind::TreeNode,
                                                                                      std::move(handler));
                                                   !status) {
                                                   return status;
                                               }
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Tree);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetNodes(PathSpace& space,
              WidgetPath const& widget,
              std::vector<TreeNode> nodes) -> SP::Expected<void> {
    if (auto status = WidgetDetail::write_value(space,
                                          widget.getPath() + "/meta/nodes",
                                          std::move(nodes));
        !status) {
        return status;
    }
    return WidgetDetail::write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Tree

} // namespace SP::UI::Declarative
