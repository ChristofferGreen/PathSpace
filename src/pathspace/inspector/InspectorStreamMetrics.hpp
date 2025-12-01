#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace SP {

class PathSpace;

namespace Inspector {

enum class StreamDisconnectReason {
    Client,
    Server,
    Backpressure,
    Timeout,
};

struct StreamMetricsSnapshot {
    std::uint64_t active_sessions        = 0;
    std::uint64_t total_sessions         = 0;
    std::uint64_t queue_depth            = 0;
    std::uint64_t max_queue_depth        = 0;
    std::uint64_t dropped_events         = 0;
    std::uint64_t resent_snapshots       = 0;
    std::uint64_t disconnect_client      = 0;
    std::uint64_t disconnect_server      = 0;
    std::uint64_t disconnect_backpressure = 0;
    std::uint64_t disconnect_timeout     = 0;
};

class StreamMetricsRecorder {
public:
    explicit StreamMetricsRecorder(PathSpace& space,
                                   std::string root = "/inspector/metrics/stream");

    auto record_session_started() -> void;
    auto record_session_ended(StreamDisconnectReason reason) -> void;
    auto record_queue_depth(std::size_t depth) -> void;
    auto record_drop(std::size_t dropped) -> void;
    auto record_snapshot_resent() -> void;

    [[nodiscard]] auto snapshot() const -> StreamMetricsSnapshot;

private:
    auto publish_all_locked() -> void;
    auto publish_if_changed_locked(std::string const& suffix,
                                   std::uint64_t& published_value,
                                   std::uint64_t current_value) -> void;
    auto build_path(std::string const& suffix) const -> std::string;

    PathSpace&              space_;
    std::string             root_;
    mutable std::mutex      mutex_;
    StreamMetricsSnapshot   snapshot_{};
    StreamMetricsSnapshot   published_{};
    bool                    published_initialized_ = false;
};

} // namespace Inspector
} // namespace SP
