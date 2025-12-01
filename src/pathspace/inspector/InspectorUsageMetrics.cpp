#include "inspector/InspectorUsageMetrics.hpp"

#include "PathSpace.hpp"
#include "inspector/InspectorMetricUtils.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace SP::Inspector {

namespace {

[[nodiscard]] auto make_panel_suffix(std::string_view panel_id,
                                     std::string_view metric) -> std::string {
    std::string path{"panels/"};
    path.append(panel_id.data(), panel_id.size());
    if (!metric.empty()) {
        path.push_back('/');
        path.append(metric.data(), metric.size());
    }
    return path;
}

} // namespace

UsageMetricsRecorder::UsageMetricsRecorder(PathSpace& space, std::string root)
    : space_(space)
    , root_(std::move(root)) {
    if (root_.empty()) {
        root_ = "/diagnostics/web/inspector/usage";
    }
    publish_all_locked();
}

auto UsageMetricsRecorder::record(PanelUsageEvent const& event) -> void {
    record(std::vector<PanelUsageEvent>{event});
}

auto UsageMetricsRecorder::record(std::vector<PanelUsageEvent> const& events) -> void {
    if (events.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);

    bool                    updated     = false;
    std::vector<std::string> changed_ids;
    changed_ids.reserve(events.size());

    for (auto const& event : events) {
        if (event.panel_id.empty()) {
            continue;
        }
        if (event.dwell_ms == 0 && event.entries == 0) {
            continue;
        }

        auto const timestamp = event.timestamp_ms != 0 ? event.timestamp_ms : current_time_ms();

        auto& metrics = snapshot_.panels[event.panel_id];
        metrics.dwell_ms_total = saturating_add(metrics.dwell_ms_total, event.dwell_ms);
        metrics.entries_total  = saturating_add(metrics.entries_total, event.entries);
        metrics.last_dwell_ms  = event.dwell_ms;
        metrics.last_updated_ms = timestamp;

        snapshot_.total_dwell_ms = saturating_add(snapshot_.total_dwell_ms, event.dwell_ms);
        snapshot_.total_entries  = saturating_add(snapshot_.total_entries, event.entries);
        snapshot_.last_updated_ms = timestamp;

        changed_ids.push_back(event.panel_id);
        updated = true;
    }

    if (!updated) {
        return;
    }

    std::sort(changed_ids.begin(), changed_ids.end());
    changed_ids.erase(std::unique(changed_ids.begin(), changed_ids.end()), changed_ids.end());

    publish_totals_locked();
    for (auto const& id : changed_ids) {
        auto it = snapshot_.panels.find(id);
        if (it == snapshot_.panels.end()) {
            continue;
        }
        publish_panel_locked(id, it->second);
    }
}

auto UsageMetricsRecorder::snapshot() const -> UsageMetricsSnapshot {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

auto UsageMetricsRecorder::publish_all_locked() -> void {
    published_initialized_ = false;
    publish_totals_locked();
    for (auto const& [id, metrics] : snapshot_.panels) {
        publish_panel_locked(id, metrics);
    }
    published_initialized_ = true;
}

auto UsageMetricsRecorder::publish_totals_locked() -> void {
    publish_if_changed_locked("total/dwell_ms",
                              published_.total_dwell_ms,
                              snapshot_.total_dwell_ms);
    publish_if_changed_locked("total/entries",
                              published_.total_entries,
                              snapshot_.total_entries);
    publish_if_changed_locked("last_updated_ms",
                              published_.last_updated_ms,
                              snapshot_.last_updated_ms);
}

auto UsageMetricsRecorder::publish_panel_locked(std::string const& panel_id,
                                                PanelUsageMetrics const& metrics) -> void {
    auto& published_metrics = published_.panels[panel_id];

    auto publish_panel_metric = [&](std::string_view suffix,
                                    std::uint64_t PanelUsageMetrics::*member,
                                    std::uint64_t value) {
        if (published_initialized_ && published_metrics.*member == value) {
            return;
        }
        published_metrics.*member = value;
        auto path = build_panel_path(panel_id, suffix);
        if (auto replaced = Detail::ReplaceMetricValue(space_, path, value); !replaced) {
            (void)replaced;
        }
    };

    publish_panel_metric("dwell_ms", &PanelUsageMetrics::dwell_ms_total, metrics.dwell_ms_total);
    publish_panel_metric("entries", &PanelUsageMetrics::entries_total, metrics.entries_total);
    publish_panel_metric("last_dwell_ms", &PanelUsageMetrics::last_dwell_ms, metrics.last_dwell_ms);
    publish_panel_metric("last_updated_ms",
                        &PanelUsageMetrics::last_updated_ms,
                        metrics.last_updated_ms);
}

auto UsageMetricsRecorder::publish_if_changed_locked(std::string const& suffix,
                                                     std::uint64_t& published_value,
                                                     std::uint64_t current_value) -> void {
    if (published_initialized_ && published_value == current_value) {
        return;
    }
    published_value = current_value;
    auto path       = build_path(suffix);
    if (auto replaced = Detail::ReplaceMetricValue(space_, path, current_value); !replaced) {
        (void)replaced;
    }
}

auto UsageMetricsRecorder::build_path(std::string const& suffix) const -> std::string {
    std::string path = root_;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(suffix);
    return path;
}

auto UsageMetricsRecorder::build_panel_path(std::string_view panel_id, std::string_view suffix) const
    -> std::string {
    auto relative = make_panel_suffix(panel_id, suffix);
    return build_path(relative);
}

auto UsageMetricsRecorder::saturating_add(std::uint64_t lhs, std::uint64_t rhs) -> std::uint64_t {
    auto const max = std::numeric_limits<std::uint64_t>::max();
    if (max - lhs < rhs) {
        return max;
    }
    return lhs + rhs;
}

auto UsageMetricsRecorder::current_time_ms() -> std::uint64_t {
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace SP::Inspector
