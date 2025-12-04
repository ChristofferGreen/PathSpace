#pragma once

#include <memory>

#include <pathspace/web/ServeHtmlIdentifier.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/Routes.hpp>
#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>

#include "httplib.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

namespace SP::ServeHtml {

namespace detail {

inline constexpr std::size_t kMaxApiPayloadBytes = 1024 * 1024;

inline void handle_api_ops_request(HttpRequestContext& ctx,
                                   httplib::Request const& req,
                                   httplib::Response&      res) {
    using json = nlohmann::json;

    [[maybe_unused]] RequestMetricsScope request_scope{ctx.metrics, RouteMetric::ApiOps, res};

    auto session_cookie = read_cookie_value(req, ctx.session_store.cookie_name());
    if (!apply_rate_limits(ctx, "api_ops", req, res, session_cookie, nullptr)) {
        return;
    }
    if (!ensure_session(ctx, req, res, session_cookie)) {
        return;
    }
    if (req.matches.size() < 2) {
        respond_bad_request(res, "invalid op route");
        return;
    }

    std::string op = req.matches[1];
    if (!is_identifier(op)) {
        respond_bad_request(res, "invalid op identifier");
        return;
    }

    auto content_type = req.get_header_value("Content-Type");
    if (content_type.find("application/json") == std::string::npos) {
        respond_unsupported_media_type(res);
        return;
    }
    if (req.body.empty()) {
        respond_bad_request(res, "body must not be empty");
        return;
    }
    if (req.body.size() > kMaxApiPayloadBytes) {
        respond_payload_too_large(res);
        return;
    }

    auto payload = json::parse(req.body, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        respond_bad_request(res, "body must be a JSON object");
        return;
    }

    auto app_it    = payload.find("app");
    auto schema_it = payload.find("schema");
    if (app_it == payload.end() || !app_it->is_string() || schema_it == payload.end()
        || !schema_it->is_string()) {
        respond_bad_request(res, "app and schema fields are required");
        return;
    }

    std::string app    = app_it->get<std::string>();
    std::string schema = schema_it->get<std::string>();
    if (!is_identifier(app) || schema.empty()) {
        respond_bad_request(res, "invalid app or schema");
        return;
    }

    std::string serialized = payload.dump();
    if (serialized.size() > kMaxApiPayloadBytes) {
        respond_payload_too_large(res);
        return;
    }

    auto queue_path    = make_ops_queue_path(ctx.options, app, op);
    auto enqueue_start = std::chrono::steady_clock::now();
    auto inserted      = ctx.space.insert(queue_path, serialized);
    auto enqueue_end   = std::chrono::steady_clock::now();
    ctx.metrics.record_render_trigger_latency(
        std::chrono::duration_cast<std::chrono::microseconds>(enqueue_end - enqueue_start));
    if (!inserted.errors.empty()) {
        respond_server_error(res, "failed to enqueue op: " + SP::describeError(inserted.errors.front()));
        return;
    }

    res.set_header("X-PathSpace-App", app);
    res.set_header("X-PathSpace-Op", op);
    res.set_header("X-PathSpace-Queue", queue_path);

    write_json_response(res,
                        json{{"status", "enqueued"},
                             {"app", app},
                             {"op", op},
                             {"schema", schema},
                             {"queue", queue_path},
                             {"bytes", serialized.size()}},
                        202,
                        true);
}

} // namespace detail

class OpsController {
public:
    static auto Create(HttpRequestContext& ctx) -> std::unique_ptr<OpsController> {
        return std::unique_ptr<OpsController>(new OpsController(ctx));
    }

    void register_routes(httplib::Server& server) {
        server.Post(R"(/api/ops/([A-Za-z0-9_\-\.]+))",
                    [this](httplib::Request const& req, httplib::Response& res) {
                        detail::handle_api_ops_request(ctx_, req, res);
                    });
    }

    ~OpsController() = default;

private:
    explicit OpsController(HttpRequestContext& ctx)
        : ctx_(ctx) {}

    HttpRequestContext& ctx_;
};

} // namespace SP::ServeHtml

