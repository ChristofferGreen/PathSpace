#include "BuildersDetail.hpp"

namespace SP::UI::Builders::Widgets::Reducers {

using namespace Detail;

namespace {

auto to_widget_action(Bindings::WidgetOp const& op) -> WidgetAction {
    WidgetAction action{};
    action.kind = op.kind;
    action.widget_path = op.widget_path;
    action.pointer = op.pointer;
    action.analog_value = op.value;
    action.sequence = op.sequence;
    action.timestamp_ns = op.timestamp_ns;

    switch (op.kind) {
    case Bindings::WidgetOpKind::ListHover:
    case Bindings::WidgetOpKind::ListSelect:
    case Bindings::WidgetOpKind::ListActivate:
        action.discrete_index = static_cast<std::int32_t>(std::llround(op.value));
        break;
    default:
        action.discrete_index = -1;
        break;
    }

    return action;
}

} // namespace

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/actions/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions) -> SP::Expected<std::vector<WidgetAction>> {
    std::vector<WidgetAction> actions;
    if (max_actions == 0) {
        return actions;
    }

    auto queue_path = std::string(ops_queue.getPath());
    if (queue_path.empty()) {
        return actions;
    }

    for (std::size_t processed = 0; processed < max_actions; ++processed) {
        auto taken = space.take<Bindings::WidgetOp, std::string>(queue_path);
        if (taken) {
            actions.push_back(to_widget_action(*taken));
            continue;
        }

        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }

    return actions;
}

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void> {
    if (actions.empty()) {
        return {};
    }

    auto queue_path = std::string(actions_queue.getPath());
    if (queue_path.empty()) {
        return {};
    }

    for (auto const& action : actions) {
        auto inserted = space.insert(queue_path, action);
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }

    return {};
}

} // namespace SP::UI::Builders::Widgets::Reducers
