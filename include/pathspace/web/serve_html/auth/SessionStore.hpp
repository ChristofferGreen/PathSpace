#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace SP::ServeHtml {

class ServeHtmlSpace;
struct ServeHtmlOptions;

struct SessionConfig {
    std::string          cookie_name;
    std::chrono::seconds idle_timeout{std::chrono::seconds{0}};
    std::chrono::seconds absolute_timeout{std::chrono::seconds{0}};
};

class SessionStore {
public:
    explicit SessionStore(SessionConfig config);
    virtual ~SessionStore() = default;

    auto create_session(std::string username) -> std::optional<std::string>;
    auto validate(std::string const& id) -> std::optional<std::string>;
    void revoke(std::string const& id);

    auto cookie_max_age() const -> std::chrono::seconds;
    auto cookie_name() const -> std::string const&;

protected:
    using Clock = std::chrono::system_clock;

    struct SessionRecord {
        std::string       id;
        std::string       username;
        Clock::time_point created_at{};
        Clock::time_point last_seen{};
    };

    static auto generate_token() -> std::string;
    auto        is_expired(SessionRecord const& record, Clock::time_point now) const -> bool;

    virtual auto read_session(std::string const& id) -> std::optional<SessionRecord> = 0;
    virtual auto write_session(SessionRecord const& record) -> bool                 = 0;
    virtual void delete_session(std::string const& id)                              = 0;

    SessionConfig config_;
};

class InMemorySessionStore final : public SessionStore {
public:
    explicit InMemorySessionStore(SessionConfig config);

private:
    auto read_session(std::string const& id) -> std::optional<SessionRecord> override;
    auto write_session(SessionRecord const& record) -> bool override;
    void delete_session(std::string const& id) override;

    std::unordered_map<std::string, SessionRecord> sessions_;
    mutable std::mutex                             mutex_;
};

class PathSpaceSessionStore final : public SessionStore {
public:
    PathSpaceSessionStore(SessionConfig config, ServeHtmlSpace& space, std::string root_path);

private:
    static auto to_epoch_seconds(SessionStore::Clock::time_point time) -> std::int64_t;
    static auto from_epoch_seconds(std::int64_t value) -> SessionStore::Clock::time_point;
    auto read_session(std::string const& id) -> std::optional<SessionRecord> override;
    auto write_session(SessionRecord const& record) -> bool override;
    void delete_session(std::string const& id) override;

    auto build_session_path(std::string const& id) const -> std::string;

    ServeHtmlSpace& space_;
    std::string     root_path_;
};

auto make_session_store(ServeHtmlSpace&          space,
                        ServeHtmlOptions const& options,
                        SessionConfig const&    config) -> std::unique_ptr<SessionStore>;

} // namespace SP::ServeHtml
