#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

namespace SP {
class PathSpaceContext;
}

namespace httplib {
class Server;
class Request;
class Response;
struct DataSink;
} // namespace httplib

namespace SP::ServeHtml {

namespace UiRuntime = SP::UI::Runtime;

class ServeHtmlSpace;
struct ServeHtmlOptions;
class MetricsCollector;
struct HttpRequestContext;

class HtmlEventStreamSession {
public:
    HtmlEventStreamSession(ServeHtmlSpace&   space,
                           std::string      html_base,
                           std::string      common_base,
                           std::string      diagnostics_path,
                           std::string      watch_glob,
                           std::uint64_t    resume_revision,
                           MetricsCollector* metrics,
                           std::atomic<bool>& should_stop);

    auto pump(httplib::DataSink& sink) -> bool;
    void cancel();
    void finalize(bool done);

private:
    static constexpr auto kKeepAliveInterval = std::chrono::milliseconds(5000);
    static constexpr auto kWaitTimeout       = std::chrono::milliseconds(1500);

    struct StreamSnapshot {
        std::optional<std::uint64_t> frame_index;
        std::optional<std::uint64_t> revision;
        std::optional<UiRuntime::Diagnostics::PathSpaceError> diagnostic;
    };

    auto read_snapshot() -> SP::Expected<StreamSnapshot>;
    auto deliver_updates(StreamSnapshot const& snapshot,
                         httplib::DataSink&   sink,
                         bool                 initial) -> bool;
    void wait_for_change();
    void emit_frame_event(httplib::DataSink& sink, std::uint64_t revision, std::uint64_t frame_index);
    void emit_reload_event(httplib::DataSink& sink, std::uint64_t from_revision, std::uint64_t to_revision);
    void emit_diagnostic_event(httplib::DataSink&                                      sink,
                               std::optional<UI::Runtime::Diagnostics::PathSpaceError> const& diagnostic);
    void emit_keepalive(httplib::DataSink& sink);
    void emit_error_event(httplib::DataSink& sink, std::string message);
    void record_event(std::string_view type);

    ServeHtmlSpace&                               space_;
    std::shared_ptr<SP::PathSpaceContext>         context_;
    std::string                                   html_base_;
    std::string                                   common_base_;
    std::string                                   diagnostics_path_;
    std::string                                   watch_glob_;
    std::uint64_t                                 last_revision_sent_{0};
    std::optional<UiRuntime::Diagnostics::PathSpaceError> last_diagnostic_;
    bool                                          started_{false};
    std::atomic<bool>                             cancelled_{false};
    std::chrono::steady_clock::time_point         last_keepalive_{std::chrono::steady_clock::now()};
    MetricsCollector*                             metrics_{nullptr};
    std::atomic<bool>&                            should_stop_;
};

class SseBroadcaster {
public:
    static auto Create(HttpRequestContext& ctx, std::atomic<bool>& should_stop)
        -> std::unique_ptr<SseBroadcaster>;

    void register_routes(httplib::Server& server);

    ~SseBroadcaster();

private:
    SseBroadcaster(HttpRequestContext& ctx, std::atomic<bool>& should_stop);

    void handle_events_request(httplib::Request const& req, httplib::Response& res);

    HttpRequestContext& ctx_;
    std::atomic<bool>& should_stop_;
};

} // namespace SP::ServeHtml
