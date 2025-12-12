#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct WidgetMailboxTopicTotals {
    std::string   topic;
    std::uint64_t total = 0;
};

struct WidgetMailboxMetrics {
    std::string                  widget_path;
    std::string                  widget_kind;
    std::vector<std::string>     subscriptions;
    std::uint64_t                events_total            = 0;
    std::uint64_t                dispatch_failures_total = 0;
    std::optional<std::uint64_t> last_dispatch_ns;
    std::optional<std::string>   last_event_kind;
    std::optional<std::uint64_t> last_event_ns;
    std::optional<std::string>   last_event_target;
    std::vector<WidgetMailboxTopicTotals> topics;
};

struct MailboxMetricsSummary {
    std::uint64_t                widgets_scanned       = 0;
    std::uint64_t                widgets_with_mailbox  = 0;
    std::uint64_t                total_events          = 0;
    std::uint64_t                total_failures        = 0;
    std::optional<std::uint64_t> last_event_ns;
    std::optional<std::string>   last_event_kind;
    std::optional<std::string>   last_event_widget;
};

struct MailboxMetricsSnapshot {
    MailboxMetricsSummary           summary;
    std::vector<WidgetMailboxMetrics> widgets;
    std::vector<std::string>        diagnostics;
};

struct MailboxMetricsOptions {
    std::string root        = "/system/applications";
    std::size_t max_widgets = 0; // 0 = no limit
};

[[nodiscard]] auto CollectMailboxMetrics(PathSpace& space,
                                         MailboxMetricsOptions const& options = {})
    -> Expected<MailboxMetricsSnapshot>;

} // namespace Inspector
} // namespace SP

