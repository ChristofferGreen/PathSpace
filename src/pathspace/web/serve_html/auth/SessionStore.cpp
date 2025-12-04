#include <pathspace/web/serve_html/auth/SessionStore.hpp>

#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>

#include "core/Error.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace SP::ServeHtml {

namespace {

using json = nlohmann::json;

} // namespace

SessionStore::SessionStore(SessionConfig config)
    : config_{std::move(config)} {}

auto SessionStore::create_session(std::string username) -> std::optional<std::string> {
    if (username.empty()) {
        return std::nullopt;
    }
    SessionRecord record{};
    record.id = generate_token();
    record.username = std::move(username);
    record.created_at = Clock::now();
    record.last_seen = record.created_at;
    if (!write_session(record)) {
        std::cerr << "[serve_html] failed to persist session " << record.id << "\n";
        return std::nullopt;
    }
    return record.id;
}

auto SessionStore::validate(std::string const& id) -> std::optional<std::string> {
    if (id.empty()) {
        return std::nullopt;
    }

    auto const now    = Clock::now();
    auto       record = read_session(id);
    if (!record) {
        return std::nullopt;
    }
    if (is_expired(*record, now)) {
        delete_session(id);
        return std::nullopt;
    }
    record->last_seen = now;
    if (!write_session(*record)) {
        std::cerr << "[serve_html] failed to update session " << id << "\n";
        return std::nullopt;
    }
    return record->username;
}

void SessionStore::revoke(std::string const& id) {
    if (id.empty()) {
        return;
    }
    delete_session(id);
}

auto SessionStore::cookie_max_age() const -> std::chrono::seconds {
    if (config_.absolute_timeout.count() > 0) {
        return config_.absolute_timeout;
    }
    return config_.idle_timeout.count() > 0 ? config_.idle_timeout : std::chrono::seconds{0};
}

auto SessionStore::cookie_name() const -> std::string const& {
    return config_.cookie_name;
}

auto SessionStore::generate_token() -> std::string {
    std::array<unsigned char, 32> buffer{};
    std::random_device            device;
    for (auto& byte : buffer) {
        byte = static_cast<unsigned char>(device());
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (auto byte : buffer) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

auto SessionStore::is_expired(SessionRecord const& record, Clock::time_point now) const -> bool {
    if (config_.absolute_timeout.count() > 0
        && (now - record.created_at) > config_.absolute_timeout) {
        return true;
    }
    if (config_.idle_timeout.count() > 0 && (now - record.last_seen) > config_.idle_timeout) {
        return true;
    }
    return false;
}

InMemorySessionStore::InMemorySessionStore(SessionConfig config)
    : SessionStore(std::move(config)) {}

auto InMemorySessionStore::read_session(std::string const& id)
    -> std::optional<SessionRecord> {
    std::lock_guard const lock{mutex_};
    auto                  it = sessions_.find(id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

auto InMemorySessionStore::write_session(SessionRecord const& record) -> bool {
    std::lock_guard const lock{mutex_};
    sessions_[record.id] = record;
    return true;
}

void InMemorySessionStore::delete_session(std::string const& id) {
    std::lock_guard const lock{mutex_};
    sessions_.erase(id);
}

PathSpaceSessionStore::PathSpaceSessionStore(SessionConfig config,
                                             ServeHtmlSpace& space,
                                             std::string     root_path)
    : SessionStore(std::move(config))
    , space_{space}
    , root_path_{std::move(root_path)} {}

auto PathSpaceSessionStore::to_epoch_seconds(SessionStore::Clock::time_point time) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

auto PathSpaceSessionStore::from_epoch_seconds(std::int64_t value) -> SessionStore::Clock::time_point {
    return SessionStore::Clock::time_point{std::chrono::seconds{value}};
}

auto PathSpaceSessionStore::read_session(std::string const& id)
    -> std::optional<SessionRecord> {
    auto const path  = build_session_path(id);
    auto       value = space_.read<std::string, std::string>(path);
    if (!value) {
        auto const& error = value.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return std::nullopt;
        }
        std::cerr << "[serve_html] failed to read session " << id << ": "
                  << SP::describeError(error) << "\n";
        return std::nullopt;
    }

    auto payload = json::parse(*value, nullptr, false);
    if (payload.is_discarded()) {
        std::cerr << "[serve_html] session payload for " << id << " is invalid JSON\n";
        (void)clear_queue<std::string>(space_, path);
        return std::nullopt;
    }
    if (!payload.contains("username") || !payload["username"].is_string()
        || !payload.contains("created_at") || !payload["created_at"].is_number_integer()
        || !payload.contains("last_seen") || !payload["last_seen"].is_number_integer()) {
        std::cerr << "[serve_html] session payload for " << id << " missing required fields\n";
        (void)clear_queue<std::string>(space_, path);
        return std::nullopt;
    }

    SessionRecord record{};
    record.id        = id;
    record.username  = payload["username"].get<std::string>();
    record.created_at = from_epoch_seconds(payload["created_at"].get<std::int64_t>());
    record.last_seen  = from_epoch_seconds(payload["last_seen"].get<std::int64_t>());
    return record;
}

auto PathSpaceSessionStore::write_session(SessionRecord const& record) -> bool {
    auto const path = build_session_path(record.id);
    json       payload{{"version", 1},
                       {"username", record.username},
                       {"created_at", to_epoch_seconds(record.created_at)},
                       {"last_seen", to_epoch_seconds(record.last_seen)}};
    auto serialized = payload.dump();
    auto result     = replace_single_value<std::string>(space_, path, serialized);
    if (!result) {
        std::cerr << "[serve_html] failed to persist session " << record.id << ": "
                  << SP::describeError(result.error()) << "\n";
        return false;
    }
    return true;
}

void PathSpaceSessionStore::delete_session(std::string const& id) {
    auto const path = build_session_path(id);
    auto       result = clear_queue<std::string>(space_, path);
    if (!result) {
        std::cerr << "[serve_html] failed to clear session " << id << ": "
                  << SP::describeError(result.error()) << "\n";
    }
}

auto PathSpaceSessionStore::build_session_path(std::string const& id) const -> std::string {
    if (root_path_.empty() || root_path_ == "/") {
        std::string path{"/"};
        path.append(id);
        return path;
    }
    std::string path = root_path_;
    path.push_back('/');
    path.append(id);
    return path;
}

auto make_session_store(ServeHtmlSpace&          space,
                        ServeHtmlOptions const& options,
                        SessionConfig const&    config) -> std::unique_ptr<SessionStore> {
    auto backend = options.session_store_backend;
    if (backend == "pathspace") {
        return std::make_unique<PathSpaceSessionStore>(config, space, options.session_store_path);
    }
    if (backend != "memory") {
        std::cerr << "[serve_html] unsupported session store backend '" << backend
                  << "', defaulting to in-memory\n";
    }
    return std::make_unique<InMemorySessionStore>(config);
}

} // namespace SP::ServeHtml
