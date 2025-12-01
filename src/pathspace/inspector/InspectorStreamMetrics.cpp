#include "inspector/InspectorStreamMetrics.hpp"

#include "PathSpace.hpp"
#include "inspector/InspectorMetricUtils.hpp"

#include <algorithm>

namespace SP::Inspector {

StreamMetricsRecorder::StreamMetricsRecorder(PathSpace& space, std::string root)
    : space_(space)
    , root_(std::move(root)) {
    if (root_.empty()) {
        root_ = "/inspector/metrics/stream";
    }
    publish_all_locked();
}

auto StreamMetricsRecorder::record_session_started() -> void {
    std::lock_guard lock(mutex_);
    ++snapshot_.active_sessions;
    ++snapshot_.total_sessions;
    publish_if_changed_locked("active_sessions", published_.active_sessions, snapshot_.active_sessions);
    publish_if_changed_locked("total_sessions", published_.total_sessions, snapshot_.total_sessions);
}

auto StreamMetricsRecorder::record_session_ended(StreamDisconnectReason reason) -> void {
    std::lock_guard lock(mutex_);
    if (snapshot_.active_sessions > 0) {
        --snapshot_.active_sessions;
    }
    switch (reason) {
    case StreamDisconnectReason::Client:
        ++snapshot_.disconnect_client;
        break;
    case StreamDisconnectReason::Server:
        ++snapshot_.disconnect_server;
        break;
    case StreamDisconnectReason::Backpressure:
        ++snapshot_.disconnect_backpressure;
        break;
    case StreamDisconnectReason::Timeout:
        ++snapshot_.disconnect_timeout;
        break;
    }
    publish_if_changed_locked("active_sessions", published_.active_sessions, snapshot_.active_sessions);
    publish_if_changed_locked("disconnect/client",
                              published_.disconnect_client,
                              snapshot_.disconnect_client);
    publish_if_changed_locked("disconnect/server",
                              published_.disconnect_server,
                              snapshot_.disconnect_server);
    publish_if_changed_locked("disconnect/backpressure",
                              published_.disconnect_backpressure,
                              snapshot_.disconnect_backpressure);
    publish_if_changed_locked("disconnect/timeout",
                              published_.disconnect_timeout,
                              snapshot_.disconnect_timeout);
}

auto StreamMetricsRecorder::record_queue_depth(std::size_t depth) -> void {
    std::lock_guard lock(mutex_);
    snapshot_.queue_depth     = depth;
    snapshot_.max_queue_depth = std::max(snapshot_.max_queue_depth, static_cast<std::uint64_t>(depth));
    publish_if_changed_locked("queue_depth", published_.queue_depth, snapshot_.queue_depth);
    publish_if_changed_locked("max_queue_depth",
                              published_.max_queue_depth,
                              snapshot_.max_queue_depth);
}

auto StreamMetricsRecorder::record_drop(std::size_t dropped) -> void {
    if (dropped == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    snapshot_.dropped_events += dropped;
    publish_if_changed_locked("dropped", published_.dropped_events, snapshot_.dropped_events);
}

auto StreamMetricsRecorder::record_snapshot_resent() -> void {
    std::lock_guard lock(mutex_);
    ++snapshot_.resent_snapshots;
    publish_if_changed_locked("resent", published_.resent_snapshots, snapshot_.resent_snapshots);
}

auto StreamMetricsRecorder::snapshot() const -> StreamMetricsSnapshot {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

auto StreamMetricsRecorder::publish_all_locked() -> void {
    published_initialized_ = false;
    publish_if_changed_locked("active_sessions", published_.active_sessions, snapshot_.active_sessions);
    publish_if_changed_locked("total_sessions", published_.total_sessions, snapshot_.total_sessions);
    publish_if_changed_locked("queue_depth", published_.queue_depth, snapshot_.queue_depth);
    publish_if_changed_locked("max_queue_depth", published_.max_queue_depth, snapshot_.max_queue_depth);
    publish_if_changed_locked("dropped", published_.dropped_events, snapshot_.dropped_events);
    publish_if_changed_locked("resent", published_.resent_snapshots, snapshot_.resent_snapshots);
    publish_if_changed_locked("disconnect/client",
                              published_.disconnect_client,
                              snapshot_.disconnect_client);
    publish_if_changed_locked("disconnect/server",
                              published_.disconnect_server,
                              snapshot_.disconnect_server);
    publish_if_changed_locked("disconnect/backpressure",
                              published_.disconnect_backpressure,
                              snapshot_.disconnect_backpressure);
    publish_if_changed_locked("disconnect/timeout",
                              published_.disconnect_timeout,
                              snapshot_.disconnect_timeout);
    published_initialized_ = true;
}

auto StreamMetricsRecorder::publish_if_changed_locked(std::string const& suffix,
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

auto StreamMetricsRecorder::build_path(std::string const& suffix) const -> std::string {
    std::string path = root_;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(suffix);
    return path;
}

} // namespace SP::Inspector
