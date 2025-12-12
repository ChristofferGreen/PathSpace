#include "WidgetDetail.hpp"
#include <pathspace/ui/declarative/Reducers.hpp>

namespace SP::UI::Runtime::Widgets::Reducers {

namespace DeclarativeReducers = SP::UI::Declarative::Reducers;

auto MakeWidgetAction(Bindings::WidgetOp const& op) -> WidgetAction {
    return DeclarativeReducers::MakeWidgetAction(op);
}

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    return DeclarativeReducers::DefaultActionsQueue(widget_root);
}

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void> {
    return DeclarativeReducers::PublishActions(space, actions_queue, actions);
}

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions) -> SP::Expected<ProcessActionsResult> {
    return DeclarativeReducers::ProcessPendingActions(space, widget_root, max_actions);
}

} // namespace SP::UI::Runtime::Widgets::Reducers
