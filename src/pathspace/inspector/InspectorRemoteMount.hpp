#pragma once

#include "inspector/InspectorSnapshot.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct RemoteMountOptions {
    std::string                alias;
    std::string                host = "127.0.0.1";
    int                        port = 8765;
    bool                       use_tls = false;
    std::string                root = "/";
    InspectorSnapshotOptions   snapshot;
    std::string                access_hint;
    std::chrono::milliseconds refresh_interval{std::chrono::milliseconds(750)};
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds(4000)};
};

struct RemoteMountStatus {
    std::string                           alias;
    bool                                  connected = false;
    std::string                           message;
    std::chrono::system_clock::time_point last_update;
    std::string                           path;
    std::string                           access_hint;
    std::chrono::milliseconds             last_latency{0};
    std::chrono::milliseconds             average_latency{0};
    std::chrono::milliseconds             max_latency{0};
    std::uint64_t                         success_count        = 0;
    std::uint64_t                         error_count          = 0;
    std::uint64_t                         consecutive_errors   = 0;
    std::uint64_t                         waiter_depth         = 0;
    std::uint64_t                         max_waiter_depth     = 0;
    std::chrono::system_clock::time_point last_error_time;
    std::string                           health;
};

class RemoteMountRegistry {
public:
    RemoteMountRegistry(PathSpace* metrics_space = nullptr,
                        std::string metrics_root = "/inspector/metrics/remotes");
    explicit RemoteMountRegistry(std::vector<RemoteMountOptions> options,
                                 PathSpace*                     metrics_space = nullptr,
                                 std::string                    metrics_root = "/inspector/metrics/remotes");

    void setOptions(std::vector<RemoteMountOptions> options);
    void updateSnapshot(std::string const& alias,
                        InspectorSnapshot  snapshot,
                        std::chrono::milliseconds latency);
    void updateError(std::string const& alias,
                     std::string        message,
                     std::chrono::milliseconds latency);
    void incrementWaiters(std::string const& alias);
    void decrementWaiters(std::string const& alias);

    enum class RootKind {
        Local,
        RemoteContainer,
        RemoteMount,
        RemoteSubtree,
    };

    [[nodiscard]] auto classifyRoot(std::string const& root) const -> RootKind;
    [[nodiscard]] auto buildRemoteSnapshot(InspectorSnapshotOptions const& options) const
        -> std::optional<Expected<InspectorSnapshot>>;
    void augmentLocalSnapshot(InspectorSnapshot& snapshot) const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] auto statuses() const -> std::vector<RemoteMountStatus>;

private:
    struct MountData {
        RemoteMountOptions               options;
        std::optional<InspectorSnapshot> snapshot;
        std::chrono::system_clock::time_point last_update{};
        bool                               connected = false;
        std::string                        last_error;
        std::uint64_t                      version = 0;
        std::chrono::milliseconds          last_latency{0};
        std::chrono::milliseconds          average_latency{0};
        std::chrono::milliseconds          max_latency{0};
        std::uint64_t                      success_count = 0;
        std::uint64_t                      error_count   = 0;
        std::uint64_t                      consecutive_errors = 0;
        std::uint64_t                      waiter_depth = 0;
        std::uint64_t                      max_waiter_depth = 0;
        std::uint64_t                      total_latency_ms  = 0;
        std::chrono::system_clock::time_point last_error_time{};
    };

    [[nodiscard]] auto findMount(std::string const& alias) -> MountData*;
    [[nodiscard]] auto findMount(std::string const& alias) const -> MountData const*;

    static auto aliasRoot(std::string const& alias) -> std::string;

    [[nodiscard]] auto buildStatus(MountData const& mount) const -> RemoteMountStatus;
    void publish_metrics_locked(MountData const& mount) const;

    PathSpace*                     metrics_space_ = nullptr;
    std::string                    metrics_root_;
    std::vector<MountData>         mounts_;
    mutable std::shared_mutex      mutex_;
};

class RemoteMountManager {
public:
    explicit RemoteMountManager(std::vector<RemoteMountOptions> options = {},
                                PathSpace*                     metrics_space = nullptr,
                                std::string                    metrics_root = "/inspector/metrics/remotes");
    ~RemoteMountManager();

    void start();
    void stop();

    [[nodiscard]] bool hasMounts() const;
    [[nodiscard]] auto classifyRoot(std::string const& root) const -> RemoteMountRegistry::RootKind;
    [[nodiscard]] auto aliasForRoot(std::string const& root) const -> std::optional<std::string>;
    [[nodiscard]] auto buildRemoteSnapshot(InspectorSnapshotOptions const& options) const
        -> std::optional<Expected<InspectorSnapshot>>;
    void augmentLocalSnapshot(InspectorSnapshot& snapshot) const;
    [[nodiscard]] auto statuses() const -> std::vector<RemoteMountStatus>;
    void incrementWaiters(std::string const& alias);
    void decrementWaiters(std::string const& alias);

    void updateSnapshotForTest(std::string const& alias,
                               InspectorSnapshot  snapshot,
                               std::chrono::milliseconds latency = std::chrono::milliseconds{0});

private:
    struct MountWorker {
        RemoteMountOptions        options;
        std::thread               thread;
        std::atomic<bool>         stop{false};
    };

    void launchWorker(RemoteMountOptions const& options);
    void pollLoop(std::shared_ptr<MountWorker> worker);

    std::vector<RemoteMountOptions>             options_;
    std::vector<std::shared_ptr<MountWorker>>   workers_;
    RemoteMountRegistry                         registry_;
    std::atomic<bool>                           running_{false};
};

} // namespace Inspector
} // namespace SP
