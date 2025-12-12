#include <pathspace/ui/declarative/Reducers.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <pathspace/ui/declarative/WidgetMailbox.hpp>

namespace SP::UI::Declarative::Reducers {

namespace Bindings = SP::UI::Runtime::Widgets::Bindings;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Declarative::WidgetMailboxEvent;

auto MakeWidgetAction(Bindings::WidgetOp const& op) -> WidgetAction {
    WidgetAction action{};
    action.kind = op.kind;
    action.widget_path = op.widget_path;
    action.target_id = op.target_id;
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

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    auto queue_path = WidgetSpacePath(widget_root, "/ops/actions/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}
namespace {

auto mailbox_queue_path(WidgetPath const& widget_root, std::string const& topic) -> std::string {
    auto path = WidgetSpacePath(widget_root, "/capsule/mailbox/events/");
    path.append(topic);
    path.append("/queue");
    return path;
}

auto read_mailbox_topics(PathSpace& space, WidgetPath const& widget_root)
    -> SP::Expected<std::vector<std::string>> {
    auto topics_path = WidgetSpacePath(widget_root, "/capsule/mailbox/subscriptions");
    auto topics = space.read<std::vector<std::string>, std::string>(topics_path);
    if (!topics) {
        auto const& error = topics.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return std::vector<std::string>{};
        }
        return std::unexpected(error);
    }
    return *topics;
}

} // namespace

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

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions) -> SP::Expected<ProcessActionsResult> {
    ProcessActionsResult result{};
    result.actions_queue = DefaultActionsQueue(widget_root);

    auto topics = read_mailbox_topics(space, widget_root);
    if (!topics) {
        return std::unexpected(topics.error());
    }
    result.mailbox_topics = std::move(*topics);

    if (max_actions == 0 || result.mailbox_topics.empty()) {
        return result;
    }

    for (auto const& topic : result.mailbox_topics) {
        auto queue_path = mailbox_queue_path(widget_root, topic);
        while (result.actions.size() < max_actions) {
            auto taken = space.take<WidgetMailboxEvent, std::string>(queue_path);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound
                    || error.code == SP::Error::Code::NoSuchPath) {
                    break;
                }
                return std::unexpected(error);
            }

            Bindings::WidgetOp op{};
            op.kind = taken->kind;
            op.widget_path = taken->widget_path;
            op.target_id = taken->target_id;
            op.pointer = taken->pointer;
            op.value = taken->value;
            op.sequence = taken->sequence;
            op.timestamp_ns = taken->timestamp_ns;

            result.actions.push_back(MakeWidgetAction(op));
        }

        if (result.actions.size() >= max_actions) {
            break;
        }
    }

    std::sort(result.actions.begin(), result.actions.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.sequence < rhs.sequence;
    });

    if (!result.actions.empty()) {
        auto publish = SP::UI::Declarative::Reducers::PublishActions(space,
                                                                     ConcretePathView{result.actions_queue.getPath()},
                                                                     std::span<const WidgetAction>{result.actions.data(), result.actions.size()});
        if (!publish) {
            return std::unexpected(publish.error());
        }
    }

    return result;
}

} // namespace SP::UI::Declarative::Reducers
