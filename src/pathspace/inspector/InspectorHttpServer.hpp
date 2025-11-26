#pragma once

#include "core/Error.hpp"
#include "inspector/InspectorSnapshot.hpp"
#include "inspector/PaintScreenshotCard.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#include "httplib.h"

namespace SP {

class PathSpace;

namespace Inspector {

class InspectorHttpServer {
public:
    struct Options {
        std::string                host      = "127.0.0.1";
        int                        port      = 8765;
        InspectorSnapshotOptions   snapshot;
        PaintScreenshotCardOptions paint_card;
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

    PathSpace&                  space_;
    Options                     options_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                 server_thread_;
    std::atomic<bool>           running_{false};
    std::uint16_t               bound_port_ = 0;
    mutable std::mutex          mutex_;
};

} // namespace Inspector
} // namespace SP
