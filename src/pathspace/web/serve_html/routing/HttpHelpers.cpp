#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>

#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/TimeUtils.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/auth/SessionStore.hpp>

#include "core/Error.hpp"

#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace SP::ServeHtml {

namespace {

std::string trim_view(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

std::string make_security_log_queue_path(std::string const& base_root) {
    if (base_root.empty()) {
        return {};
    }
    std::string path = base_root;
    path.append("/io/log/security/request_rejections/queue");
    return path;
}

void log_security_rejection(ServeHtmlSpace&           space,
                            std::string const&       base_root,
                            std::string_view         scope,
                            std::string_view         route,
                            std::string_view         remote_addr,
                            std::string_view         session_hint) {
    auto log_path = make_security_log_queue_path(base_root);
    if (log_path.empty()) {
        return;
    }

    nlohmann::json entry{{"ts", format_timestamp(std::chrono::system_clock::now())},
                         {"scope", scope},
                         {"route", route},
                         {"remote_addr", remote_addr}};
    if (!session_hint.empty()) {
        entry["session"] = session_hint;
    }

    auto result = space.insert(log_path, entry.dump());
    if (!result.errors.empty()) {
        std::cerr << "[serve_html] Failed to append security log at " << log_path << ": "
                  << SP::describeError(result.errors.front()) << "\n";
    }
}

auto read_cookie_header(httplib::Request const& req) -> std::string {
    auto value = req.get_header_value("Cookie");
    if (!value.empty()) {
        return value;
    }
    auto it = req.headers.find("cookie");
    if (it != req.headers.end()) {
        return it->second;
    }
    return {};
}

std::string build_cookie_header(std::string const&                         name,
                                std::string const&                         value,
                                std::optional<std::chrono::seconds> const& max_age,
                                bool                                        http_only = true) {
    std::ostringstream header;
    header << name << '=' << value << "; Path=/; SameSite=Lax";
    if (http_only) {
        header << "; HttpOnly";
    }
    if (max_age.has_value()) {
        header << "; Max-Age=" << max_age->count();
    }
    return header.str();
}

} // namespace

TokenBucketRateLimiter::TokenBucketRateLimiter(std::int64_t per_minute, std::int64_t burst)
    : capacity_{static_cast<double>(std::max<std::int64_t>(burst, 0))}
    , refill_per_second_{per_minute <= 0 ? 0.0 : static_cast<double>(per_minute) / 60.0} {}

auto TokenBucketRateLimiter::allow(std::string_view key, Clock::time_point now) -> bool {
    if (!enabled()) {
        return true;
    }

    std::string normalized_key = key.empty() ? std::string{"<unknown>"} : std::string{key};

    std::lock_guard const lock{mutex_};
    auto& bucket = buckets_[normalized_key];

    if (bucket.last_refill.time_since_epoch().count() == 0) {
        bucket.tokens      = capacity_;
        bucket.last_refill = now;
    } else if (now > bucket.last_refill) {
        auto const delta = std::chrono::duration<double>(now - bucket.last_refill).count();
        bucket.tokens = std::min(capacity_, bucket.tokens + delta * refill_per_second_);
        bucket.last_refill = now;
    }

    bucket.last_used = now;
    if (bucket.tokens < 1.0) {
        prune_locked(now);
        return false;
    }

    bucket.tokens -= 1.0;
    prune_locked(now);
    return true;
}

auto TokenBucketRateLimiter::enabled() const -> bool {
    return capacity_ > 0.0 && refill_per_second_ > 0.0;
}

void TokenBucketRateLimiter::prune_locked(Clock::time_point now) {
    if (++operations_since_prune_ < 512) {
        return;
    }
    operations_since_prune_ = 0;
    auto const max_idle = std::chrono::minutes{10};
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if ((now - it->second.last_used) > max_idle || buckets_.size() > 4096) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

auto read_cookie_value(httplib::Request const& req, std::string const& name)
    -> std::optional<std::string> {
    auto header = read_cookie_header(req);
    if (header.empty()) {
        return std::nullopt;
    }
    std::string_view cookie_view{header};
    while (!cookie_view.empty()) {
        auto semicolon = cookie_view.find(';');
        std::string_view segment;
        if (semicolon == std::string_view::npos) {
            segment = cookie_view;
            cookie_view = {};
        } else {
            segment = cookie_view.substr(0, semicolon);
            cookie_view.remove_prefix(semicolon + 1);
        }
        auto equals = segment.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        auto key = trim_view(segment.substr(0, equals));
        if (key == name) {
            auto value = trim_view(segment.substr(equals + 1));
            return std::string{value};
        }
    }
    return std::nullopt;
}

auto get_client_address(httplib::Request const& req) -> std::string {
    if (!req.remote_addr.empty()) {
        return req.remote_addr;
    }
    if (!req.get_header_value("X-Forwarded-For").empty()) {
        return req.get_header_value("X-Forwarded-For");
    }
    if (!req.get_header_value("x-forwarded-for").empty()) {
        return req.get_header_value("x-forwarded-for");
    }
    return "<unknown>";
}

auto abbreviate_token(std::string_view value) -> std::string {
    if (value.size() <= 8) {
        return std::string{value};
    }
    std::string shortened;
    shortened.reserve(10);
    shortened.append(value.substr(0, 4));
    shortened.append("…");
    shortened.append(value.substr(value.size() - 3));
    return shortened;
}

void write_json_response(httplib::Response& res,
                         nlohmann::json const& payload,
                         int                  status,
                         bool                 no_store) {
    res.status = status;
    res.set_content(payload.dump(), "application/json; charset=utf-8");
    if (no_store) {
        res.set_header("Cache-Control", "no-store");
    }
}

void respond_unauthorized(httplib::Response& res) {
    write_json_response(res,
                        nlohmann::json{{"error", "unauthorized"},
                                       {"message", "Authentication required"}},
                        401,
                        true);
}

void respond_bad_request(httplib::Response& res, std::string_view message) {
    write_json_response(res,
                        nlohmann::json{{"error", "bad_request"},
                                       {"message", message}},
                        400,
                        true);
}

void respond_server_error(httplib::Response& res, std::string_view message) {
    write_json_response(res,
                        nlohmann::json{{"error", "internal"},
                                       {"message", message}},
                        500);
}

void respond_payload_too_large(httplib::Response& res) {
    write_json_response(res,
                        nlohmann::json{{"error", "payload_too_large"},
                                       {"message", "Request body exceeds 1 MiB limit"}},
                        413,
                        true);
}

void respond_unsupported_media_type(httplib::Response& res) {
    write_json_response(res,
                        nlohmann::json{{"error", "unsupported_media_type"},
                                       {"message", "Expected Content-Type: application/json"}},
                        415,
                        true);
}

void respond_rate_limited(httplib::Response& res) {
    write_json_response(res,
                        nlohmann::json{{"error", "rate_limited"},
                                       {"message", "Too many requests"}},
                        429,
                        true);
}

void apply_session_cookie(HttpRequestContext& ctx, httplib::Response& res, std::string const& value) {
    auto age = ctx.session_store.cookie_max_age();
    std::optional<std::chrono::seconds> max_age;
    if (age.count() > 0) {
        max_age = age;
    }
    res.set_header("Set-Cookie",
                   build_cookie_header(ctx.session_store.cookie_name(), value, max_age));
}

void expire_session_cookie(HttpRequestContext& ctx, httplib::Response& res) {
    res.set_header("Set-Cookie",
                   build_cookie_header(ctx.session_store.cookie_name(),
                                       "",
                                       std::optional<std::chrono::seconds>{std::chrono::seconds{0}}));
}

auto ensure_session(HttpRequestContext&         ctx,
                    httplib::Request const&    req,
                    httplib::Response&         res,
                    std::optional<std::string> cookie_hint) -> bool {
    std::optional<std::string> cookie = cookie_hint;
    if (!cookie.has_value()) {
        cookie = read_cookie_value(req, ctx.session_store.cookie_name());
    }
    if (!cookie || cookie->empty()) {
        if (ctx.options.auth_optional) {
            return true;
        }
        respond_unauthorized(res);
        return false;
    }

    auto username = ctx.session_store.validate(*cookie);
    if (!username) {
        expire_session_cookie(ctx, res);
        if (ctx.options.auth_optional) {
            return true;
        }
        respond_unauthorized(res);
        return false;
    }

    (void)username;
    return true;
}

auto apply_rate_limits(HttpRequestContext&         ctx,
                       std::string_view           route_name,
                       httplib::Request const&    req,
                       httplib::Response&         res,
                       std::optional<std::string> session_cookie,
                       std::string const*         app_root) -> bool {
    auto const now        = TokenBucketRateLimiter::Clock::now();
    auto       remote_addr = get_client_address(req);
    auto       session_hint = session_cookie && !session_cookie->empty()
                                  ? abbreviate_token(*session_cookie)
                                  : std::string{};
    std::string const& log_root = (app_root != nullptr && !app_root->empty())
                                       ? *app_root
                                       : ctx.options.apps_root;

    if (!ctx.ip_rate_limiter.allow(remote_addr, now)) {
        respond_rate_limited(res);
        ctx.metrics.record_rate_limit("ip", route_name);
        log_security_rejection(ctx.space, log_root, "ip", route_name, remote_addr, session_hint);
        return false;
    }

    if (session_cookie && !session_cookie->empty()
        && !ctx.session_rate_limiter.allow(*session_cookie, now)) {
        respond_rate_limited(res);
        ctx.metrics.record_rate_limit("session", route_name);
        log_security_rejection(ctx.space,
                               log_root,
                               "session",
                               route_name,
                               remote_addr,
                               session_hint);
        return false;
    }

    return true;
}

} // namespace SP::ServeHtml
