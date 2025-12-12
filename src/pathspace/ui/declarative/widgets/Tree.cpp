#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::WidgetPath;

namespace Tree {

auto Fragment(Args args) -> WidgetFragment {
    auto on_node_event = std::move(args.on_node_event);
    bool has_node_handler = on_node_event.has_value();
    auto builder = WidgetDetail::FragmentBuilder{"tree",
                                   [args = std::move(args), has_node_handler](FragmentContext const& ctx)
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
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/meta/nodes"),
                                                                                args.nodes);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Tree);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::mirror_tree_capsule(ctx.space,
                                                                                       ctx.root,
                                                                                       state,
                                                                                       args.style,
                                                                                       args.nodes,
                                                                                       has_node_handler);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        ;

    if (on_node_event) {
        HandlerVariant handler = TreeNodeHandler{std::move(*on_node_event)};
        builder.with_handler("node_event", HandlerKind::TreeNode, std::move(handler));
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

auto SetNodes(PathSpace& space,
              WidgetPath const& widget,
              std::vector<TreeNode> nodes) -> SP::Expected<void> {
    if (auto status = WidgetDetail::write_value(space,
                                          WidgetSpacePath(widget.getPath(), "/meta/nodes"),
                                          nodes);
        !status) {
        return status;
    }
    if (auto status = WidgetDetail::update_tree_capsule_nodes(space, widget.getPath(), nodes);
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace Tree

} // namespace SP::UI::Declarative
