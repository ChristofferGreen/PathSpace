#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct PanelUsageEvent {
    std::string   panel_id;
    std::uint64_t dwell_ms     = 0;
    std::uint64_t entries      = 0;
    std::uint64_t timestamp_ms = 0;
};

struct PanelUsageMetrics {
    std::uint64_t dwell_ms_total = 0;
    std::uint64_t entries_total  = 0;
    std::uint64_t last_dwell_ms  = 0;
    std::uint64_t last_updated_ms = 0;
};

struct UsageMetricsSnapshot {
    std::uint64_t total_dwell_ms = 0;
    std::uint64_t total_entries  = 0;
    std::uint64_t last_updated_ms = 0;
    std::unordered_map<std::string, PanelUsageMetrics> panels;
};

class UsageMetricsRecorder {
public:
    explicit UsageMetricsRecorder(PathSpace& space,
                                  std::string root = "/diagnostics/web/inspector/usage");

    auto record(PanelUsageEvent const& event) -> void;
    auto record(std::vector<PanelUsageEvent> const& events) -> void;

    [[nodiscard]] auto snapshot() const -> UsageMetricsSnapshot;

private:
    auto publish_all_locked() -> void;
    auto publish_totals_locked() -> void;
    auto publish_panel_locked(std::string const& panel_id,
                              PanelUsageMetrics const& metrics) -> void;
    auto publish_if_changed_locked(std::string const& suffix,
                                   std::uint64_t& published_value,
                                   std::uint64_t current_value) -> void;
    auto build_path(std::string const& suffix) const -> std::string;
    auto build_panel_path(std::string_view panel_id, std::string_view suffix) const
        -> std::string;

    static auto saturating_add(std::uint64_t lhs, std::uint64_t rhs) -> std::uint64_t;
    static auto current_time_ms() -> std::uint64_t;

    PathSpace&           space_;
    std::string          root_;
    mutable std::mutex   mutex_;
    UsageMetricsSnapshot snapshot_{};
    UsageMetricsSnapshot published_{};
    bool                 published_initialized_ = false;
};

} // namespace Inspector
} // namespace SP

