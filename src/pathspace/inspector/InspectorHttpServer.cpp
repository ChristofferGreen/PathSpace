#include "inspector/InspectorHttpServer.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "inspector/InspectorSnapshot.hpp"
#include "inspector/PaintScreenshotCard.hpp"

#include <charconv>
#include <chrono>
#include <string_view>
#include <system_error>
#include <utility>

#include "nlohmann/json.hpp"

namespace SP::Inspector {
namespace {

[[nodiscard]] auto parse_unsigned(std::string const& value, std::size_t fallback) -> std::size_t {
    if (value.empty()) {
        return fallback;
    }
    std::size_t parsed = fallback;
    auto const* begin  = value.data();
    auto const* end    = value.data() + value.size();
    auto        result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return parsed;
}

[[nodiscard]] auto make_error(std::string const& message, int status) -> std::pair<int, std::string> {
    nlohmann::json json{
        {"error", message},
    };
    return {status, json.dump(2)};
}

} // namespace

InspectorHttpServer::InspectorHttpServer(PathSpace& space)
    : InspectorHttpServer(space, Options{}) {}

InspectorHttpServer::InspectorHttpServer(PathSpace& space, Options options)
    : space_(space)
    , options_(std::move(options)) {}

InspectorHttpServer::~InspectorHttpServer() {
    this->stop();
    this->join();
}

auto InspectorHttpServer::start() -> Expected<void> {
    std::unique_lock lock(mutex_);
    if (server_) {
        return std::unexpected(
            Error{Error::Code::InvalidError, "Inspector server already running"});
    }

    server_ = std::make_unique<httplib::Server>();
    this->configure_routes(*server_);

    auto       requested_port = options_.port;
    if (requested_port < 0) {
        requested_port = 0;
    }

    int bound_port = requested_port;
    if (requested_port == 0) {
        bound_port = server_->bind_to_any_port(options_.host);
        if (bound_port < 0) {
            server_.reset();
            return std::unexpected(
                Error{Error::Code::UnknownError, "Failed to bind inspector HTTP server"});
        }
    } else {
        if (!server_->bind_to_port(options_.host, requested_port)) {
            server_.reset();
            return std::unexpected(
                Error{Error::Code::UnknownError, "Failed to bind inspector HTTP server"});
        }
    }

    bound_port_ = static_cast<std::uint16_t>(bound_port);
    running_.store(true);

    server_thread_ = std::thread([this]() {
        if (server_) {
            server_->listen_after_bind();
        }
        running_.store(false);
    });

    lock.unlock();
    server_->wait_until_ready();
    lock.lock();
    if (!server_ || !server_->is_running()) {
        server_->stop();
        lock.unlock();
        this->join();
        lock.lock();
        server_.reset();
        bound_port_ = 0;
        running_.store(false);
        return std::unexpected(
            Error{Error::Code::UnknownError, "Inspector server failed to start listening"});
    }

    return {};
}

auto InspectorHttpServer::stop() -> void {
    std::unique_lock lock(mutex_);
    if (!server_) {
        return;
    }
    server_->stop();
    lock.unlock();
    this->join();
    lock.lock();
    server_.reset();
    bound_port_ = 0;
    running_.store(false);
}

auto InspectorHttpServer::join() -> void {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

auto InspectorHttpServer::is_running() const -> bool {
    return running_.load();
}

auto InspectorHttpServer::port() const -> std::uint16_t {
    return bound_port_;
}

auto InspectorHttpServer::configure_routes(httplib::Server& server) -> void {
    if (options_.enable_ui) {
        server.Get("/", [this](httplib::Request const&, httplib::Response& res) {
            this->handle_ui_request(res, "index.html");
        });
        server.Get("/index.html", [this](httplib::Request const&, httplib::Response& res) {
            this->handle_ui_request(res, "index.html");
        });
    }

    server.Get("/inspector/tree", [this](httplib::Request const& req, httplib::Response& res) {
        auto options = options_.snapshot;
        if (auto root = req.get_param_value("root"); !root.empty()) {
            options.root = root;
        }
        if (auto depth = req.get_param_value("depth"); !depth.empty()) {
            options.max_depth = parse_unsigned(depth, options.max_depth);
        }
        if (auto max_children = req.get_param_value("max_children"); !max_children.empty()) {
            options.max_children = parse_unsigned(max_children, options.max_children);
        }

        auto snapshot = BuildInspectorSnapshot(space_, options);
        if (!snapshot) {
            auto [status, payload] = make_error(
                describeError(snapshot.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializeInspectorSnapshot(*snapshot), "application/json");
    });

    server.Get("/inspector/node", [this](httplib::Request const& req, httplib::Response& res) {
        auto path = req.get_param_value("path");
        if (path.empty()) {
            auto [status, payload] = make_error("path parameter required", 400);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        InspectorSnapshotOptions options = options_.snapshot;
        options.root                     = path;
        options.max_depth                = parse_unsigned(req.get_param_value("depth"), 0);
        options.max_children             = parse_unsigned(req.get_param_value("max_children"), options.max_children);

        auto snapshot = BuildInspectorSnapshot(space_, options);
        if (!snapshot) {
            auto [status, payload] = make_error(
                describeError(snapshot.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializeInspectorSnapshot(*snapshot), "application/json");
    });

    server.Get("/inspector/cards/paint-example", [this](httplib::Request const& req, httplib::Response& res) {
        auto options = options_.paint_card;
        if (auto override_path = req.get_param_value("diagnostics_root"); !override_path.empty()) {
            options.diagnostics_root = override_path;
        }

        auto card = BuildPaintScreenshotCard(space_, options);
        if (!card) {
            auto [status, payload] = make_error(
                describeError(card.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializePaintScreenshotCard(*card), "application/json");
    });
}

auto InspectorHttpServer::handle_ui_request(httplib::Response& res, std::string_view asset) -> void {
    auto bundle = LoadInspectorUiAsset(options_.ui_root, asset);
    res.status  = 200;
    res.set_content(bundle.content, bundle.content_type);
    res.set_header("Cache-Control", "no-store");
}

} // namespace SP::Inspector
