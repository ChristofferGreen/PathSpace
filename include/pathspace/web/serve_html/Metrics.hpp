#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib {
class Response;
}

namespace SP::ServeHtml {

enum class RouteMetric : std::size_t {
    Root = 0,
    Healthz,
    Login,
    LoginGoogle,
    LoginGoogleCallback,
    Logout,
    Session,
    Apps,
    Assets,
    ApiOps,
    Events,
    Metrics,
    Diagnostics,
    Count,
};

class MetricsCollector {
public:
    struct HistogramSnapshot {
        static constexpr std::size_t kBucketCount = 10;
        std::array<std::uint64_t, kBucketCount>   buckets{};
        std::uint64_t                             count{0};
        std::uint64_t                             sum_micros{0};
    };

    struct MetricsSnapshot {
        struct RouteCounters {
            HistogramSnapshot latency;
            std::uint64_t     total{0};
            std::uint64_t     errors{0};
        };

        struct RateLimitEntry {
            std::string scope;
            std::string route;
            std::uint64_t count{0};
        };

        struct SseEventEntry {
            std::string type;
            std::uint64_t count{0};
        };

        std::chrono::system_clock::time_point                               captured_at{};
        std::array<RouteCounters, static_cast<std::size_t>(RouteMetric::Count)> routes{};
        std::int64_t                                                      sse_connections_current{0};
        std::uint64_t                                                     sse_connections_total{0};
        std::uint64_t                                                     asset_cache_hits{0};
        std::uint64_t                                                     asset_cache_misses{0};
        std::uint64_t                                                     auth_failures{0};
        HistogramSnapshot                                                 render_trigger_latency;
        std::vector<RateLimitEntry>                                       rate_limits;
        std::vector<SseEventEntry>                                        sse_events;
    };

    void record_request(RouteMetric route,
                        int         status,
                        std::chrono::microseconds latency);

    void record_auth_failure();
    void record_rate_limit(std::string_view scope, std::string_view route);
    void record_asset_cache_hit();
    void record_asset_cache_miss();
    void record_sse_connection_open();
    void record_sse_connection_close();
    void record_sse_event(std::string_view event_type);
    void record_render_trigger_latency(std::chrono::microseconds latency);

    auto capture_snapshot() const -> MetricsSnapshot;
    auto render_prometheus() const -> std::string;
    auto render_prometheus(MetricsSnapshot const& snapshot) const -> std::string;
    auto snapshot_json() const -> nlohmann::json;
    auto snapshot_json(MetricsSnapshot const& snapshot) const -> nlohmann::json;

private:
    class Histogram {
    public:
        void observe(std::chrono::microseconds value);
        auto snapshot() const -> HistogramSnapshot;
        static auto bucket_boundaries() -> std::array<double, HistogramSnapshot::kBucketCount> const&;

    private:
        static constexpr std::array<double, HistogramSnapshot::kBucketCount> kLatencyBucketsMs{
            1.0,   5.0,    20.0,   50.0,   100.0,
            250.0, 500.0,  1000.0, 2500.0, std::numeric_limits<double>::infinity()};

        std::array<std::atomic<std::uint64_t>, HistogramSnapshot::kBucketCount> buckets_{};
        std::atomic<std::uint64_t>                                               count_{0};
        std::atomic<std::uint64_t>                                               sum_micros_{0};
    };

    struct RateLimitKey {
        std::string scope;
        std::string route;

        auto operator<(RateLimitKey const& other) const -> bool;
    };

    struct RouteCounters {
        Histogram                  latency;
        std::atomic<std::uint64_t> total{0};
        std::atomic<std::uint64_t> errors{0};
    };

    std::array<RouteCounters, static_cast<std::size_t>(RouteMetric::Count)> routes_{};
    std::atomic<std::int64_t>                                            sse_connections_current_{0};
    std::atomic<std::uint64_t>                                           sse_connections_total_{0};
    std::atomic<std::uint64_t>                                           asset_cache_hits_{0};
    std::atomic<std::uint64_t>                                           asset_cache_misses_{0};
    std::atomic<std::uint64_t>                                           auth_failures_{0};
    mutable std::atomic<std::uint64_t>                                   metrics_scrapes_{0};
    Histogram                                                            render_trigger_latency_;

    mutable std::mutex                    rate_limit_mutex_;
    std::map<RateLimitKey, std::uint64_t> rate_limit_counts_;
    mutable std::mutex                    sse_event_mutex_;
    std::map<std::string, std::uint64_t>  sse_event_counts_;
};

class RequestMetricsScope {
public:
    RequestMetricsScope(MetricsCollector& metrics, RouteMetric route, httplib::Response& res);
    ~RequestMetricsScope();

private:
    MetricsCollector&                     metrics_;
    RouteMetric                           route_;
    httplib::Response&                    response_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace SP::ServeHtml
