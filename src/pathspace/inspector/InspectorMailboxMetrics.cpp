#include "inspector/InspectorMailboxMetrics.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace SP::Inspector {
namespace {

template <typename T>
auto read_optional(PathSpace& space,
                   std::string const& path,
                   std::vector<std::string>& diagnostics) -> std::optional<T> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return *value;
    }

    switch (value.error().code) {
    case Error::Code::NoObjectFound:
    case Error::Code::NoSuchPath:
    case Error::Code::TypeMismatch:
    case Error::Code::InvalidType:
    case Error::Code::SerializationFunctionMissing:
    case Error::Code::UnserializableType:
        return std::nullopt;
    default:
        diagnostics.push_back(std::string{"read failed for "}.append(path).append(": ")
                                  .append(describeError(value.error())));
        return std::nullopt;
    }
}

auto list_children_sorted(PathSpace& space, std::string const& root) -> std::vector<std::string> {
    auto children = space.listChildren(SP::ConcretePathStringView{root});
    std::sort(children.begin(), children.end());
    return children;
}

void collect_widget(PathSpace& space,
                    std::string const& widget_root,
                    MailboxMetricsOptions const& options,
                    MailboxMetricsSnapshot& snapshot) {
    (void)options;
    snapshot.summary.widgets_scanned++;

    auto events_total = read_optional<std::uint64_t>(
        space,
        widget_root + "/capsule/mailbox/metrics/events_total",
        snapshot.diagnostics);
    if (!events_total) {
        return;
    }

    WidgetMailboxMetrics metrics{};
    metrics.widget_path  = widget_root;
    metrics.events_total = *events_total;

    metrics.dispatch_failures_total = read_optional<std::uint64_t>(
                                        space,
                                        widget_root + "/capsule/mailbox/metrics/dispatch_failures_total",
                                        snapshot.diagnostics)
                                           .value_or(0);

    metrics.last_dispatch_ns = read_optional<std::uint64_t>(
        space,
        widget_root + "/capsule/mailbox/metrics/last_dispatch_ns",
        snapshot.diagnostics);
    metrics.last_event_kind = read_optional<std::string>(
        space,
        widget_root + "/capsule/mailbox/metrics/last_event/kind",
        snapshot.diagnostics);
    metrics.last_event_ns = read_optional<std::uint64_t>(
        space,
        widget_root + "/capsule/mailbox/metrics/last_event/ns",
        snapshot.diagnostics);
    metrics.last_event_target = read_optional<std::string>(
        space,
        widget_root + "/capsule/mailbox/metrics/last_event/target",
        snapshot.diagnostics);

    metrics.subscriptions = read_optional<std::vector<std::string>>(
                                space,
                                widget_root + "/capsule/mailbox/subscriptions",
                                snapshot.diagnostics)
                                .value_or(std::vector<std::string>{});

    metrics.widget_kind = read_optional<std::string>(space,
                                                     widget_root + "/meta/kind",
                                                     snapshot.diagnostics)
                              .value_or(std::string{});

    auto topics = list_children_sorted(space, widget_root + "/capsule/mailbox/events");
    for (auto const& topic : topics) {
        auto total = read_optional<std::uint64_t>(
            space,
            widget_root + "/capsule/mailbox/events/" + topic + "/total",
            snapshot.diagnostics);
        if (!total) {
            continue;
        }
        metrics.topics.push_back(WidgetMailboxTopicTotals{
            .topic = topic,
            .total = *total,
        });
    }

    snapshot.summary.widgets_with_mailbox++;
    snapshot.summary.total_events += metrics.events_total;
    snapshot.summary.total_failures += metrics.dispatch_failures_total;

    if (metrics.last_event_ns) {
        if (!snapshot.summary.last_event_ns
            || *metrics.last_event_ns > *snapshot.summary.last_event_ns) {
            snapshot.summary.last_event_ns     = metrics.last_event_ns;
            snapshot.summary.last_event_kind   = metrics.last_event_kind;
            snapshot.summary.last_event_widget = metrics.widget_path;
        }
    }

    snapshot.widgets.push_back(std::move(metrics));
}

void collect_widget_tree(PathSpace& space,
                         std::string const& widgets_root,
                         MailboxMetricsOptions const& options,
                         MailboxMetricsSnapshot& snapshot) {
    auto widget_ids = list_children_sorted(space, widgets_root);
    for (auto const& widget_id : widget_ids) {
        if (options.max_widgets > 0 && snapshot.widgets.size() >= options.max_widgets) {
            return;
        }

        auto widget_root = widgets_root;
        if (!widget_root.empty() && widget_root.back() != '/') {
            widget_root.push_back('/');
        }
        widget_root.append(widget_id);

        collect_widget(space, widget_root, options, snapshot);

        if (options.max_widgets == 0 || snapshot.widgets.size() < options.max_widgets) {
            collect_widget_tree(space, widget_root + "/children", options, snapshot);
        }
    }
}

} // namespace

auto CollectMailboxMetrics(PathSpace& space, MailboxMetricsOptions const& options)
    -> Expected<MailboxMetricsSnapshot> {
    MailboxMetricsSnapshot snapshot{};

    auto apps_root = options.root.empty() ? std::string{"/"} : options.root;
    auto apps      = list_children_sorted(space, apps_root);
    for (auto const& app : apps) {
        auto windows_root = apps_root + "/" + app + "/windows";
        auto windows      = list_children_sorted(space, windows_root);
        for (auto const& window : windows) {
            auto views_root = windows_root + "/" + window + "/views";
            auto views      = list_children_sorted(space, views_root);
            for (auto const& view : views) {
                auto widgets_root = views_root + "/" + view + "/widgets";
                collect_widget_tree(space, widgets_root, options, snapshot);
                if (options.max_widgets > 0 && snapshot.widgets.size() >= options.max_widgets) {
                    return snapshot;
                }
            }
        }
    }

    return snapshot;
}

} // namespace SP::Inspector

