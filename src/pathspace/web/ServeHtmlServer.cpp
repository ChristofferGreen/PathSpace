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
} // namespace

void RequestServeHtmlStop() {
    g_should_stop.store(true);
}

void ResetServeHtmlStopFlag() {
    g_should_stop.store(false);
}

int RunServeHtmlServer(ServeHtmlSpace& space, ServeHtmlOptions const& options) {
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
        demo_refresh_thread = std::thread([&space, demo_options, interval, &demo_refresh_stop]() mutable {
            Demo::RunDemoRefresh(space, demo_options, interval, demo_refresh_stop, g_should_stop);
        });
    }

    SessionConfig session_config{
        .cookie_name = options.session_cookie_name,
        .idle_timeout = std::chrono::seconds{options.session_idle_timeout_seconds},
        .absolute_timeout = std::chrono::seconds{options.session_absolute_timeout_seconds},
    };
    auto session_store_ptr = make_session_store(space, options, session_config);
    if (!session_store_ptr) {
        std::cerr << "[serve_html] failed to initialize session store\n";
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

    auto           metrics_publish_path = make_metrics_publish_path(options.apps_root);
    std::atomic<bool> metrics_publish_stop{false};
    std::thread        metrics_publish_thread;
    if (!metrics_publish_path.empty()) {
        metrics_publish_thread = std::thread([&space,
                                              metrics_publish_path,
                                              &metrics,
                                              &metrics_publish_stop]() {
            while (!metrics_publish_stop.load(std::memory_order_acquire)
                   && !g_should_stop.load(std::memory_order_acquire)) {
                auto snapshot   = metrics.capture_snapshot();
                auto serialized = metrics.snapshot_json(snapshot).dump();
                auto status = replace_single_value(space, metrics_publish_path, serialized);
                if (!status) {
                    std::cerr << "[serve_html] Failed to publish metrics at "
                              << metrics_publish_path << ": "
                              << SP::describeError(status.error()) << "\n";
                }
                for (int i = 0; i < 10 && !metrics_publish_stop.load(std::memory_order_acquire)
                                     && !g_should_stop.load(std::memory_order_acquire);
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

    auto html_controller = HtmlController::Create(http_context);
    html_controller->register_routes(server);

    auto sse_broadcaster = SseBroadcaster::Create(http_context, g_should_stop);
    sse_broadcaster->register_routes(server);

    std::atomic<bool> listen_failed{false};
    std::thread server_thread([&]() {
        if (!server.listen(options.host.c_str(), options.port)) {
            if (!g_should_stop.load()) {
                std::cerr << "[serve_html] Failed to bind " << options.host << ":" << options.port << "\n";
                listen_failed.store(true);
                g_should_stop.store(true);
            }
        }
    });

    std::cout << "[serve_html] Listening on http://" << options.host << ":" << options.port << "\n";
    if (options.seed_demo) {
        std::cout << "[serve_html] Try http://" << options.host << ":" << options.port << "/apps/"
                  << Demo::kDemoApp << "/" << Demo::kDemoView << std::endl;
    }

    while (!g_should_stop.load() && !listen_failed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

} // namespace SP::ServeHtml
