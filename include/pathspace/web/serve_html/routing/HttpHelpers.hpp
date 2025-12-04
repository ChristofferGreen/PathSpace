#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace httplib {
class Request;
class Response;
}

namespace SP::ServeHtml {

class ServeHtmlSpace;
struct ServeHtmlOptions;
class SessionStore;
class MetricsCollector;

class TokenBucketRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucketRateLimiter(std::int64_t per_minute, std::int64_t burst);

    auto allow(std::string_view key, Clock::time_point now = Clock::now()) -> bool;

private:
    struct Bucket {
        double            tokens{0.0};
        Clock::time_point last_refill{};
        Clock::time_point last_used{};
    };

    auto enabled() const -> bool;
    void prune_locked(Clock::time_point now);

    double                                  capacity_{0.0};
    double                                  refill_per_second_{0.0};
    std::unordered_map<std::string, Bucket> buckets_;
    std::size_t                             operations_since_prune_{0};
    std::mutex                              mutex_;
};

struct HttpRequestContext {
    ServeHtmlSpace&          space;
    ServeHtmlOptions const&  options;
    SessionStore&            session_store;
    MetricsCollector&        metrics;
    TokenBucketRateLimiter&  ip_rate_limiter;
    TokenBucketRateLimiter&  session_rate_limiter;
};

auto read_cookie_value(httplib::Request const& req, std::string const& name)
    -> std::optional<std::string>;

auto get_client_address(httplib::Request const& req) -> std::string;

auto abbreviate_token(std::string_view token) -> std::string;

void write_json_response(httplib::Response& res,
                         nlohmann::json const& payload,
                         int                  status,
                         bool                 no_store = false);

void respond_unauthorized(httplib::Response& res);
void respond_bad_request(httplib::Response& res, std::string_view message);
void respond_server_error(httplib::Response& res, std::string_view message);
void respond_payload_too_large(httplib::Response& res);
void respond_unsupported_media_type(httplib::Response& res);
void respond_rate_limited(httplib::Response& res);

void apply_session_cookie(HttpRequestContext& ctx, httplib::Response& res, std::string const& value);
void expire_session_cookie(HttpRequestContext& ctx, httplib::Response& res);

auto ensure_session(HttpRequestContext&         ctx,
                    httplib::Request const&    req,
                    httplib::Response&         res,
                    std::optional<std::string> cookie_hint = std::nullopt) -> bool;

auto apply_rate_limits(HttpRequestContext&         ctx,
                       std::string_view           route_name,
                       httplib::Request const&    req,
                       httplib::Response&         res,
                       std::optional<std::string> session_cookie,
                       std::string const*         app_root) -> bool;

} // namespace SP::ServeHtml
