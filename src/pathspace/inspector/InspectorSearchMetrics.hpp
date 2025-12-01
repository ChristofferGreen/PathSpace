#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace SP {

class PathSpace;

namespace Inspector {

struct SearchQueryEvent {
    std::uint64_t latency_ms     = 0;
    std::uint64_t match_count    = 0;
    std::uint64_t returned_count = 0;
};

struct SearchWatchlistEvent {
    std::uint64_t live         = 0;
    std::uint64_t missing      = 0;
    std::uint64_t truncated    = 0;
    std::uint64_t out_of_scope = 0;
    std::uint64_t unknown      = 0;
};

struct SearchQueryMetricsSnapshot {
    std::uint64_t total_queries           = 0;
    std::uint64_t truncated_queries       = 0;
    std::uint64_t truncated_results_total = 0;
    std::uint64_t last_latency_ms         = 0;
    std::uint64_t average_latency_ms      = 0;
    std::uint64_t last_match_count        = 0;
    std::uint64_t last_returned_count     = 0;
    std::uint64_t last_truncated_count    = 0;
    std::uint64_t last_updated_ms         = 0;
};

struct SearchWatchMetricsSnapshot {
    std::uint64_t live           = 0;
    std::uint64_t missing        = 0;
    std::uint64_t truncated      = 0;
    std::uint64_t out_of_scope   = 0;
    std::uint64_t unknown        = 0;
    std::uint64_t total          = 0;
    std::uint64_t last_updated_ms = 0;
};

struct SearchMetricsSnapshot {
    SearchQueryMetricsSnapshot queries;
    SearchWatchMetricsSnapshot watch;
};

class SearchMetricsRecorder {
public:
    explicit SearchMetricsRecorder(PathSpace& space,
                                   std::string root = "/inspector/metrics/search");

    auto record_query(SearchQueryEvent const& event) -> void;
    auto record_watchlist(SearchWatchlistEvent const& event) -> void;

    [[nodiscard]] auto snapshot() const -> SearchMetricsSnapshot;

private:
    [[nodiscard]] static auto current_time_ms() -> std::uint64_t;

    auto publish_all_locked() -> void;
    auto publish_query_locked() -> void;
    auto publish_watch_locked() -> void;
    auto publish_if_changed_locked(std::string const& suffix,
                                   std::uint64_t& published_value,
                                   std::uint64_t current_value) -> void;
    [[nodiscard]] auto build_path(std::string const& suffix) const -> std::string;

    PathSpace&             space_;
    std::string            root_;
    mutable std::mutex     mutex_;
    SearchMetricsSnapshot  snapshot_{};
    SearchMetricsSnapshot  published_{};
    std::uint64_t          total_latency_ms_    = 0;
    bool                   published_initialized_ = false;
};

} // namespace Inspector
} // namespace SP

