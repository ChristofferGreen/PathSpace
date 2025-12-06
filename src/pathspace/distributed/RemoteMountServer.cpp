#include "pathspace/distributed/RemoteMountServer.hpp"

#include "core/NodeData.hpp"
#include "distributed/TypedPayloadBridge.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "inspector/InspectorMetricUtils.hpp"
#include "log/TaggedLogger.hpp"
#include "path/ConcretePath.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace SP::Distributed {
namespace {

constexpr std::string_view kCapRead   = "read";
constexpr std::string_view kCapWait   = "wait";
constexpr std::string_view kCapInsert = "insert";
constexpr std::string_view kCapTake   = "take";
constexpr std::uint32_t    kMaxTakeBatch               = 64;
constexpr std::size_t      kNotificationThrottleThreshold = 128;
constexpr std::size_t      kNotificationMaxQueue        = 1024;
constexpr std::chrono::milliseconds kNotificationThrottleWindow{250};

auto make_error(Error::Code code, std::string_view message) -> Error {
    return Error{code, std::string(message)};
}

[[nodiscard]] auto canonicalize_path(std::string_view path) -> Expected<std::string> {
    ConcretePathString candidate{std::string(path)};
    auto               canonical = candidate.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    return canonical->getPath();
}

[[nodiscard]] bool path_within(std::string const& absolute, std::string const& root) {
    if (root == "/" || root.empty()) {
        return true;
    }
    if (absolute.size() < root.size()) {
        return false;
    }
    if (absolute.rfind(root, 0) != 0) {
        return false;
    }
    if (absolute.size() == root.size()) {
        return true;
    }
    return absolute[root.size()] == '/';
}

[[nodiscard]] auto current_time_ms() -> std::uint64_t {
    auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

[[nodiscard]] auto encode_base64(std::span<std::byte const> bytes) -> std::string {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

    std::size_t index = 0;
    while (index + 2U < bytes.size()) {
        auto b0 = static_cast<unsigned char>(bytes[index]);
        auto b1 = static_cast<unsigned char>(bytes[index + 1U]);
        auto b2 = static_cast<unsigned char>(bytes[index + 2U]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
        encoded.push_back(kAlphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)]);
        encoded.push_back(kAlphabet[b2 & 0x3FU]);
        index += 3U;
    }

    if (index < bytes.size()) {
        auto b0 = static_cast<unsigned char>(bytes[index]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        if (index + 1U < bytes.size()) {
            auto b1 = static_cast<unsigned char>(bytes[index + 1U]);
            encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
            encoded.push_back(kAlphabet[(b1 & 0x0FU) << 2U]);
            encoded.push_back('=');
        } else {
            encoded.push_back(kAlphabet[(b0 & 0x03U) << 4U]);
            encoded.push_back('=');
            encoded.push_back('=');
        }
    }

    return encoded;
}

[[nodiscard]] auto decode_base64(std::string_view input) -> Expected<std::vector<std::uint8_t>> {
    static constexpr char kPad = '=';
    auto decode_char = [](char ch) -> int {
        if ('A' <= ch && ch <= 'Z') return ch - 'A';
        if ('a' <= ch && ch <= 'z') return ch - 'a' + 26;
        if ('0' <= ch && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };

    std::vector<std::uint8_t> output;
    output.reserve((input.size() * 3U) / 4U);
    std::array<int, 4> chunk{};
    std::size_t        idx = 0;
    while (idx < input.size()) {
        std::size_t filled = 0;
        while (filled < 4 && idx < input.size()) {
            auto value = decode_char(input[idx]);
            if (value >= 0) {
                chunk[filled++] = value;
            } else if (input[idx] == kPad) {
                chunk[filled++] = -1;
            }
            ++idx;
        }
        if (filled < 2) {
            break;
        }
        output.push_back(static_cast<std::uint8_t>((chunk[0] << 2) | ((chunk[1] & 0x30) >> 4)));
        if (chunk[2] >= 0) {
            output.push_back(static_cast<std::uint8_t>(((chunk[1] & 0x0F) << 4)
                                                      | ((chunk[2] & 0x3C) >> 2)));
            if (chunk[3] >= 0) {
                output.push_back(static_cast<std::uint8_t>(((chunk[2] & 0x03) << 6) | chunk[3]));
            }
        }
    }
    return output;
}

[[nodiscard]] auto summarize_error(Error const& error) -> std::string {
    auto code = errorCodeToString(error.code);
    if (error.message && !error.message->empty()) {
        std::string summary{code};
        summary.push_back(':');
        summary.append(*error.message);
        return summary;
    }
    return std::string{code};
}

std::atomic<std::uint64_t> g_session_counter{1};

[[nodiscard]] auto front_type_name(NodeData const& node) -> std::optional<std::string> {
    auto const& summary = node.typeSummary();
    if (summary.empty() || summary.front().typeInfo == nullptr) {
        return std::nullopt;
    }
    return std::string(summary.front().typeInfo->name());
}

[[nodiscard]] auto encode_node_value(NodeData const& node,
                                     std::optional<std::string> type_hint) -> Expected<ValuePayload> {
    auto bytes = node.frontSerializedValueBytes();
    if (!bytes) {
        return std::unexpected(
            make_error(Error::Code::InvalidType, "unable to encode value payload"));
    }
    auto type_name = front_type_name(node);
    if (!type_name && type_hint && !type_hint->empty()) {
        type_name = type_hint;
    }
    if (!type_name) {
        return std::unexpected(
            make_error(Error::Code::InvalidType, "value missing type metadata"));
    }
    if (type_hint && !type_hint->empty() && *type_name != *type_hint) {
        return std::unexpected(
            make_error(Error::Code::InvalidType, "type mismatch"));
    }
    ValuePayload payload;
    payload.encoding  = std::string{kEncodingTypedSlidingBuffer};
    payload.type_name = *type_name;
    payload.data      = encode_base64(*bytes);
    return payload;
}

[[nodiscard]] auto snapshot_front_node(std::vector<std::byte> const& snapshot)
    -> Expected<NodeData> {
    auto restored = NodeData::deserializeSnapshot(std::span<const std::byte>(snapshot.data(), snapshot.size()));
    if (!restored) {
        return std::unexpected(make_error(Error::Code::InvalidType,
                                          "unable to decode serialized snapshot"));
    }
    NodeData front;
    if (auto error = restored->popFrontSerialized(front); error.has_value()) {
        return std::unexpected(*error);
    }
    return front;
}

[[nodiscard]] auto encode_snapshot_value(std::vector<std::byte> const& snapshot,
                                         std::optional<std::string> type_hint)
    -> Expected<ValuePayload> {
    auto node = snapshot_front_node(snapshot);
    if (!node) {
        return std::unexpected(node.error());
    }
    return encode_node_value(*node, type_hint);
}

[[nodiscard]] auto validate_alias(std::string const& alias) -> Expected<void> {
    if (alias.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "alias required"));
    }
    for (unsigned char ch : alias) {
        if (std::isalnum(ch) != 0 || ch == '_' || ch == '-') {
            continue;
        }
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "alias contains invalid characters"));
    }
    return {};
}

[[nodiscard]] auto error_payload(std::string code,
                                 std::string message,
                                 bool retryable = false) -> ErrorPayload {
    return ErrorPayload{
        .code        = std::move(code),
        .message     = std::move(message),
        .retryable   = retryable,
        .retry_after = std::chrono::milliseconds{0},
    };
}

[[nodiscard]] auto metrics_base(RemoteMountServerOptions const& options,
                                std::string const& alias) -> std::string {
    std::string path = options.metrics_root.empty() ? "/inspector/metrics/remotes"
                                                   : options.metrics_root;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    if (alias.empty()) {
        path.push_back('_');
    } else {
        path.append(alias);
    }
    return path;
}

template <typename T>
void publish_metric(RemoteMountServerOptions const& options,
                    std::string const& alias,
                    std::string_view suffix,
                    T const& value) {
    if (!options.metrics_space) {
        return;
    }
    auto path = metrics_base(options, alias);
    if (!suffix.empty()) {
        if (suffix.front() != '/') {
            path.push_back('/');
        }
        path.append(suffix.begin(), suffix.end());
    }
    auto replaced = Inspector::Detail::ReplaceMetricValue(*options.metrics_space, path, value);
    (void)replaced;
}

void record_diagnostic(RemoteMountServerOptions const& options,
                       std::string const& alias,
                       std::string_view code,
                       std::string_view message,
                       AuthContext const& auth) {
    if (!options.diagnostics_space) {
        return;
    }
    std::string root = options.diagnostics_root.empty() ? "/diagnostics/web/inspector/acl"
                                                        : options.diagnostics_root;
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    if (alias.empty()) {
        root.push_back('_');
    } else {
        root.append(alias);
    }
    root.append("/events/");
    std::ostringstream oss;
    oss << std::setw(20) << std::setfill('0') << current_time_ms();
    root.append(oss.str());

    nlohmann::json payload{{"code", code},
                           {"message", message},
                           {"subject", auth.subject},
                           {"audience", auth.audience}};
    if (!auth.fingerprint.empty()) {
        payload["fingerprint"] = auth.fingerprint;
    }
    if (!auth.proof.empty()) {
        payload["proof"] = auth.proof;
    }
    auto inserted = options.diagnostics_space->insert(root, payload.dump());
    (void)inserted;
}

} // namespace

namespace detail {

class RemoteMountNotificationSink final : public NotificationSink {
public:
    RemoteMountNotificationSink(std::weak_ptr<RemoteMountServer> server,
                                std::string alias,
                                std::shared_ptr<NotificationSink> downstream)
        : server_(std::move(server))
        , alias_(std::move(alias))
        , downstream_(std::move(downstream)) {}

    void notify(const std::string& notificationPath) override {
        if (auto server = server_.lock()) {
            server->handleLocalNotification(alias_, notificationPath);
        }
        if (downstream_) {
            downstream_->notify(notificationPath);
        }
    }

private:
    std::weak_ptr<RemoteMountServer> server_;
    std::string                      alias_;
    std::shared_ptr<NotificationSink> downstream_;
};

} // namespace detail

struct RemoteMountServer::ExportEntry {
    RemoteMountExportOptions        options;
    std::string                     canonical_root;
    PathSpace*                      space = nullptr;
    std::unordered_set<std::string> capability_set;
    std::uint64_t                   active_sessions = 0;
    std::uint64_t                   total_sessions  = 0;
    std::uint64_t                   waiter_count    = 0;
    RemoteMountThrottleOptions      throttle;
    std::uint64_t                   throttle_hits{0};
    std::uint64_t                   waiter_rejections{0};
};

struct RemoteMountServer::Session {
    std::string                               session_id;
    std::string                               alias;
    std::uint64_t                             lease_expires_ms{0};
    std::chrono::steady_clock::time_point     deadline;
    std::vector<std::string>                  capabilities;
    std::shared_ptr<SessionThrottleState>     throttle;
};

struct RemoteMountServer::SessionThrottleState {
    RemoteMountThrottleOptions                options;
    mutable std::mutex                        mutex;
    std::chrono::steady_clock::time_point     next_allowed{std::chrono::steady_clock::now()};
    std::uint32_t                             active_waiters{0};
};

struct RemoteMountServer::Subscription {
    std::string              subscription_id;
    std::string              session_id;
    std::string              alias;
    std::string              path;
    bool                     include_value   = false;
    bool                     include_children = false;
    std::optional<std::uint64_t> min_version;
    std::deque<Notification> pending;
    std::weak_ptr<SessionThrottleState> throttle;
};

struct RemoteMountServer::SessionStream {
    std::string                             alias;
    std::mutex                              mutex;
    std::condition_variable                 cv;
    std::deque<Notification>                pending;
    std::size_t                             dropped{0};
    bool                                    closed{false};
    bool                                    throttled{false};
    std::chrono::steady_clock::time_point   throttle_until{};
};

RemoteMountServer::RemoteMountServer(RemoteMountServerOptions options)
    : options_(std::move(options)) {
    if (!options_.payload_compatibility.has_value()) {
        options_.payload_compatibility = defaultRemotePayloadCompatibility();
    }
    payload_mode_ = *options_.payload_compatibility;
    if (allowLegacyPayloads(payload_mode_)) {
        sp_log(
            "RemoteMountServer allowing legacy remote payload encodings (set PATHSPACE_REMOTE_TYPED_PAYLOADS=1 to re-disable)",
            "RemoteMountServer");
    }
    for (auto& export_option : options_.exports) {
        if (!export_option.space) {
            sp_log("RemoteMountServer skipping export with null space", "RemoteMountServer");
            continue;
        }
        auto alias_check = validate_alias(export_option.alias);
        if (!alias_check) {
            sp_log("RemoteMountServer skipping export due to invalid alias", "RemoteMountServer");
            continue;
        }
        auto canonical_root = canonicalize_path(export_option.export_root);
        if (!canonical_root) {
            sp_log("RemoteMountServer skipping export due to invalid root", "RemoteMountServer");
            continue;
        }
        ExportEntry entry;
        entry.options        = export_option;
        entry.space          = export_option.space;
        entry.canonical_root = *canonical_root;
        entry.throttle       = export_option.throttle;
        for (auto const& cap : export_option.capabilities) {
            entry.capability_set.insert(cap);
        }
        if (entry.capability_set.empty()) {
            entry.capability_set.insert(std::string(kCapRead));
            entry.capability_set.insert(std::string(kCapWait));
        }
        exports_.emplace(entry.options.alias, std::move(entry));
    }
}

RemoteMountServer::~RemoteMountServer() {
    detachNotificationSinks();
}

void RemoteMountServer::ensureSinksAttached() {
    std::call_once(sinks_once_, [weak = weak_from_this(), this]() {
        this->attachNotificationSinks(weak);
    });
}

void RemoteMountServer::attachNotificationSinks(std::weak_ptr<RemoteMountServer> self) {
    for (auto& [alias, entry] : exports_) {
        if (!entry.space) {
            continue;
        }
        auto context = entry.space->sharedContext();
        if (!context) {
            continue;
        }
        auto downstream = context->getSink().lock();
        auto sink       = std::make_shared<detail::RemoteMountNotificationSink>(self, alias, downstream);
        context->setSink(sink);
        attachments_.push_back(NotificationAttachment{
            .context    = context,
            .sink       = sink,
            .downstream = downstream,
            .alias      = alias,
        });
    }
}

void RemoteMountServer::detachNotificationSinks() {
    for (auto& attachment : attachments_) {
        if (auto context = attachment.context.lock()) {
            context->setSink(attachment.downstream);
        }
    }
    attachments_.clear();
}

void RemoteMountServer::expireSessions() {
    std::vector<std::string> expired;
    {
        std::lock_guard lock(sessions_mutex_);
        auto            now = std::chrono::steady_clock::now();
        for (auto const& [session_id, session] : sessions_) {
            if (now >= session.deadline) {
                expired.push_back(session_id);
            }
        }
    }
    for (auto const& session_id : expired) {
        dropSession(session_id);
    }
}

void RemoteMountServer::dropSession(std::string const& session_id) {
    std::optional<Session> removed;
    {
        std::lock_guard lock(sessions_mutex_);
        if (auto it = sessions_.find(session_id); it != sessions_.end()) {
            removed = it->second;
            sessions_.erase(it);
        }
    }
    if (!removed) {
        return;
    }
    closeSessionStream(session_id);
    std::unordered_map<std::string, std::size_t> removed_waiters;
    {
        std::lock_guard sub_lock(subscriptions_mutex_);
        for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
            if (it->second.session_id == session_id) {
                releaseWaiter(it->second.throttle);
                removed_waiters[it->second.alias] += 1;
                it = subscriptions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (auto export_it = exports_.find(removed->alias); export_it != exports_.end()) {
        std::lock_guard metrics_lock(metrics_mutex_);
        if (export_it->second.active_sessions > 0) {
            --export_it->second.active_sessions;
        }
        publish_metric(options_, removed->alias, "server/sessions/active",
                       export_it->second.active_sessions);
    }

    for (auto const& [alias, count] : removed_waiters) {
        auto export_it = exports_.find(alias);
        if (export_it == exports_.end()) {
            continue;
        }
        std::lock_guard metrics_lock(metrics_mutex_);
        auto& waiter_count = export_it->second.waiter_count;
        if (waiter_count >= count) {
            waiter_count -= count;
        } else {
            waiter_count = 0;
        }
        publish_metric(options_, alias, "waiters/current", waiter_count);
    }
}

void RemoteMountServer::dropSubscription(std::string const& subscription_id) {
    std::optional<Subscription> removed;
    {
        std::lock_guard lock(subscriptions_mutex_);
        if (auto it = subscriptions_.find(subscription_id); it != subscriptions_.end()) {
            removed = it->second;
            subscriptions_.erase(it);
        }
    }
    if (!removed) {
        return;
    }
    releaseWaiter(removed->throttle);
    if (auto export_it = exports_.find(removed->alias); export_it != exports_.end()) {
        std::lock_guard metrics_lock(metrics_mutex_);
        if (export_it->second.waiter_count > 0) {
            --export_it->second.waiter_count;
        }
        publish_metric(options_, removed->alias, "waiters/current",
                       export_it->second.waiter_count);
    }
}

auto RemoteMountServer::handleMountOpen(MountOpenRequest const& request)
    -> Expected<MountOpenResponse> {
    ensureSinksAttached();
    expireSessions();

    auto fail = [&](Error::Code code,
                    std::string_view diag_code,
                    std::string_view message) -> Expected<MountOpenResponse> {
        record_diagnostic(options_, request.alias, diag_code, message, request.auth);
        return std::unexpected(make_error(code, message));
    };

    auto canonical_root = canonicalize_path(request.export_root);
    if (!canonical_root) {
        return fail(canonical_root.error().code, "invalid_root", "failed to canonicalize export root");
    }

    auto export_it = exports_.find(request.alias);
    if (export_it == exports_.end()) {
        return fail(Error::Code::NoSuchPath, "invalid_alias", "unknown mount alias");
    }
    auto& export_entry = export_it->second;

    if (*canonical_root != export_entry.canonical_root) {
        return fail(Error::Code::InvalidPath, "root_mismatch", "export root mismatch");
    }

    if (request.auth.subject.empty() || request.auth.proof.empty()) {
        return fail(Error::Code::InvalidPermissions,
                    "auth_missing",
                    "auth subject/proof required");
    }

    std::vector<std::string> granted;
    for (auto const& capability : request.capabilities) {
        if (export_entry.capability_set.contains(capability.name)) {
            granted.push_back(capability.name);
        }
    }
    if (granted.empty()) {
        granted.push_back(std::string(kCapRead));
    }

    Session session;
    session.alias        = request.alias;
    session.session_id   = std::string("sess-")
                         + std::to_string(g_session_counter.fetch_add(1));
    session.capabilities = granted;
    bool needs_throttle = export_entry.throttle.enabled
        || export_entry.throttle.max_waiters_per_session > 0;
    if (needs_throttle) {
        auto throttle_state     = std::make_shared<SessionThrottleState>();
        throttle_state->options = export_entry.throttle;
        throttle_state->next_allowed = std::chrono::steady_clock::now();
        session.throttle        = std::move(throttle_state);
    }
    auto now_ms          = current_time_ms();
    session.lease_expires_ms
        = now_ms + static_cast<std::uint64_t>(options_.lease_duration.count());
    session.deadline = std::chrono::steady_clock::now() + options_.lease_duration;

    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.emplace(session.session_id, session);
    }
    {
        std::lock_guard stream_lock(session_streams_mutex_);
        auto stream = std::make_shared<SessionStream>();
        stream->alias = session.alias;
        session_streams_.emplace(session.session_id, std::move(stream));
    }

    {
        std::lock_guard lock(metrics_mutex_);
        ++export_entry.active_sessions;
        ++export_entry.total_sessions;
        publish_metric(options_, session.alias, "server/sessions/active",
                       export_entry.active_sessions);
        publish_metric(options_, session.alias, "server/sessions/total",
                       export_entry.total_sessions);
        publish_metric(options_, session.alias, "status/lease_expires_ms", session.lease_expires_ms);
        publish_metric(options_, session.alias, "status/last_subject", request.auth.subject);
        if (!request.auth.fingerprint.empty()) {
            publish_metric(options_, session.alias, "status/last_fingerprint",
                           request.auth.fingerprint);
        }
    }

    record_diagnostic(options_, session.alias, "mount_open", "session accepted", request.auth);

    MountOpenResponse response;
    response.version            = request.version;
    response.request_id         = request.request_id;
    response.accepted           = true;
    response.session_id         = session.session_id;
    response.granted_capabilities = session.capabilities;
    response.lease_expires_ms   = session.lease_expires_ms;
    response.heartbeat_interval = options_.heartbeat_interval;
    return response;
}

auto RemoteMountServer::handleHeartbeat(Heartbeat const& heartbeat) -> Expected<void> {
    std::lock_guard lock(sessions_mutex_);
    auto            it = sessions_.find(heartbeat.session_id);
    if (it == sessions_.end()) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
    }
    it->second.deadline
        = std::chrono::steady_clock::now() + options_.lease_duration;
    it->second.lease_expires_ms = current_time_ms()
        + static_cast<std::uint64_t>(options_.lease_duration.count());
    publish_metric(options_, it->second.alias, "status/lease_expires_ms",
                   it->second.lease_expires_ms);
    return {};
}

auto RemoteMountServer::handleNotificationStream(std::string const&    session_id,
                                                 std::chrono::milliseconds timeout,
                                                 std::size_t             max_batch)
    -> Expected<std::vector<Notification>> {
    expireSessions();
    auto stream = findSessionStream(session_id);
    if (!stream) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
    }
    if (max_batch == 0) {
        max_batch = 1;
    }
    std::unique_lock<std::mutex> stream_lock(stream->mutex);
    if (!stream->cv.wait_for(stream_lock, timeout, [&]() {
            return stream->closed || !stream->pending.empty();
        })) {
        return std::vector<Notification>{};
    }
    if (stream->closed) {
        return std::unexpected(make_error(Error::Code::InvalidPermissions, "session closed"));
    }
    std::size_t batch = std::min<std::size_t>(max_batch, stream->pending.size());
    std::vector<Notification> notifications;
    notifications.reserve(batch);
    for (std::size_t index = 0; index < batch; ++index) {
        notifications.push_back(stream->pending.front());
        stream->pending.pop_front();
    }
    auto pending = stream->pending.size();
    auto now     = std::chrono::steady_clock::now();
    if (stream->throttled && now >= stream->throttle_until
        && pending < kNotificationThrottleThreshold) {
        stream->throttled = false;
        publish_metric(options_, stream->alias, "server/notifications/throttled", 0);
        publish_metric(options_, stream->alias, "server/notifications/retry_after_ms", 0);
    }
    publish_metric(options_, stream->alias, "server/notifications/pending",
                   static_cast<std::int64_t>(pending));
    stream_lock.unlock();
    return notifications;
}

auto RemoteMountServer::handleRead(ReadRequest const& request) -> Expected<ReadResponse> {
    expireSessions();
    Session session;
    {
        std::lock_guard lock(sessions_mutex_);
        auto            it = sessions_.find(request.session_id);
        if (it == sessions_.end()) {
            return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
        }
        session = it->second;
    }
    auto export_it = exports_.find(session.alias);
    if (export_it == exports_.end()) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown alias"));
    }
    auto canonical_path = canonicalize_path(request.path);
    if (!canonical_path) {
        return std::unexpected(canonical_path.error());
    }
    if (!path_within(*canonical_path, export_it->second.canonical_root)) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path outside export"));
    }

    applyRequestThrottle(session, export_it->second);

    applyRequestThrottle(session, export_it->second);

    auto* space = export_it->second.space;
    if (!space) {
        return std::unexpected(make_error(Error::Code::UnknownError, "export not available"));
    }

    struct SnapshotResult {
        bool                        exists = false;
        bool                        deleted = false;
        std::optional<ValuePayload> value;
    } snapshot;

    VisitOptions options;
    options.root          = *canonical_path;
    options.maxDepth      = 1;
    options.includeValues = request.include_value;

    auto want_value = request.include_value;
    std::optional<Error> encode_error;
    auto visitor = [&](PathEntry const& entry, ValueHandle& handle) {
        if (entry.path != *canonical_path) {
            return VisitControl::Continue;
        }
        snapshot.exists = true;
        if (want_value) {
            auto serialized = VisitDetail::Access::SerializeNodeData(handle);
            if (serialized && !serialized->empty()) {
                auto payload = encode_snapshot_value(*serialized, request.type_name);
                if (!payload) {
                    encode_error = payload.error();
                    return VisitControl::Stop;
                }
                snapshot.value = std::move(*payload);
            }
        }
        return VisitControl::Stop;
    };

    auto visit_result = space->visit(visitor, options);
    if (encode_error) {
        return std::unexpected(*encode_error);
    }
    if (!visit_result) {
        if (visit_result.error().code == Error::Code::NoSuchPath) {
            snapshot.deleted = true;
        } else {
            return std::unexpected(visit_result.error());
        }
    }

    ReadResponse response;
    response.request_id        = request.request_id;
    response.path              = *canonical_path;
    response.children_included = request.include_children;

    {
        std::lock_guard version_lock(version_mutex_);
        auto& version = path_versions_[*canonical_path];
        if (version == 0) {
            version = 1;
        }
        response.version = version;
        if (request.consistency && request.consistency->mode == ReadConsistencyMode::AtLeastVersion
            && request.consistency->at_least_version
            && response.version < *request.consistency->at_least_version) {
            response.error = error_payload("consistency_not_met",
                                           "requested version not yet available",
                                           true);
            return response;
        }
    }

    if (request.include_children) {
        ConcretePathStringView view{response.path};
        auto children = space->listChildren(view);
        response.children = std::move(children);
    }

    if (!snapshot.exists && !snapshot.deleted) {
        response.error = error_payload("not_found", "path missing", false);
        return response;
    }

    if (snapshot.value) {
        response.value = snapshot.value;
    }
    if (snapshot.deleted) {
        response.error = error_payload("deleted", "path has been removed", false);
    }

    return response;
}

auto RemoteMountServer::handleInsert(InsertRequest const& request) -> Expected<InsertResponse> {
    expireSessions();
    Session session;
    {
        std::lock_guard lock(sessions_mutex_);
        auto            it = sessions_.find(request.session_id);
        if (it == sessions_.end()) {
            return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
        }
        session = it->second;
    }

    auto export_it = exports_.find(session.alias);
    if (export_it == exports_.end()) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown alias"));
    }

    auto canonical_path = canonicalize_path(request.path);
    if (!canonical_path) {
        return std::unexpected(canonical_path.error());
    }
    if (!path_within(*canonical_path, export_it->second.canonical_root)) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path outside export"));
    }

    if (std::find(session.capabilities.begin(), session.capabilities.end(), kCapInsert)
        == session.capabilities.end()) {
        return std::unexpected(make_error(Error::Code::InvalidPermissions, "insert not permitted"));
    }

    applyRequestThrottle(session, export_it->second);

    auto* space = export_it->second.space;
    if (!space) {
        return std::unexpected(make_error(Error::Code::UnknownError, "export not available"));
    }

    auto legacy_not_allowed = [&]() -> Expected<InsertResponse> {
        return std::unexpected(make_error(
            Error::Code::InvalidType,
            "Legacy remote payload encodings are disabled (set PATHSPACE_REMOTE_TYPED_PAYLOADS=0 to re-enable temporarily)"));
    };

    if (request.value.encoding == kEncodingVoid) {
        InsertResponse response;
        response.request_id      = request.request_id;
        response.success         = true;
        response.tasks_inserted  = 1;
        response.values_inserted = 0;
        response.spaces_inserted = 0;
        return response;
    }

    InsertReturn insert_ret;
    if (request.value.encoding == kEncodingString) {
        if (!allowLegacyPayloads(payload_mode_)) {
            return legacy_not_allowed();
        }
        auto decoded = decode_base64(request.value.data);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::string value(decoded->begin(), decoded->end());
        insert_ret = space->insert(*canonical_path, value);
    } else if (request.value.encoding == kEncodingTypedSlidingBuffer) {
        auto const& payload_type = request.value.type_name.empty() ? request.type_name : request.value.type_name;
        if (payload_type.empty()) {
            return std::unexpected(make_error(Error::Code::InvalidType, "typed payload missing type name"));
        }
        auto decoded = decode_base64(request.value.data);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto raw_span = std::span<const std::uint8_t>(decoded->data(), decoded->size());
        auto bytes    = std::as_bytes(raw_span);
        auto inserted = insertTypedPayloadFromBytes(*space, *canonical_path, payload_type, bytes);
        if (!inserted) {
            return std::unexpected(inserted.error());
        }
        insert_ret = *inserted;
    } else {
        return std::unexpected(
            make_error(Error::Code::InvalidType, "unsupported remote payload encoding"));
    }

    InsertResponse response;
    response.request_id      = request.request_id;
    response.success         = insert_ret.errors.empty();
    response.values_inserted = insert_ret.nbrValuesInserted;
    response.spaces_inserted = insert_ret.nbrSpacesInserted;
    response.tasks_inserted  = insert_ret.nbrTasksInserted;
    if (!insert_ret.errors.empty()) {
        response.error = error_payload("insert_failed", summarize_error(insert_ret.errors.front()));
    }
    return response;
}

auto RemoteMountServer::handleTake(TakeRequest const& request) -> Expected<TakeResponse> {
    expireSessions();
    Session session;
    {
        std::lock_guard lock(sessions_mutex_);
        auto            it = sessions_.find(request.session_id);
        if (it == sessions_.end()) {
            return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
        }
        session = it->second;
    }

    auto export_it = exports_.find(session.alias);
    if (export_it == exports_.end()) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown alias"));
    }

    auto canonical_path = canonicalize_path(request.path);
    if (!canonical_path) {
        return std::unexpected(canonical_path.error());
    }
    if (!path_within(*canonical_path, export_it->second.canonical_root)) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path outside export"));
    }

    if (std::find(session.capabilities.begin(), session.capabilities.end(), kCapTake)
        == session.capabilities.end()) {
        return std::unexpected(make_error(Error::Code::InvalidPermissions, "take not permitted"));
    }

    applyRequestThrottle(session, export_it->second);

    auto* space = export_it->second.space;
    if (!space) {
        return std::unexpected(make_error(Error::Code::UnknownError, "export not available"));
    }

    TakeResponse response;
    response.request_id = request.request_id;
    if (!request.type_name || request.type_name->empty()) {
        response.success = false;
        response.error   = error_payload("type_required", "type_name is required", false);
        return response;
    }
    auto batch_size = std::clamp<std::uint32_t>(request.max_items == 0 ? 1U : request.max_items,
                                                1U,
                                                kMaxTakeBatch);
    auto const& type_name = *request.type_name;

    for (std::uint32_t index = 0; index < batch_size; ++index) {
        Out options;
        if (index == 0 && request.do_block) {
            options = options & Block{request.timeout};
        }
        auto result = takeTypedPayloadToBytes(*space, *canonical_path, type_name, options);
        if (!result) {
            auto const& error = result.error();
            bool        exhausted = (error.code == Error::Code::NoObjectFound
                          || error.code == Error::Code::NoSuchPath);
            if (response.values.empty() || !exhausted) {
                response.success = false;
                response.error   = error_payload("take_failed", summarize_error(error));
                return response;
            }
            break;
        }

        ValuePayload payload;
        payload.encoding  = std::string{kEncodingTypedSlidingBuffer};
        payload.type_name = type_name;
        auto bytes        = std::span<const std::byte>(result->data(), result->size());
        payload.data      = encode_base64(bytes);
        response.values.push_back(std::move(payload));
    }

    if (response.values.empty()) {
        response.success = false;
        response.error   = error_payload("take_failed", "no values available", false);
        return response;
    }

    response.success = true;
    return response;
}

auto RemoteMountServer::handleWaitSubscribe(WaitSubscriptionRequest const& request)
    -> Expected<WaitSubscriptionAck> {
    ensureSinksAttached();
    expireSessions();

    Session session;
    {
        std::lock_guard lock(sessions_mutex_);
        auto            it = sessions_.find(request.session_id);
        if (it == sessions_.end()) {
            return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown session"));
        }
        session = it->second;
    }

    auto export_it = exports_.find(session.alias);
    if (export_it == exports_.end()) {
        return std::unexpected(make_error(Error::Code::NoSuchPath, "unknown alias"));
    }

    auto canonical_path = canonicalize_path(request.path);
    if (!canonical_path) {
        return std::unexpected(canonical_path.error());
    }
    if (!path_within(*canonical_path, export_it->second.canonical_root)) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path outside export"));
    }

    if (auto stream = findSessionStream(session.session_id)) {
        std::chrono::milliseconds retry_after{0};
        {
            std::lock_guard<std::mutex> stream_lock(stream->mutex);
            if (stream->throttled) {
                auto now = std::chrono::steady_clock::now();
                if (now < stream->throttle_until) {
                    retry_after = std::chrono::duration_cast<std::chrono::milliseconds>(
                        stream->throttle_until - now);
                }
            }
        }
        if (retry_after.count() > 0) {
            WaitSubscriptionAck ack;
            ack.subscription_id = request.subscription_id;
            ack.accepted        = false;
            ack.error           = error_payload("notify_backpressure",
                                      "notification backlog high",
                                      true);
            ack.error->retry_after = retry_after;
            publish_metric(options_, session.alias, "server/notifications/throttled", 1);
            publish_metric(options_, session.alias, "server/notifications/retry_after_ms",
                           static_cast<std::int64_t>(retry_after.count()));
            return ack;
        }
    }

    std::chrono::milliseconds waiter_retry_after{0};
    if (!reserveWaiter(session, export_it->second, waiter_retry_after)) {
        WaitSubscriptionAck ack;
        ack.subscription_id = request.subscription_id;
        ack.accepted        = false;
        ack.error           = error_payload("too_many_waiters",
                                  "session exceeded waiter limit",
                                  true);
        ack.error->retry_after = waiter_retry_after;
        return ack;
    }

    {
        std::lock_guard lock(subscriptions_mutex_);
        if (subscriptions_.contains(request.subscription_id)) {
            releaseWaiter(session.throttle);
            return std::unexpected(make_error(Error::Code::InvalidPath,
                                              "duplicate subscription"));
        }
    }

    Subscription subscription;
    subscription.subscription_id  = request.subscription_id;
    subscription.session_id       = session.session_id;
    subscription.alias            = session.alias;
    subscription.path             = *canonical_path;
    subscription.include_value    = request.include_value;
    subscription.include_children = request.include_children;
    if (request.after_version) {
        subscription.min_version = request.after_version;
    }
    subscription.throttle = session.throttle;

    {
        std::lock_guard lock(subscriptions_mutex_);
        subscriptions_.emplace(subscription.subscription_id, subscription);
    }

    {
        std::lock_guard lock(metrics_mutex_);
        ++export_it->second.waiter_count;
        publish_metric(options_, session.alias, "waiters/current", export_it->second.waiter_count);
    }

    WaitSubscriptionAck ack;
    ack.subscription_id = request.subscription_id;
    ack.accepted        = true;
    return ack;
}

auto RemoteMountServer::nextNotification(std::string const& subscription_id)
    -> std::optional<Notification> {
    std::lock_guard lock(subscriptions_mutex_);
    auto            it = subscriptions_.find(subscription_id);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    if (it->second.pending.empty()) {
        return std::nullopt;
    }
    auto notification = it->second.pending.front();
    it->second.pending.pop_front();
    return notification;
}

void RemoteMountServer::handleLocalNotification(std::string const& alias,
                                                 std::string const& absolute_path) {
    auto export_it = exports_.find(alias);
    if (export_it == exports_.end()) {
        return;
    }
    if (!path_within(absolute_path, export_it->second.canonical_root)) {
        return;
    }

    std::vector<std::string> targets;
    bool                     needs_value = false;
    {
        std::lock_guard lock(subscriptions_mutex_);
        for (auto const& [id, sub] : subscriptions_) {
            if (sub.alias == alias && sub.path == absolute_path) {
                targets.push_back(id);
                needs_value = needs_value || sub.include_value;
            }
        }
    }
    if (targets.empty()) {
        return;
    }

    std::optional<NodeData>     cached_value;
    std::optional<ValuePayload> payload;
    std::optional<std::string>  cached_type_name;
    bool                        deleted = false;
    {
        VisitOptions options;
        options.root          = absolute_path;
        options.maxDepth      = 1;
        options.includeValues = true;
        auto space = export_it->second.space;
        if (space) {
            std::optional<Error> encode_error;
            auto visitor = [&](PathEntry const& entry, ValueHandle& handle) {
                if (entry.path == absolute_path) {
                    auto serialized = VisitDetail::Access::SerializeNodeData(handle);
                    if (serialized && !serialized->empty()) {
                        auto node = snapshot_front_node(*serialized);
                        if (!node) {
                            encode_error = node.error();
                            return VisitControl::Stop;
                        }
                        cached_type_name = front_type_name(*node);
                        cached_value     = std::move(*node);
                    }
                    return VisitControl::Stop;
                }
                return VisitControl::Continue;
            };
            auto visit_result = space->visit(visitor, options);
            if (encode_error) {
                return;
            }
            if (!visit_result && visit_result.error().code == Error::Code::NoSuchPath) {
                deleted = true;
            }
        }
    }
    if (needs_value && cached_value) {
        auto encoded = encode_node_value(*cached_value, std::nullopt);
        if (!encoded) {
            return;
        }
        payload = std::move(*encoded);
        cached_type_name = payload->type_name;
    }

    std::uint64_t version = 0;
    {
        std::lock_guard version_lock(version_mutex_);
        auto& entry = path_versions_[absolute_path];
        version     = ++entry;
    }

    for (auto const& id : targets) {
        std::lock_guard lock(subscriptions_mutex_);
        auto            it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            continue;
        }
        if (it->second.min_version && version <= *it->second.min_version) {
            continue;
        }
        Notification notification;
        notification.subscription_id = id;
        notification.path            = absolute_path;
        notification.version         = version;
        notification.deleted         = deleted;
        if (it->second.include_value && payload) {
            notification.value = payload;
        }
        if (payload) {
            notification.type_name = payload->type_name;
        } else if (cached_type_name) {
            notification.type_name = cached_type_name;
        }
        it->second.pending.push_back(notification);
        it->second.min_version = version;
        enqueueSessionNotification(it->second.session_id, notification);
    }
}

auto RemoteMountServer::findSessionStream(std::string const& session_id)
    -> std::shared_ptr<SessionStream> {
    std::lock_guard<std::mutex> lock(session_streams_mutex_);
    auto                        it = session_streams_.find(session_id);
    if (it == session_streams_.end()) {
        return nullptr;
    }
    return it->second;
}

void RemoteMountServer::enqueueSessionNotification(std::string const& session_id,
                                                   Notification const& notification) {
    auto stream = findSessionStream(session_id);
    if (!stream) {
        return;
    }
    std::unique_lock<std::mutex> stream_lock(stream->mutex);
    if (stream->closed) {
        return;
    }
    stream->pending.push_back(notification);
    auto pending = stream->pending.size();
    if (pending > kNotificationMaxQueue) {
        auto overflow = pending - kNotificationMaxQueue;
        stream->dropped += overflow;
        while (stream->pending.size() > kNotificationMaxQueue) {
            stream->pending.pop_front();
        }
        pending = stream->pending.size();
    }
    auto now = std::chrono::steady_clock::now();
    if (pending >= kNotificationThrottleThreshold) {
        stream->throttled      = true;
        stream->throttle_until = now + kNotificationThrottleWindow;
        publish_metric(options_, stream->alias, "server/notifications/throttled", 1);
        publish_metric(options_, stream->alias, "server/notifications/retry_after_ms",
                       static_cast<std::int64_t>(kNotificationThrottleWindow.count()));
    }
    publish_metric(options_, stream->alias, "server/notifications/pending",
                   static_cast<std::int64_t>(pending));
    publish_metric(options_, stream->alias, "server/notifications/dropped",
                   static_cast<std::int64_t>(stream->dropped));
    stream_lock.unlock();
    stream->cv.notify_one();
}

void RemoteMountServer::closeSessionStream(std::string const& session_id) {
    std::shared_ptr<SessionStream> stream;
    {
        std::lock_guard<std::mutex> lock(session_streams_mutex_);
        auto                        it = session_streams_.find(session_id);
        if (it == session_streams_.end()) {
            return;
        }
        stream = it->second;
        session_streams_.erase(it);
    }
    if (!stream) {
        return;
    }
    {
        std::lock_guard<std::mutex> stream_lock(stream->mutex);
        stream->closed = true;
    }
    stream->cv.notify_all();
}

void RemoteMountServer::applyRequestThrottle(Session const& session,
                                             ExportEntry&   export_entry) const {
    auto throttle = session.throttle;
    if (!throttle) {
        return;
    }
    auto const& options = throttle->options;
    if (!options.enabled || options.max_requests_per_window == 0
        || options.request_window.count() <= 0) {
        return;
    }

    auto per_request = options.request_window / options.max_requests_per_window;
    if (per_request.count() <= 0) {
        per_request = std::chrono::milliseconds{1};
    }

    auto now       = std::chrono::steady_clock::now();
    auto wake_time = now;
    {
        std::lock_guard lock(throttle->mutex);
        if (throttle->next_allowed <= now) {
            throttle->next_allowed = now + per_request;
            return;
        }
        wake_time               = throttle->next_allowed;
        throttle->next_allowed += per_request;
    }

    auto sleep_duration = std::chrono::duration_cast<std::chrono::milliseconds>(wake_time - now);
    auto penalty        = options.penalty_increment;
    if (penalty.count() > 0 && sleep_duration < penalty) {
        sleep_duration = penalty;
    }
    auto max_delay = options.penalty_cap;
    if (max_delay.count() > 0 && sleep_duration > max_delay) {
        sleep_duration = max_delay;
    }
    if (sleep_duration.count() <= 0) {
        return;
    }
    std::this_thread::sleep_for(sleep_duration);
    publish_metric(options_, session.alias, "server/throttle/last_sleep_ms",
                   static_cast<std::int64_t>(sleep_duration.count()));
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        ++export_entry.throttle_hits;
        publish_metric(options_, session.alias, "server/throttle/hits_total",
                       export_entry.throttle_hits);
    }
}

bool RemoteMountServer::reserveWaiter(Session const& session,
                                      ExportEntry&   export_entry,
                                      std::chrono::milliseconds& retry_after) {
    retry_after = std::chrono::milliseconds{0};
    auto throttle = session.throttle;
    if (!throttle) {
        return true;
    }
    auto const& options = throttle->options;
    if (options.max_waiters_per_session == 0) {
        return true;
    }

    {
        std::lock_guard lock(throttle->mutex);
        if (throttle->active_waiters < options.max_waiters_per_session) {
            ++throttle->active_waiters;
            return true;
        }
    }

    retry_after = options.wait_retry_after.count() > 0 ? options.wait_retry_after
                                                       : std::chrono::milliseconds{250};
    {
        std::lock_guard metrics_lock(metrics_mutex_);
        ++export_entry.waiter_rejections;
        publish_metric(options_, session.alias, "server/throttle/waiters_rejected",
                       export_entry.waiter_rejections);
        publish_metric(options_, session.alias, "server/throttle/retry_after_ms",
                       static_cast<std::int64_t>(retry_after.count()));
    }
    return false;
}

void RemoteMountServer::releaseWaiter(std::weak_ptr<SessionThrottleState> throttle) const {
    if (auto state = throttle.lock()) {
        std::lock_guard lock(state->mutex);
        if (state->active_waiters > 0) {
            --state->active_waiters;
        }
    }
}

} // namespace SP::Distributed
