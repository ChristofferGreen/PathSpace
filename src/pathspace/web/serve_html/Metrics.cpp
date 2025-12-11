#include <pathspace/web/serve_html/Metrics.hpp>

#include <pathspace/web/serve_html/TimeUtils.hpp>

#include "httplib.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace SP::ServeHtml {

using json = nlohmann::json;

namespace {

constexpr std::array<std::pair<RouteMetric, char const*>, static_cast<std::size_t>(RouteMetric::Count)>
    kRouteMetricNames{{
        {RouteMetric::Root, "root"},
        {RouteMetric::Healthz, "healthz"},
        {RouteMetric::Login, "login"},
        {RouteMetric::LoginGoogle, "login_google"},
        {RouteMetric::LoginGoogleCallback, "login_google_callback"},
        {RouteMetric::Logout, "logout"},
        {RouteMetric::Session, "session"},
        {RouteMetric::Apps, "apps"},
        {RouteMetric::Assets, "assets"},
        {RouteMetric::ApiOps, "api_ops"},
        {RouteMetric::Events, "events"},
        {RouteMetric::Metrics, "metrics"},
        {RouteMetric::Diagnostics, "diagnostics_ui"},
    }};

constexpr std::size_t kRouteCount = static_cast<std::size_t>(RouteMetric::Count);

} // namespace

void MetricsCollector::Histogram::observe(std::chrono::microseconds value) {
    auto const micros = static_cast<std::uint64_t>(value.count());
    sum_micros_.fetch_add(micros, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    double const millis = static_cast<double>(micros) / 1000.0;
    for (std::size_t i = 0; i < kLatencyBucketsMs.size(); ++i) {
        if (millis <= kLatencyBucketsMs[i]) {
            buckets_[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    buckets_.back().fetch_add(1, std::memory_order_relaxed);
}

auto MetricsCollector::Histogram::snapshot() const -> HistogramSnapshot {
    HistogramSnapshot snapshot{};
    for (std::size_t i = 0; i < kLatencyBucketsMs.size(); ++i) {
        snapshot.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
    }
    snapshot.count      = count_.load(std::memory_order_relaxed);
    snapshot.sum_micros = sum_micros_.load(std::memory_order_relaxed);
    return snapshot;
}

auto MetricsCollector::Histogram::bucket_boundaries()
    -> std::array<double, HistogramSnapshot::kBucketCount> const& {
    return kLatencyBucketsMs;
}

auto MetricsCollector::RateLimitKey::operator<(RateLimitKey const& other) const -> bool {
    if (scope != other.scope) {
        return scope < other.scope;
    }
    return route < other.route;
}

void MetricsCollector::record_request(RouteMetric route,
                                      int         status,
                                      std::chrono::microseconds latency) {
    auto const index = static_cast<std::size_t>(route);
    if (index >= routes_.size()) {
        return;
    }
    auto& counters = routes_[index];
    counters.latency.observe(latency);
    counters.total.fetch_add(1, std::memory_order_relaxed);
    int effective_status = status == 0 ? 200 : status;
    if (effective_status >= 400) {
        counters.errors.fetch_add(1, std::memory_order_relaxed);
    }
}

void MetricsCollector::record_auth_failure() {
    auth_failures_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::record_rate_limit(std::string_view scope, std::string_view route) {
    std::lock_guard const lock{rate_limit_mutex_};
    RateLimitKey          key{std::string{scope}, std::string{route}};
    rate_limit_counts_[std::move(key)] += 1;
}

void MetricsCollector::record_asset_cache_hit() {
    asset_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::record_asset_cache_miss() {
    asset_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::record_sse_connection_open() {
    sse_connections_current_.fetch_add(1, std::memory_order_relaxed);
    sse_connections_total_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::record_sse_connection_close() {
    sse_connections_current_.fetch_sub(1, std::memory_order_relaxed);
}

void MetricsCollector::record_sse_event(std::string_view event_type) {
    std::lock_guard const lock{sse_event_mutex_};
    sse_event_counts_[std::string{event_type}] += 1;
}

void MetricsCollector::record_render_trigger_latency(std::chrono::microseconds latency) {
    render_trigger_latency_.observe(latency);
}

auto MetricsCollector::capture_snapshot() const -> MetricsSnapshot {
    MetricsSnapshot snapshot;
    snapshot.captured_at = std::chrono::system_clock::now();
    for (std::size_t i = 0; i < routes_.size(); ++i) {
        snapshot.routes[i].latency = routes_[i].latency.snapshot();
        snapshot.routes[i].total = routes_[i].total.load(std::memory_order_relaxed);
        snapshot.routes[i].errors = routes_[i].errors.load(std::memory_order_relaxed);
    }
    snapshot.sse_connections_current = sse_connections_current_.load(std::memory_order_relaxed);
    snapshot.sse_connections_total = sse_connections_total_.load(std::memory_order_relaxed);
    snapshot.asset_cache_hits = asset_cache_hits_.load(std::memory_order_relaxed);
    snapshot.asset_cache_misses = asset_cache_misses_.load(std::memory_order_relaxed);
    snapshot.auth_failures = auth_failures_.load(std::memory_order_relaxed);
    snapshot.render_trigger_latency = render_trigger_latency_.snapshot();

    {
        std::lock_guard const lock{rate_limit_mutex_};
        snapshot.rate_limits.reserve(rate_limit_counts_.size());
        for (auto const& entry : rate_limit_counts_) {
            snapshot.rate_limits.push_back(
                MetricsSnapshot::RateLimitEntry{entry.first.scope, entry.first.route, entry.second});
        }
    }

    {
        std::lock_guard const lock{sse_event_mutex_};
        snapshot.sse_events.reserve(sse_event_counts_.size());
        for (auto const& entry : sse_event_counts_) {
            snapshot.sse_events.push_back(MetricsSnapshot::SseEventEntry{entry.first, entry.second});
        }
    }

    return snapshot;
}

auto MetricsCollector::render_prometheus() const -> std::string {
    auto snapshot = capture_snapshot();
    return render_prometheus(snapshot);
}

auto MetricsCollector::render_prometheus(MetricsSnapshot const& snapshot) const -> std::string {
    metrics_scrapes_.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;

    out << "# HELP pathspace_serve_html_request_duration_seconds Request latency histogram\n";
    out << "# TYPE pathspace_serve_html_request_duration_seconds histogram\n";
    auto const& buckets = Histogram::bucket_boundaries();
    for (std::size_t i = 0; i < snapshot.routes.size(); ++i) {
        auto const& route_stats = snapshot.routes[i];
        auto const* name        = kRouteMetricNames[i].second;
        std::uint64_t cumulative = 0;
        for (std::size_t b = 0; b < buckets.size(); ++b) {
            cumulative += route_stats.latency.buckets[b];
            auto boundary = buckets[b];
            out << "pathspace_serve_html_request_duration_seconds_bucket{route=\"" << name
                << "\",le=\"" << (std::isinf(boundary) ? std::string{"+Inf"}
                                                           : std::to_string(boundary / 1000.0))
                << "\"} " << cumulative << "\n";
        }
        double sum_seconds = route_stats.latency.sum_micros / 1'000'000.0;
        out << "pathspace_serve_html_request_duration_seconds_sum{route=\"" << name
            << "\"} " << sum_seconds << "\n";
        out << "pathspace_serve_html_request_duration_seconds_count{route=\"" << name
            << "\"} " << route_stats.latency.count << "\n";
    }

    out << "# HELP pathspace_serve_html_requests_total Total HTTP requests\n";
    out << "# TYPE pathspace_serve_html_requests_total counter\n";
    out << "# HELP pathspace_serve_html_request_errors_total HTTP requests returning >=400\n";
    out << "# TYPE pathspace_serve_html_request_errors_total counter\n";
    for (std::size_t i = 0; i < snapshot.routes.size(); ++i) {
        auto const* name = kRouteMetricNames[i].second;
        out << "pathspace_serve_html_requests_total{route=\"" << name << "\"} "
            << snapshot.routes[i].total << "\n";
        out << "pathspace_serve_html_request_errors_total{route=\"" << name << "\"} "
            << snapshot.routes[i].errors << "\n";
    }

    out << "# HELP pathspace_serve_html_sse_connections Current SSE connections\n";
    out << "# TYPE pathspace_serve_html_sse_connections gauge\n";
    out << "pathspace_serve_html_sse_connections "
        << snapshot.sse_connections_current << "\n";
    out << "# HELP pathspace_serve_html_sse_connections_total Total SSE connections opened\n";
    out << "# TYPE pathspace_serve_html_sse_connections_total counter\n";
    out << "pathspace_serve_html_sse_connections_total "
        << snapshot.sse_connections_total << "\n";

    {
        if (!snapshot.sse_events.empty()) {
            out << "# HELP pathspace_serve_html_sse_events_total SSE events emitted by type\n";
            out << "# TYPE pathspace_serve_html_sse_events_total counter\n";
            for (auto const& entry : snapshot.sse_events) {
                out << "pathspace_serve_html_sse_events_total{type=\"" << entry.type << "\"} "
                    << entry.count << "\n";
            }
        }
    }

    out << "# HELP pathspace_serve_html_asset_cache_hits_total Asset cache hits (304)\n";
    out << "# TYPE pathspace_serve_html_asset_cache_hits_total counter\n";
    out << "pathspace_serve_html_asset_cache_hits_total "
        << snapshot.asset_cache_hits << "\n";
    out << "# HELP pathspace_serve_html_asset_cache_misses_total Asset cache misses\n";
    out << "# TYPE pathspace_serve_html_asset_cache_misses_total counter\n";
    out << "pathspace_serve_html_asset_cache_misses_total "
        << snapshot.asset_cache_misses << "\n";

    out << "# HELP pathspace_serve_html_auth_failures_total Authentication failures\n";
    out << "# TYPE pathspace_serve_html_auth_failures_total counter\n";
    out << "pathspace_serve_html_auth_failures_total "
        << snapshot.auth_failures << "\n";

    out << "# HELP pathspace_serve_html_render_trigger_latency_seconds Ops enqueue latency\n";
    out << "# TYPE pathspace_serve_html_render_trigger_latency_seconds histogram\n";
    auto const& render_snapshot = snapshot.render_trigger_latency;
    std::uint64_t cumulative = 0;
    auto const& latency_buckets = Histogram::bucket_boundaries();
    for (std::size_t b = 0; b < latency_buckets.size(); ++b) {
        cumulative += render_snapshot.buckets[b];
        auto boundary = latency_buckets[b];
        out << "pathspace_serve_html_render_trigger_latency_seconds_bucket{le=\""
            << (std::isinf(boundary) ? std::string{"+Inf"}
                                      : std::to_string(boundary / 1000.0))
            << "\"} " << cumulative << "\n";
    }
    out << "pathspace_serve_html_render_trigger_latency_seconds_sum "
        << (render_snapshot.sum_micros / 1'000'000.0) << "\n";
    out << "pathspace_serve_html_render_trigger_latency_seconds_count " << render_snapshot.count
        << "\n";

    {
        if (!snapshot.rate_limits.empty()) {
            out << "# HELP pathspace_serve_html_rate_limit_rejections_total Rate-limited requests\n";
            out << "# TYPE pathspace_serve_html_rate_limit_rejections_total counter\n";
            for (auto const& entry : snapshot.rate_limits) {
                out << "pathspace_serve_html_rate_limit_rejections_total{scope=\""
                    << entry.scope << "\",route=\"" << entry.route << "\"} "
                    << entry.count << "\n";
            }
        }
    }

    out << "# HELP pathspace_serve_html_metrics_scrapes_total Metrics scrapes\n";
    out << "# TYPE pathspace_serve_html_metrics_scrapes_total counter\n";
    out << "pathspace_serve_html_metrics_scrapes_total "
        << metrics_scrapes_.load(std::memory_order_relaxed) << "\n";

    return out.str();
}

auto MetricsCollector::snapshot_json() const -> json {
    auto snapshot = capture_snapshot();
    return snapshot_json(snapshot);
}

auto MetricsCollector::snapshot_json(MetricsSnapshot const& snapshot) const -> json {
    json payload;
    payload["captured_at"] = format_timestamp(snapshot.captured_at);

    json request_stats;
    for (std::size_t i = 0; i < snapshot.routes.size(); ++i) {
        auto const* name   = kRouteMetricNames[i].second;
        auto const& stats  = snapshot.routes[i];
        double       avg_ms = stats.latency.count == 0
                                  ? 0.0
                                  : static_cast<double>(stats.latency.sum_micros) / 1000.0
                                        / static_cast<double>(stats.latency.count);
        request_stats[name] = json{{"total", stats.total}, {"errors", stats.errors}, {"avg_ms", avg_ms}};
    }
    payload["requests"] = std::move(request_stats);

    payload["sse"] = json{{"connections_current", snapshot.sse_connections_current},
                           {"connections_total", snapshot.sse_connections_total}};

    payload["assets"] = json{{"cache_hits", snapshot.asset_cache_hits},
                               {"cache_misses", snapshot.asset_cache_misses}};

    payload["auth_failures"] = snapshot.auth_failures;

    double render_avg_ms = snapshot.render_trigger_latency.count == 0
                               ? 0.0
                               : static_cast<double>(snapshot.render_trigger_latency.sum_micros) / 1000.0
                                     / static_cast<double>(snapshot.render_trigger_latency.count);
    payload["render_triggers"] = json{{"count", snapshot.render_trigger_latency.count},
                                       {"avg_ms", render_avg_ms}};

    json rate_limits = json::array();
    for (auto const& entry : snapshot.rate_limits) {
        rate_limits.push_back(json{{"scope", entry.scope},
                                   {"route", entry.route},
                                   {"count", entry.count}});
    }
    payload["rate_limits"] = std::move(rate_limits);

    json sse_events = json::array();
    for (auto const& entry : snapshot.sse_events) {
        sse_events.push_back(json{{"type", entry.type}, {"count", entry.count}});
    }
    payload["sse_events"] = std::move(sse_events);

    return payload;
}

RequestMetricsScope::RequestMetricsScope(MetricsCollector& metrics,
                                         RouteMetric       route,
                                         httplib::Response& res)
    : metrics_{metrics}
    , route_{route}
    , response_{res}
    , start_{std::chrono::steady_clock::now()} {}

RequestMetricsScope::~RequestMetricsScope() {
    auto duration = std::chrono::steady_clock::now() - start_;
    metrics_.record_request(route_,
                            response_.status,
                            std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

} // namespace SP::ServeHtml
