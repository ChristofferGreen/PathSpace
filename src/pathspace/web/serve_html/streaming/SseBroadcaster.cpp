#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <pathspace/web/serve_html/streaming/SseBroadcaster.hpp>

#include <pathspace/web/ServeHtmlIdentifier.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/Routes.hpp>
#include <pathspace/web/serve_html/TimeUtils.hpp>
#include <pathspace/web/serve_html/auth/SessionStore.hpp>
#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>

#include "core/Error.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

namespace SP::ServeHtml {

namespace {

using json = nlohmann::json;
namespace UiRuntime = SP::UI::Runtime;

std::string_view severity_to_string(UiRuntime::Diagnostics::PathSpaceError::Severity severity) {
    using Severity = UiRuntime::Diagnostics::PathSpaceError::Severity;
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Recoverable:
        return "recoverable";
    case Severity::Fatal:
        return "fatal";
    }
    return "info";
}

bool has_active_diagnostic(UiRuntime::Diagnostics::PathSpaceError const& error) {
    using Severity = UiRuntime::Diagnostics::PathSpaceError::Severity;
    if (error.code != 0) {
        return true;
    }
    if (error.severity != Severity::Info) {
        return true;
    }
    if (!error.message.empty() || !error.detail.empty()) {
        return true;
    }
    return false;
}

bool diagnostic_equals(UiRuntime::Diagnostics::PathSpaceError const& lhs,
                       UiRuntime::Diagnostics::PathSpaceError const& rhs) {
    return lhs.code == rhs.code && lhs.severity == rhs.severity && lhs.message == rhs.message
           && lhs.detail == rhs.detail && lhs.path == rhs.path && lhs.revision == rhs.revision
           && lhs.timestamp_ns == rhs.timestamp_ns;
}

void write_sse_event(httplib::DataSink& sink,
                     std::string_view   event_name,
                     std::string const& payload,
                     std::string const* event_id = nullptr) {
    std::string block;
    block.reserve(payload.size() + 64);
    if (event_id != nullptr && !event_id->empty()) {
        block.append("id: ");
        block.append(*event_id);
        block.append("\n");
    }
    block.append("event: ");
    block.append(event_name);
    block.append("\n");
    std::size_t start = 0U;
    while (start < payload.size()) {
        auto end = payload.find('\n', start);
        auto len = (end == std::string::npos ? payload.size() : end) - start;
        block.append("data: ");
        block.append(payload.data() + start, len);
        block.append("\n");
        if (end == std::string::npos) {
            start = payload.size();
        } else {
            start = end + 1;
        }
    }
    block.append("\n");
    sink.write(block.data(), block.size());
}

void write_sse_comment(httplib::DataSink& sink, std::string_view comment) {
    std::string block = ": ";
    block.append(comment.data(), comment.size());
    block.append("\n\n");
    sink.write(block.data(), block.size());
}

void write_sse_retry(httplib::DataSink& sink, int milliseconds) {
    std::string block = "retry: ";
    block.append(std::to_string(milliseconds));
    block.append("\n\n");
    sink.write(block.data(), block.size());
}

auto parse_last_event_id(httplib::Request const& req) -> std::optional<std::uint64_t> {
    auto header = req.get_header_value("Last-Event-ID");
    if (header.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    auto          result = std::from_chars(header.data(), header.data() + header.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

} // namespace

HtmlEventStreamSession::HtmlEventStreamSession(ServeHtmlSpace&   space,
                                               std::string      html_base,
                                               std::string      common_base,
                                               std::string      diagnostics_path,
                                               std::string      watch_glob,
                                               std::uint64_t    resume_revision,
                                               MetricsCollector* metrics,
                                               std::atomic<bool>& should_stop)
    : space_(space)
    , context_(space.shared_context())
    , html_base_(std::move(html_base))
    , common_base_(std::move(common_base))
    , diagnostics_path_(std::move(diagnostics_path))
    , watch_glob_(std::move(watch_glob))
    , last_revision_sent_(resume_revision)
    , metrics_(metrics)
    , should_stop_(should_stop) {}

auto HtmlEventStreamSession::pump(httplib::DataSink& sink) -> bool {
    if (cancelled_.load(std::memory_order_acquire) || should_stop_.load(std::memory_order_acquire)) {
        return false;
    }

    if (!started_) {
        started_ = true;
        write_sse_retry(sink, 2000);
        auto snapshot = read_snapshot();
        if (!snapshot) {
            emit_error_event(sink, SP::describeError(snapshot.error()));
        } else {
            if (deliver_updates(*snapshot, sink, true)) {
                last_keepalive_ = std::chrono::steady_clock::now();
            }
        }
        wait_for_change();
        return true;
    }

    wait_for_change();

    if (cancelled_.load(std::memory_order_acquire) || should_stop_.load(std::memory_order_acquire)) {
        return false;
    }

    auto snapshot = read_snapshot();
    if (!snapshot) {
        emit_error_event(sink, SP::describeError(snapshot.error()));
        return true;
    }

    bool emitted = deliver_updates(*snapshot, sink, false);
    auto now      = std::chrono::steady_clock::now();
    if (emitted) {
        last_keepalive_ = now;
    } else if (now - last_keepalive_ >= kKeepAliveInterval) {
        emit_keepalive(sink);
        last_keepalive_ = now;
    }
    return true;
}

void HtmlEventStreamSession::cancel() {
    cancelled_.store(true, std::memory_order_release);
}

void HtmlEventStreamSession::finalize(bool) {
    cancelled_.store(true, std::memory_order_release);
}

auto HtmlEventStreamSession::read_snapshot() -> SP::Expected<StreamSnapshot> {
    StreamSnapshot snapshot{};
    auto           frame_path = common_base_ + "/frameIndex";
    auto           frame_val  = read_optional_value<std::uint64_t>(space_, frame_path);
    if (!frame_val) {
        return std::unexpected(frame_val.error());
    }
    snapshot.frame_index = std::move(*frame_val);

    auto revision_path = html_base_ + "/revision";
    auto revision_val  = read_optional_value<std::uint64_t>(space_, revision_path);
    if (!revision_val) {
        return std::unexpected(revision_val.error());
    }
    snapshot.revision = std::move(*revision_val);

    auto diag_val = read_optional_value<UiRuntime::Diagnostics::PathSpaceError>(space_, diagnostics_path_);
    if (!diag_val) {
        return std::unexpected(diag_val.error());
    }
    snapshot.diagnostic = std::move(*diag_val);
    return snapshot;
}

auto HtmlEventStreamSession::deliver_updates(StreamSnapshot const& snapshot,
                                             httplib::DataSink&   sink,
                                             bool                 initial) -> bool {
    bool emitted = false;
    if (snapshot.revision && snapshot.frame_index) {
        auto revision = *snapshot.revision;
        if (revision > 0) {
            bool should_emit = false;
            if (revision > last_revision_sent_) {
                if (last_revision_sent_ > 0 && revision > last_revision_sent_ + 1) {
                    emit_reload_event(sink, last_revision_sent_, revision);
                    emitted = true;
                }
                should_emit = true;
            } else if (initial && last_revision_sent_ == 0) {
                should_emit = true;
            }
            if (should_emit) {
                emit_frame_event(sink, revision, *snapshot.frame_index);
                emitted             = true;
                last_revision_sent_ = revision;
            }
        }
    }

    bool diag_changed = initial;
    if (!initial) {
        if (!snapshot.diagnostic && !last_diagnostic_) {
            diag_changed = false;
        } else if (snapshot.diagnostic && last_diagnostic_) {
            diag_changed = !diagnostic_equals(*snapshot.diagnostic, *last_diagnostic_);
        } else {
            diag_changed = true;
        }
    }
    if (diag_changed) {
        emit_diagnostic_event(sink, snapshot.diagnostic);
        last_diagnostic_ = snapshot.diagnostic;
        emitted          = true;
    }
    return emitted;
}

void HtmlEventStreamSession::wait_for_change() {
    if (cancelled_.load(std::memory_order_acquire) || should_stop_.load(std::memory_order_acquire)) {
        return;
    }
    if (!context_) {
        std::this_thread::sleep_for(kWaitTimeout);
        return;
    }
    auto guard = context_->wait(watch_glob_);
    guard.wait_until(std::chrono::system_clock::now() + kWaitTimeout);
}

void HtmlEventStreamSession::emit_frame_event(httplib::DataSink& sink,
                                              std::uint64_t      revision,
                                              std::uint64_t      frame_index) {
    json payload{{"type", "frame"},
                 {"revision", revision},
                 {"frameIndex", frame_index},
                 {"timestamp", format_timestamp(std::chrono::system_clock::now())}};
    auto id    = std::to_string(revision);
    auto body  = payload.dump();
    write_sse_event(sink, "frame", body, &id);
    record_event("frame");
}

void HtmlEventStreamSession::emit_reload_event(httplib::DataSink& sink,
                                               std::uint64_t      from_revision,
                                               std::uint64_t      to_revision) {
    json payload{{"type", "reload"},
                 {"fromRevision", from_revision},
                 {"toRevision", to_revision}};
    auto id   = std::to_string(to_revision);
    auto body = payload.dump();
    write_sse_event(sink, "reload", body, &id);
    record_event("reload");
}

void HtmlEventStreamSession::emit_diagnostic_event(
    httplib::DataSink&                                      sink,
    std::optional<UiRuntime::Diagnostics::PathSpaceError> const& diagnostic) {
    UiRuntime::Diagnostics::PathSpaceError value{};
    bool                                  has_value = false;
    if (diagnostic) {
        value      = *diagnostic;
        has_value  = true;
    }
    bool active = has_value && has_active_diagnostic(value);
    json payload{{"type", "diagnostic"},
                 {"active", active},
                 {"code", value.code},
                 {"severity", std::string(severity_to_string(value.severity))},
                 {"message", value.message},
                 {"path", value.path},
                 {"detail", value.detail},
                 {"revision", value.revision}};
    if (value.timestamp_ns != 0) {
        payload["timestamp"] = format_timestamp_from_ns(value.timestamp_ns);
    }
    auto body = payload.dump();
    write_sse_event(sink, "diagnostic", body, nullptr);
    record_event("diagnostic");
}

void HtmlEventStreamSession::emit_keepalive(httplib::DataSink& sink) {
    auto comment = std::string{"keep-alive "};
    comment.append(format_timestamp(std::chrono::system_clock::now()));
    write_sse_comment(sink, comment);
    record_event("keepalive");
}

void HtmlEventStreamSession::emit_error_event(httplib::DataSink& sink, std::string message) {
    json payload{{"type", "error"},
                 {"message", std::move(message)}};
    auto body = payload.dump();
    write_sse_event(sink, "error", body, nullptr);
    record_event("error");
}

void HtmlEventStreamSession::record_event(std::string_view type) {
    if (metrics_ != nullptr) {
        metrics_->record_sse_event(type);
    }
}

auto SseBroadcaster::Create(HttpRequestContext& ctx, std::atomic<bool>& should_stop)
    -> std::unique_ptr<SseBroadcaster> {
    return std::unique_ptr<SseBroadcaster>(new SseBroadcaster(ctx, should_stop));
}

SseBroadcaster::SseBroadcaster(HttpRequestContext& ctx, std::atomic<bool>& should_stop)
    : ctx_(ctx)
    , should_stop_(should_stop) {}

SseBroadcaster::~SseBroadcaster() = default;

void SseBroadcaster::register_routes(httplib::Server& server) {
    server.Get(R"(/apps/([A-Za-z0-9_\-\.]+)/([A-Za-z0-9_\-\.]+)/events)",
               [this](httplib::Request const& req, httplib::Response& res) {
                   handle_events_request(req, res);
               });
}

void SseBroadcaster::handle_events_request(httplib::Request const& req, httplib::Response& res) {
    [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Events, res};
    if (req.matches.size() < 3) {
        res.status = 400;
        res.set_content("invalid route", "text/plain; charset=utf-8");
        return;
    }

    std::string app  = req.matches[1];
    std::string view = req.matches[2];
    if (!is_identifier(app) || !is_identifier(view)) {
        res.status = 400;
        res.set_content("invalid app or view", "text/plain; charset=utf-8");
        return;
    }

    auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
    auto app_root       = make_app_root_path(ctx_.options, app);
    if (!apply_rate_limits(ctx_, "apps_events", req, res, session_cookie, &app_root)) {
        return;
    }
    if (!ensure_session(ctx_, req, res, session_cookie)) {
        return;
    }

    auto html_base   = make_html_base(ctx_.options, app, view);
    auto common_base = make_common_base(ctx_.options, app, view);
    auto diag_path   = make_diagnostics_path(ctx_.options, app, view);
    auto watch_glob  = make_watch_glob(ctx_.options, app, view);
    auto resume_rev  = parse_last_event_id(req).value_or(0);

    auto session = std::make_shared<HtmlEventStreamSession>(ctx_.space,
                                                            std::move(html_base),
                                                            std::move(common_base),
                                                            std::move(diag_path),
                                                            std::move(watch_glob),
                                                            resume_rev,
                                                            &ctx_.metrics,
                                                            should_stop_);
    res.set_header("Cache-Control", "no-store");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    ctx_.metrics.record_sse_connection_open();
    res.set_chunked_content_provider(
        "text/event-stream",
        [session](size_t, httplib::DataSink& sink) {
            return session->pump(sink);
        },
        [session, this](bool done) {
            session->cancel();
            session->finalize(done);
            ctx_.metrics.record_sse_connection_close();
        });
}

} // namespace SP::ServeHtml

