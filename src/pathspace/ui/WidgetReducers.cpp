#include "WidgetDetail.hpp"
#include <pathspace/ui/declarative/Reducers.hpp>

namespace SP::UI::Builders::Widgets::Reducers {

namespace DeclarativeReducers = SP::UI::Declarative::Reducers;

auto MakeWidgetAction(Bindings::WidgetOp const& op) -> WidgetAction {
    return DeclarativeReducers::MakeWidgetAction(op);
}

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath {
    return DeclarativeReducers::WidgetOpsQueue(widget_root);
}

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    return DeclarativeReducers::DefaultActionsQueue(widget_root);
}

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions) -> SP::Expected<std::vector<WidgetAction>> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Widgets::Reducers::ReducePending");
    return DeclarativeReducers::ReducePending(space, ops_queue, max_actions);
}

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Widgets::Reducers::PublishActions");
    return DeclarativeReducers::PublishActions(space, actions_queue, actions);
}

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions) -> SP::Expected<ProcessActionsResult> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Widgets::Reducers::ProcessPendingActions");
    return DeclarativeReducers::ProcessPendingActions(space, widget_root, max_actions);
}

} // namespace SP::UI::Builders::Widgets::Reducers
