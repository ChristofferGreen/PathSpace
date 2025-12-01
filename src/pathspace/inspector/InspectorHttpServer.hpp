#pragma once

#include "core/Error.hpp"
#include "inspector/InspectorAcl.hpp"
#include "inspector/InspectorSnapshot.hpp"
#include "inspector/InspectorRemoteMount.hpp"
#include "inspector/InspectorSearchMetrics.hpp"
#include "inspector/InspectorStreamMetrics.hpp"
#include "inspector/InspectorUiAssets.hpp"
#include "inspector/InspectorUsageMetrics.hpp"
#include "inspector/PaintScreenshotCard.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#include "httplib.h"

namespace SP {

class PathSpace;

namespace Inspector {

enum class InspectorWriteToggleKind {
    ToggleBool,
    SetBool,
};

struct InspectorWriteToggleAction {
    std::string               id;
    std::string               label;
    std::string               path;
    std::string               description;
    InspectorWriteToggleKind  kind           = InspectorWriteToggleKind::ToggleBool;
    bool                      default_state  = false;
};

struct InspectorWriteToggleOptions {
    bool                            enabled             = false;
    std::vector<std::string>        allowed_roles;
    std::string                     confirmation_header = "x-pathspace-inspector-write-confirmed";
    std::string                     confirmation_token  = "true";
    std::string                     audit_root          = "/diagnostics/web/inspector/audit_log";
    std::vector<InspectorWriteToggleAction> actions;
};

class InspectorHttpServer {
public:
    struct Options {
        std::string                host      = "127.0.0.1";
        int                        port      = 8765;
        InspectorSnapshotOptions   snapshot;
        PaintScreenshotCardOptions paint_card;
        std::string                ui_root;
        bool                       enable_ui = true;
        bool                       enable_test_controls = false;
        std::vector<RemoteMountOptions> remote_mounts;
        InspectorAclOptions        acl;
        InspectorWriteToggleOptions write_toggles;

        struct StreamOptions {
            std::chrono::milliseconds poll_interval{std::chrono::milliseconds(350)};
            std::chrono::milliseconds keepalive_interval{std::chrono::milliseconds(5000)};
            std::chrono::milliseconds idle_timeout{std::chrono::milliseconds(30000)};
            std::size_t              max_pending_events = 64;
            std::size_t              max_events_per_tick = 8;
        } stream;

        struct WatchlistOptions {
            std::size_t max_saved_sets     = 32;
            std::size_t max_paths_per_set  = 256;
        } watchlists;

        struct SnapshotOptions {
            std::size_t max_saved_snapshots = 20;
            std::size_t max_snapshot_bytes  = 4 * 1024 * 1024;
        } snapshots;
    };

    explicit InspectorHttpServer(PathSpace& space);
    InspectorHttpServer(PathSpace& space, Options options);
    ~InspectorHttpServer();

    InspectorHttpServer(InspectorHttpServer const&) = delete;
    auto operator=(InspectorHttpServer const&) -> InspectorHttpServer& = delete;
    InspectorHttpServer(InspectorHttpServer&&) noexcept            = delete;
    auto operator=(InspectorHttpServer&&) noexcept -> InspectorHttpServer& = delete;

    [[nodiscard]] auto start() -> Expected<void>;
    auto stop() -> void;
    auto join() -> void;

    [[nodiscard]] auto is_running() const -> bool;
    [[nodiscard]] auto port() const -> std::uint16_t;

private:
    auto configure_routes(httplib::Server& server) -> void;
    auto handle_ui_request(httplib::Response& res, std::string_view asset) -> void;
    [[nodiscard]] auto enforce_acl(httplib::Request const& req,
                                   httplib::Response& res,
                                   std::string const& requested_path,
                                   std::string_view endpoint) -> bool;
    [[nodiscard]] auto extract_role(httplib::Request const& req) const -> std::string;
    [[nodiscard]] auto extract_user(httplib::Request const& req) const -> std::string;

    PathSpace&                  space_;
    Options                     options_;
    StreamMetricsRecorder       stream_metrics_;
    SearchMetricsRecorder       search_metrics_;
    UsageMetricsRecorder        usage_metrics_;
    RemoteMountManager          remote_mounts_;
    InspectorAcl                acl_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                 server_thread_;
    std::atomic<bool>           running_{false};
    std::uint16_t               bound_port_ = 0;
    mutable std::mutex          mutex_;
};

} // namespace Inspector
} // namespace SP
