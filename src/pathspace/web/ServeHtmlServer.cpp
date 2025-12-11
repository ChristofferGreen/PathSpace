#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/DemoSeed.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/auth/SessionStore.hpp>
#include <pathspace/web/serve_html/streaming/SseBroadcaster.hpp>
#include <pathspace/web/serve_html/routing/HtmlController.hpp>
#include <pathspace/web/serve_html/routing/AuthController.hpp>
#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>
#include <pathspace/web/serve_html/routing/OpsController.hpp>
#include <pathspace/web/serve_html/TimeUtils.hpp>

#include <pathspace/ui/DiagnosticsSummaryJson.hpp>

#include "core/Error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::ServeHtml {

static std::atomic<bool> g_should_stop{false};

namespace {

std::string make_metrics_publish_path(std::string const& apps_root) {
    if (apps_root.empty()) {
        return {};
    }
    std::string path = apps_root;
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    path.append("/io/metrics/web_server/serve_html/live");
    return path;
}

std::string make_ui_metrics_publish_path(std::string const& apps_root) {
    if (apps_root.empty()) {
        return {};
    }
    std::string path = apps_root;
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    path.append("/io/metrics/web_server/serve_html/ui_targets");
    return path;
}

auto build_ui_diagnostics_json(PathSpace& space,
                               std::function<void(std::string_view)> const& log_error)
    -> std::optional<nlohmann::json> {
    auto summaries = UI::Runtime::Diagnostics::CollectTargetDiagnostics(space);
    if (!summaries) {
        log_error(std::string{"[serve_html] Failed to read UI diagnostics: "}
                  + SP::describeError(summaries.error()));
        return std::nullopt;
    }

    auto captured_at = format_timestamp(std::chrono::system_clock::now());
    return UI::Runtime::Diagnostics::SerializeTargetDiagnostics(*summaries, captured_at);
}
} // namespace

void RequestServeHtmlStop() {
    g_should_stop.store(true);
}

void ResetServeHtmlStopFlag() {
    g_should_stop.store(false);
}

int RunServeHtmlServerWithStopFlag(ServeHtmlSpace&                     space,
                                   ServeHtmlOptions const&            options,
                                   std::atomic<bool>&                 should_stop,
                                   ServeHtmlLogHooks const&           log_hooks,
                                   std::function<void(SP::Expected<void>)> on_listen) {
    auto log_info = [&](std::string_view message) {
        if (log_hooks.info) {
            log_hooks.info(message);
            return;
        }
        std::cout << message << '\n';
    };

    auto log_error = [&](std::string_view message) {
        if (log_hooks.error) {
            log_hooks.error(message);
            return;
        }
        std::cerr << message << '\n';
    };

    std::atomic<bool> listen_reported{false};
    auto report_listen_status = [&](SP::Expected<void> status) {
        if (!on_listen) {
            return;
        }
        bool expected = false;
        if (!listen_reported.compare_exchange_strong(expected, true)) {
            return;
        }
        on_listen(std::move(status));
    };

    if (options.seed_demo) {
        Demo::SeedDemoApplication(space, options);
    }

    std::atomic<bool> demo_refresh_stop{false};
    std::thread       demo_refresh_thread;
    if (options.seed_demo && options.demo_refresh_interval_ms > 0) {
        auto interval = std::chrono::milliseconds(options.demo_refresh_interval_ms);
        if (interval <= std::chrono::milliseconds(0)) {
            interval = std::chrono::milliseconds(200);
        }
        ServeHtmlOptions demo_options = options;
        demo_refresh_thread = std::thread(
            [&space, demo_options, interval, &demo_refresh_stop, &should_stop]() mutable {
                Demo::RunDemoRefresh(space, demo_options, interval, demo_refresh_stop, should_stop);
            });
    }

    SessionConfig session_config{
        .cookie_name = options.session_cookie_name,
        .idle_timeout = std::chrono::seconds{options.session_idle_timeout_seconds},
        .absolute_timeout = std::chrono::seconds{options.session_absolute_timeout_seconds},
    };
    auto session_store_ptr = make_session_store(space, options, session_config);
    if (!session_store_ptr) {
        log_error("[serve_html] failed to initialize session store");
        report_listen_status(std::unexpected(
            SP::Error{SP::Error::Code::InvalidError, "failed to initialize session store"}));
        return EXIT_FAILURE;
    }
    SessionStore& session_store = *session_store_ptr;
    MetricsCollector metrics;

    TokenBucketRateLimiter ip_rate_limiter{options.ip_rate_limit_per_minute,
                                           options.ip_rate_limit_burst};
    TokenBucketRateLimiter session_rate_limiter{options.session_rate_limit_per_minute,
                                                options.session_rate_limit_burst};

    HttpRequestContext http_context{
        .space                = space,
        .options              = options,
        .session_store        = session_store,
        .metrics              = metrics,
        .ip_rate_limiter      = ip_rate_limiter,
        .session_rate_limiter = session_rate_limiter,
    };

    auto auth_controller = AuthController::Create(http_context);
    if (!auth_controller) {
        return EXIT_FAILURE;
    }

    auto metrics_publish_path    = make_metrics_publish_path(options.apps_root);
    auto ui_metrics_publish_path = make_ui_metrics_publish_path(options.apps_root);
    std::atomic<bool> metrics_publish_stop{false};
    std::thread        metrics_publish_thread;
    if (!metrics_publish_path.empty() || !ui_metrics_publish_path.empty()) {
        metrics_publish_thread = std::thread([&space,
                                              metrics_publish_path,
                                              ui_metrics_publish_path,
                                              &metrics,
                                              &metrics_publish_stop,
                                              &log_error,
                                              &should_stop]() {
            while (!metrics_publish_stop.load(std::memory_order_acquire)
                   && !should_stop.load(std::memory_order_acquire)) {
                if (!metrics_publish_path.empty()) {
                    auto snapshot   = metrics.capture_snapshot();
                    auto serialized = metrics.snapshot_json(snapshot).dump();
                    auto status = replace_single_value(space, metrics_publish_path, serialized);
                    if (!status) {
                        log_error(std::string{"[serve_html] Failed to publish metrics at "}
                                  + metrics_publish_path + ": "
                                  + SP::describeError(status.error()));
                    }
                }

                if (!ui_metrics_publish_path.empty()) {
                    if (auto ui_json = build_ui_diagnostics_json(space, log_error)) {
                        auto status = replace_single_value(space,
                                                           ui_metrics_publish_path,
                                                           ui_json->dump());
                        if (!status) {
                            log_error(std::string{"[serve_html] Failed to publish UI diagnostics at "}
                                      + ui_metrics_publish_path + ": "
                                      + SP::describeError(status.error()));
                        }
                    }
                }

                for (int i = 0; i < 10 && !metrics_publish_stop.load(std::memory_order_acquire)
                                     && !should_stop.load(std::memory_order_acquire);
                     ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });
    }

    httplib::Server server;

    auth_controller->register_routes(server);

    auto ops_controller = OpsController::Create(http_context);
    ops_controller->register_routes(server);

    server.Get("/", [&](httplib::Request const&, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Root, res};
        res.set_content(
            "PathSpace Web Server prototype\n\nPOST /login with {\"username\",\"password\"} to obtain a session cookie, then visit /apps/<app>/<view> to fetch DOM / CSS data.\n",
            "text/plain; charset=utf-8");
    });

    server.Get("/healthz", [&](httplib::Request const&, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Healthz, res};
        res.status = 200;
        res.set_content("ok", "text/plain; charset=utf-8");
    });



    server.Get("/metrics", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Metrics, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits(http_context, "metrics", req, res, session_cookie, nullptr)) {
            return;
        }
        if (!ensure_session(http_context, req, res, session_cookie)) {
            return;
        }
        auto snapshot = metrics.capture_snapshot();
        auto body     = metrics.render_prometheus(snapshot);
        res.set_header("Cache-Control", "no-store");
        res.set_content(body, "text/plain; version=0.0.4");
    });

    server.Get("/diagnostics/ui", [&](httplib::Request const& req, httplib::Response& res) {
        [[maybe_unused]] RequestMetricsScope request_scope{metrics, RouteMetric::Diagnostics, res};
        auto session_cookie = read_cookie_value(req, session_store.cookie_name());
        if (!apply_rate_limits(http_context, "diagnostics_ui", req, res, session_cookie, nullptr)) {
            return;
        }
        if (!ensure_session(http_context, req, res, session_cookie)) {
            return;
        }

        auto payload = build_ui_diagnostics_json(space, log_error);
        if (!payload) {
            respond_server_error(res, "failed to build UI diagnostics snapshot");
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(payload->dump(2), "application/json");
    });

    auto html_controller = HtmlController::Create(http_context);
    html_controller->register_routes(server);

    auto sse_broadcaster = SseBroadcaster::Create(http_context, should_stop);
    sse_broadcaster->register_routes(server);

    std::atomic<bool> listen_failed{false};
    std::thread server_thread([&]() {
        if (!server.listen(options.host.c_str(), options.port)) {
            if (!should_stop.load()) {
                listen_failed.store(true);
                should_stop.store(true);
                log_error(std::string{"[serve_html] Failed to bind "} + options.host + ":"
                          + std::to_string(options.port));
            }
        }
    });

    log_info(std::string{"[serve_html] Listening on http://"} + options.host + ":"
             + std::to_string(options.port));
    if (options.seed_demo) {
        std::string demo_message = std::string{"[serve_html] Try http://"} + options.host + ":"
                                   + std::to_string(options.port) + "/apps/"
                                   + std::string{Demo::kDemoApp} + "/" + std::string{Demo::kDemoView};
        log_info(demo_message);
    }

    while (!should_stop.load(std::memory_order_acquire) && !listen_failed.load(std::memory_order_acquire)) {
        if (!listen_reported.load(std::memory_order_acquire) && server.is_running()) {
            report_listen_status({});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (!listen_reported.load(std::memory_order_acquire)) {
        if (listen_failed.load(std::memory_order_acquire)) {
            report_listen_status(std::unexpected(SP::Error{
                SP::Error::Code::InvalidError,
                "failed to bind ServeHtml listener"}));
        } else if (should_stop.load(std::memory_order_acquire)) {
            report_listen_status(std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                                           "ServeHtml stop requested"}));
        }
    }

    demo_refresh_stop.store(true, std::memory_order_release);
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (demo_refresh_thread.joinable()) {
        demo_refresh_thread.join();
    }

    metrics_publish_stop.store(true, std::memory_order_release);
    if (metrics_publish_thread.joinable()) {
        metrics_publish_thread.join();
    }

    return listen_failed.load() ? EXIT_FAILURE : EXIT_SUCCESS;
}

int RunServeHtmlServer(ServeHtmlSpace& space, ServeHtmlOptions const& options) {
    return RunServeHtmlServerWithStopFlag(space, options, g_should_stop, {}, {});
}

} // namespace SP::ServeHtml
