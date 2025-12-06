#pragma once

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "distributed/RemoteMountProtocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SP::Distributed {

struct RemoteMountTlsClientConfig {
    std::string                ca_cert_path;
    std::string                client_cert_path;
    std::string                client_key_path;
    std::string                sni_host;
    bool                       verify_server_certificate{true};
    std::chrono::milliseconds connect_timeout{std::chrono::milliseconds{2000}};
};

struct RemoteMountClientOptions {
    std::string                     alias;
    std::string                     export_root;
    std::string                     mount_path;
    std::string                     host{"127.0.0.1"};
    std::uint16_t                   port{0};
    bool                            use_tls{true};
    std::string                     client_id{"pathspace-client"};
    std::vector<CapabilityRequest>  capabilities;
    std::uint32_t                   take_batch_size{4};
    AuthContext                     auth;
    std::optional<RemoteMountTlsClientConfig> tls;
    enum class MirrorTarget {
        RootSpace,
        MetricsSpace,
    };

    enum class MirrorMode {
        AppendOnly,
        TreeSnapshot,
    };

    struct MirrorPathOptions {
        MirrorMode                 mode{MirrorMode::TreeSnapshot};
        MirrorTarget               target{MirrorTarget::RootSpace};
        std::string                remote_root;
        std::string                local_root;
        std::size_t                max_depth{4};
        std::size_t                max_children{VisitOptions::kDefaultMaxChildren};
        std::size_t                max_nodes{256};
        std::chrono::milliseconds interval{std::chrono::milliseconds{500}};
        bool                       enabled{true};
    };

    std::vector<MirrorPathOptions> mirrors;
};

struct RemoteMountManagerOptions {
    PathSpace*                            root_space{nullptr};
    PathSpace*                            metrics_space{nullptr};
    std::string                           metrics_root{"/inspector/metrics/remotes"};
    std::string                           diagnostics_root{"/diagnostics/errors/live/remotes"};
    std::vector<RemoteMountClientOptions> mounts;
    std::optional<RemotePayloadCompatibility> payload_compatibility;
};

struct RemoteMountStatus {
    std::string               alias;
    bool                      connected{false};
    std::string               message;
    std::chrono::milliseconds last_latency{std::chrono::milliseconds{0}};
    std::chrono::milliseconds average_latency{std::chrono::milliseconds{0}};
    std::chrono::milliseconds max_latency{std::chrono::milliseconds{0}};
    std::uint64_t             success_count{0};
    std::uint64_t             error_count{0};
    std::uint64_t             consecutive_errors{0};
    std::uint64_t             waiter_depth{0};
    std::uint64_t             max_waiter_depth{0};
    std::uint64_t             queued_notifications{0};
    std::uint64_t             dropped_notifications{0};
    bool                      throttled{false};
    std::chrono::milliseconds retry_after_hint{std::chrono::milliseconds{0}};
};

class RemoteMountSession {
public:
    virtual ~RemoteMountSession() = default;

    virtual auto open(MountOpenRequest const& request) -> Expected<MountOpenResponse> = 0;
    virtual auto read(ReadRequest const& request) -> Expected<ReadResponse>          = 0;
    virtual auto insert(InsertRequest const& request) -> Expected<InsertResponse>    = 0;
    virtual auto take(TakeRequest const& request) -> Expected<TakeResponse>          = 0;
    virtual auto waitSubscribe(WaitSubscriptionRequest const& request)
        -> Expected<WaitSubscriptionAck>                                              = 0;
    virtual auto nextNotification(std::string const& subscription_id,
                                  std::chrono::milliseconds timeout)
        -> Expected<std::optional<Notification>>                                       = 0;
    virtual auto streamNotifications(std::string const& session_id,
                                     std::chrono::milliseconds timeout,
                                     std::size_t max_batch)
        -> Expected<std::vector<Notification>>                                         = 0;
    virtual auto heartbeat(Heartbeat const& heartbeat) -> Expected<void>             = 0;
};

class RemoteMountSessionFactory {
public:
    virtual ~RemoteMountSessionFactory() = default;
    virtual auto create(RemoteMountClientOptions const& options)
        -> Expected<std::shared_ptr<RemoteMountSession>> = 0;
};

class RemoteMountManager {
public:
    RemoteMountManager(RemoteMountManagerOptions options,
                       std::shared_ptr<RemoteMountSessionFactory> factory);
    RemoteMountManager(RemoteMountManager const&)            = delete;
    RemoteMountManager& operator=(RemoteMountManager const&) = delete;
    RemoteMountManager(RemoteMountManager&&)                 = delete;
    RemoteMountManager& operator=(RemoteMountManager&&)      = delete;
    ~RemoteMountManager();

    void start();
    void stop();

    [[nodiscard]] auto statuses() const -> std::vector<RemoteMountStatus>;
    [[nodiscard]] bool running() const;

private:
    struct MirrorAssignment;
    struct MountState;
    class RemoteMountSpace;

    RemoteMountManagerOptions                         options_;
    std::shared_ptr<RemoteMountSessionFactory>        factory_;
    std::vector<std::unique_ptr<MountState>>          mounts_;
    std::atomic<bool>                                 running_{false};
    std::atomic<std::uint64_t>                        request_counter_{1};
    RemotePayloadCompatibility                         payload_mode_{RemotePayloadCompatibility::TypedOnly};

    [[nodiscard]] Expected<void> ensureSession(MountState& state);
    [[nodiscard]] Expected<void> openSession(MountState& state);
    void                          heartbeatLoop(MountState* state);
    [[nodiscard]] std::optional<Error> sendHeartbeat(MountState& state);
    void                          publishMetrics(MountState const& state) const;
    void                          recordSuccess(MountState& state,
                                               std::chrono::milliseconds latency);
    void                          recordError(MountState& state,
                                              Error const&             error,
                                              bool                     connection_issue);
    [[nodiscard]] std::string     buildRemotePath(MountState const& state,
                                                  std::string const&  relative) const;
    [[nodiscard]] std::optional<ValuePayload> popCachedValue(MountState& state,
                                                             std::string const& remote_path);
    void                                      cacheValues(MountState& state,
                                                         std::string const& remote_path,
                                                         std::span<ValuePayload> payloads);
    [[nodiscard]] Expected<ValuePayload>      materializeExecutionPayload(InputData const& data) const;
    [[nodiscard]] std::optional<Error>        applyValuePayload(ValuePayload const& payload,
                                                               InputMetadata const& metadata,
                                                               void*               obj);
    [[nodiscard]] InsertReturn    performInsert(MountState& state,
                                                std::string const& relative,
                                                InputData const&    data);
    [[nodiscard]] std::optional<Error> performRead(MountState& state,
                                                   std::string const& relative,
                                                   InputMetadata const& metadata,
                                                   Out const&          options,
                                                   void*               obj);
    [[nodiscard]] std::optional<Error> performTake(MountState& state,
                                                   std::string const& relative,
                                                   InputMetadata const& metadata,
                                                   Out const&          options,
                                                   void*               obj);
    [[nodiscard]] std::optional<Error> performWait(MountState& state,
                                                   std::string const& relative,
                                                   InputMetadata const& metadata,
                                                   Out const&          options,
                                                   void*               obj);
    void                          configureMirrors(MountState& state);
    void                          startMirrorThread(MountState& state);
    void                          stopMirrorThread(MountState& state);
    void                          startNotificationThread(MountState& state);
    void                          stopNotificationThread(MountState& state);
    void                          notificationLoop(MountState* state);
    void                          deliverNotification(MountState& state,
                                                      Notification const& notification);
    void                          failPendingWaiters(MountState& state, Error const& error);
    void                          mirrorLoop(MountState* state);
    void runMirrorAssignment(MountState& state, MirrorAssignment& assignment);
    [[nodiscard]] std::optional<Error> mirrorAppendOnly(MountState& state,
                                                        MirrorAssignment& assignment,
                                                        std::shared_ptr<RemoteMountSession> const& session,
                                                        std::string const& session_id);
    [[nodiscard]] std::optional<Error> mirrorTreeSnapshot(MountState& state,
                                                          MirrorAssignment& assignment,
                                                          std::shared_ptr<RemoteMountSession> const& session,
                                                          std::string const& session_id);
    [[nodiscard]] std::optional<Error> mirrorSingleNode(PathSpace&                              space,
                                                        std::string const&                      local_path,
                                                        ValuePayload const&                     payload);
    [[nodiscard]] std::optional<Error> copyRemoteNode(MountState&                          state,
                                                      MirrorAssignment const&             assignment,
                                                      std::shared_ptr<RemoteMountSession> const& session,
                                                      std::string const&                  session_id,
                                                      std::string const&                  remote_path,
                                                      std::string const&                  local_path);
};

} // namespace SP::Distributed
