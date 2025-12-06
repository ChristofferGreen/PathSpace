#pragma once

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "distributed/RemoteMountProtocol.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SP::Distributed {

namespace detail {
class RemoteMountNotificationSink;
}

struct RemoteMountThrottleOptions {
    bool                       enabled{false};
    std::uint32_t              max_requests_per_window{0};
    std::chrono::milliseconds  request_window{std::chrono::milliseconds{100}};
    std::chrono::milliseconds  penalty_increment{std::chrono::milliseconds{5}};
    std::chrono::milliseconds  penalty_cap{std::chrono::milliseconds{250}};
    std::uint32_t              max_waiters_per_session{0};
    std::chrono::milliseconds  wait_retry_after{std::chrono::milliseconds{500}};
};

struct RemoteMountExportOptions {
    std::string alias;
    std::string export_root;
    PathSpace*  space = nullptr;
    std::vector<std::string> capabilities{ "read", "wait" };
    std::string access_hint;
    RemoteMountThrottleOptions throttle;
};

struct RemoteMountServerOptions {
    std::vector<RemoteMountExportOptions> exports;
    PathSpace*  metrics_space     = nullptr;
    std::string metrics_root      = "/inspector/metrics/remotes";
    PathSpace*  diagnostics_space = nullptr;
    std::string diagnostics_root  = "/diagnostics/web/inspector/acl";
    std::chrono::milliseconds lease_duration{std::chrono::milliseconds{15000}};
    std::chrono::milliseconds heartbeat_interval{std::chrono::milliseconds{2500}};
    std::optional<RemotePayloadCompatibility> payload_compatibility;
};

class RemoteMountServer : public std::enable_shared_from_this<RemoteMountServer> {
public:
    explicit RemoteMountServer(RemoteMountServerOptions options);
    RemoteMountServer(RemoteMountServer const&)            = delete;
    RemoteMountServer& operator=(RemoteMountServer const&) = delete;
    ~RemoteMountServer();

    [[nodiscard]] auto handleMountOpen(MountOpenRequest const& request)
        -> Expected<MountOpenResponse>;
    [[nodiscard]] auto handleRead(ReadRequest const& request) -> Expected<ReadResponse>;
    [[nodiscard]] auto handleInsert(InsertRequest const& request) -> Expected<InsertResponse>;
    [[nodiscard]] auto handleTake(TakeRequest const& request) -> Expected<TakeResponse>;
    [[nodiscard]] auto handleWaitSubscribe(WaitSubscriptionRequest const& request)
        -> Expected<WaitSubscriptionAck>;
    [[nodiscard]] auto handleHeartbeat(Heartbeat const& heartbeat) -> Expected<void>;
    [[nodiscard]] auto handleNotificationStream(std::string const&    session_id,
                                                std::chrono::milliseconds timeout,
                                                std::size_t             max_batch)
        -> Expected<std::vector<Notification>>;

    void expireSessions();
    void dropSession(std::string const& session_id);

    [[nodiscard]] auto nextNotification(std::string const& subscription_id)
        -> std::optional<Notification>;
    void dropSubscription(std::string const& subscription_id);

private:
    struct ExportEntry;
    struct Session;
    struct Subscription;
    struct SessionStream;
    struct SessionThrottleState;

    friend class detail::RemoteMountNotificationSink;

    void ensureSinksAttached();
    void attachNotificationSinks(std::weak_ptr<RemoteMountServer> self);
    void detachNotificationSinks();
    void handleLocalNotification(std::string const& alias, std::string const& absolute_path);
    [[nodiscard]] auto findSessionStream(std::string const& session_id)
        -> std::shared_ptr<SessionStream>;
    void enqueueSessionNotification(std::string const& session_id, Notification const& notification);
    void closeSessionStream(std::string const& session_id);
    void applyRequestThrottle(Session const& session, ExportEntry& export_entry) const;
    [[nodiscard]] bool reserveWaiter(Session const& session,
                                     ExportEntry&      export_entry,
                                     std::chrono::milliseconds& retry_after);
    void releaseWaiter(std::weak_ptr<SessionThrottleState> throttle) const;

    RemoteMountServerOptions options_;
    std::unordered_map<std::string, ExportEntry> exports_;
    std::unordered_map<std::string, Session>     sessions_;
    std::unordered_map<std::string, Subscription> subscriptions_;
    std::unordered_map<std::string, std::uint64_t> path_versions_;
    std::unordered_map<std::string, std::shared_ptr<SessionStream>> session_streams_;

    struct NotificationAttachment {
        std::weak_ptr<PathSpaceContext> context;
        std::shared_ptr<NotificationSink> sink;
        std::shared_ptr<NotificationSink> downstream;
        std::string alias;
    };
    std::vector<NotificationAttachment> attachments_;

    std::mutex sessions_mutex_;
    std::mutex subscriptions_mutex_;
    std::mutex version_mutex_;
    mutable std::mutex metrics_mutex_;
    std::mutex session_streams_mutex_;
    mutable std::once_flag sinks_once_;
    RemotePayloadCompatibility payload_mode_{RemotePayloadCompatibility::TypedOnly};
};

} // namespace SP::Distributed
