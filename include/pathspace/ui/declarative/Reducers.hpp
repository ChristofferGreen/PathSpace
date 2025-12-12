#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <limits>
#include <span>
#include <vector>

namespace SP::UI::Declarative::Reducers {

using WidgetPath = SP::UI::Runtime::WidgetPath;
using WidgetAction = SP::UI::Runtime::Widgets::Reducers::WidgetAction;
using ProcessActionsResult = SP::UI::Runtime::Widgets::Reducers::ProcessActionsResult;
using ConcretePath = SP::UI::Runtime::ConcretePath;
using ConcretePathView = SP::UI::Runtime::ConcretePathView;
using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;

auto MakeWidgetAction(WidgetOp const& op) -> WidgetAction;

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void>;

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions = std::numeric_limits<std::size_t>::max())
    -> SP::Expected<ProcessActionsResult>;

} // namespace SP::UI::Declarative::Reducers
