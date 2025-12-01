#include "inspector/InspectorSearchMetrics.hpp"

#include "PathSpace.hpp"
#include "inspector/InspectorMetricUtils.hpp"

#include <chrono>
#include <limits>
#include <utility>

namespace SP::Inspector {
namespace {

[[nodiscard]] auto saturating_add(std::uint64_t lhs, std::uint64_t rhs) -> std::uint64_t {
    auto const max = std::numeric_limits<std::uint64_t>::max();
    if (max - lhs < rhs) {
        return max;
    }
    return lhs + rhs;
}

} // namespace

SearchMetricsRecorder::SearchMetricsRecorder(PathSpace& space, std::string root)
    : space_(space)
    , root_(std::move(root)) {
    if (root_.empty()) {
        root_ = "/inspector/metrics/search";
    }
    publish_all_locked();
}

auto SearchMetricsRecorder::record_query(SearchQueryEvent const& event) -> void {
    std::lock_guard lock(mutex_);

    auto const truncated = event.match_count > event.returned_count
                               ? event.match_count - event.returned_count
                               : std::uint64_t{0};

    snapshot_.queries.total_queries = saturating_add(snapshot_.queries.total_queries, 1);
    snapshot_.queries.last_latency_ms = event.latency_ms;
    total_latency_ms_                = saturating_add(total_latency_ms_, event.latency_ms);
    if (snapshot_.queries.total_queries > 0) {
        snapshot_.queries.average_latency_ms =
            total_latency_ms_ / snapshot_.queries.total_queries;
    }
    snapshot_.queries.last_match_count     = event.match_count;
    snapshot_.queries.last_returned_count  = event.returned_count;
    snapshot_.queries.last_truncated_count = truncated;
    if (truncated > 0) {
        snapshot_.queries.truncated_queries =
            saturating_add(snapshot_.queries.truncated_queries, 1);
        snapshot_.queries.truncated_results_total =
            saturating_add(snapshot_.queries.truncated_results_total, truncated);
    }
    snapshot_.queries.last_updated_ms = current_time_ms();
    publish_query_locked();
}

auto SearchMetricsRecorder::record_watchlist(SearchWatchlistEvent const& event) -> void {
    std::lock_guard lock(mutex_);

    snapshot_.watch.live         = event.live;
    snapshot_.watch.missing      = event.missing;
    snapshot_.watch.truncated    = event.truncated;
    snapshot_.watch.out_of_scope = event.out_of_scope;
    snapshot_.watch.unknown      = event.unknown;
    snapshot_.watch.total        = saturating_add(
        event.live,
        saturating_add(event.missing,
                       saturating_add(event.truncated, event.unknown)));
    snapshot_.watch.last_updated_ms = current_time_ms();
    publish_watch_locked();
}

auto SearchMetricsRecorder::snapshot() const -> SearchMetricsSnapshot {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

auto SearchMetricsRecorder::current_time_ms() -> std::uint64_t {
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

auto SearchMetricsRecorder::publish_all_locked() -> void {
    published_initialized_ = false;
    publish_query_locked();
    publish_watch_locked();
    published_initialized_ = true;
}

auto SearchMetricsRecorder::publish_query_locked() -> void {
    publish_if_changed_locked("queries/total",
                              published_.queries.total_queries,
                              snapshot_.queries.total_queries);
    publish_if_changed_locked("queries/truncated_queries",
                              published_.queries.truncated_queries,
                              snapshot_.queries.truncated_queries);
    publish_if_changed_locked("queries/truncated_results_total",
                              published_.queries.truncated_results_total,
                              snapshot_.queries.truncated_results_total);
    publish_if_changed_locked("queries/last_latency_ms",
                              published_.queries.last_latency_ms,
                              snapshot_.queries.last_latency_ms);
    publish_if_changed_locked("queries/average_latency_ms",
                              published_.queries.average_latency_ms,
                              snapshot_.queries.average_latency_ms);
    publish_if_changed_locked("queries/last_match_count",
                              published_.queries.last_match_count,
                              snapshot_.queries.last_match_count);
    publish_if_changed_locked("queries/last_returned_count",
                              published_.queries.last_returned_count,
                              snapshot_.queries.last_returned_count);
    publish_if_changed_locked("queries/last_truncated_count",
                              published_.queries.last_truncated_count,
                              snapshot_.queries.last_truncated_count);
    publish_if_changed_locked("queries/last_updated_ms",
                              published_.queries.last_updated_ms,
                              snapshot_.queries.last_updated_ms);
}

auto SearchMetricsRecorder::publish_watch_locked() -> void {
    publish_if_changed_locked("watch/live", published_.watch.live, snapshot_.watch.live);
    publish_if_changed_locked("watch/missing",
                              published_.watch.missing,
                              snapshot_.watch.missing);
    publish_if_changed_locked("watch/truncated",
                              published_.watch.truncated,
                              snapshot_.watch.truncated);
    publish_if_changed_locked("watch/out_of_scope",
                              published_.watch.out_of_scope,
                              snapshot_.watch.out_of_scope);
    publish_if_changed_locked("watch/unknown",
                              published_.watch.unknown,
                              snapshot_.watch.unknown);
    publish_if_changed_locked("watch/total", published_.watch.total, snapshot_.watch.total);
    publish_if_changed_locked("watch/last_updated_ms",
                              published_.watch.last_updated_ms,
                              snapshot_.watch.last_updated_ms);
}

auto SearchMetricsRecorder::publish_if_changed_locked(std::string const& suffix,
                                                      std::uint64_t& published_value,
                                                      std::uint64_t current_value) -> void {
    if (published_initialized_ && published_value == current_value) {
        return;
    }
    published_value = current_value;
    auto path = build_path(suffix);
    if (auto replaced = Detail::ReplaceMetricValue(space_, path, current_value); !replaced) {
        (void)replaced;
    }
}

auto SearchMetricsRecorder::build_path(std::string const& suffix) const -> std::string {
    std::string path = root_;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(suffix);
    return path;
}

} // namespace SP::Inspector
