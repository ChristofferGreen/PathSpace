#include "RemoteMountManager.hpp"

#include "core/InsertReturn.hpp"
#include "core/NodeData.hpp"
#include "distributed/RemoteExecutionRegistry.hpp"
#include "distributed/TypedPayloadBridge.hpp"
#include "log/TaggedLogger.hpp"
#include "path/Iterator.hpp"
#include "type/DataCategory.hpp"
#include "type/InputMetadataT.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace SP::Distributed {
namespace {

constexpr std::chrono::milliseconds kDefaultHeartbeat{2500};
constexpr std::chrono::milliseconds kNotificationPoll{25};
constexpr std::chrono::milliseconds kNotificationStreamTimeout{250};
constexpr std::size_t               kNotificationBatch{32};
constexpr std::uint32_t             kMaxTakeBatch{64};

auto make_error(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

auto validate_alias(std::string const& alias) -> Expected<void> {
    if (alias.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "alias must not be empty"));
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

auto normalize_absolute_path(std::string path) -> Expected<std::string> {
    if (path.empty()) {
        path = "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (auto validated = validateAbsolutePath(path); !validated.has_value()) {
        return std::unexpected(validated.error());
    }
    return path;
}

auto build_mount_path(std::string const& alias) -> std::string {
    std::string path{"/remote/"};
    path.append(alias);
    return path;
}

auto join_paths(std::string const& root, std::string const& tail) -> std::string {
    if (tail.empty() || tail == "/") {
        return root;
    }
    if (root == "/") {
        if (tail.front() == '/') {
            return tail;
        }
        std::string joined{"/"};
        joined.append(tail);
        return joined;
    }
    std::string joined = root;
    if (joined.back() != '/') {
        joined.push_back('/');
    }
    if (!tail.empty() && tail.front() == '/') {
        joined.append(tail.substr(1));
    } else {
        joined.append(tail);
    }
    return joined;
}

auto substitute_alias_tokens(std::string pattern, std::string const& alias) -> std::string {
    constexpr std::string_view kAliasToken{"{alias}"};
    std::size_t                position = 0;
    while ((position = pattern.find(kAliasToken, position)) != std::string::npos) {
        pattern.replace(position, kAliasToken.size(), alias);
        position += alias.size();
    }
    return pattern;
}

auto decode_base64(std::string_view input) -> Expected<std::vector<std::uint8_t>> {
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
            output.push_back(static_cast<std::uint8_t>(((chunk[1] & 0x0F) << 4) | ((chunk[2] & 0x3C) >> 2)));
            if (chunk[3] >= 0) {
                output.push_back(static_cast<std::uint8_t>(((chunk[2] & 0x03) << 6) | chunk[3]));
            }
        }
    }
    if (output.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "base64 payload empty"));
    }
    return output;
}

auto convert_error_payload(ErrorPayload const& payload) -> Error {
    Error::Code code = Error::Code::UnknownError;
    if (payload.code == "no_such_path") {
        code = Error::Code::NoSuchPath;
    } else if (payload.code == "invalid_credentials" || payload.code == "permission_denied") {
        code = Error::Code::InvalidPermissions;
    } else if (payload.code == "lease_expired") {
        code = Error::Code::Timeout;
    } else if (payload.code == "notify_backpressure") {
        code = Error::Code::CapacityExceeded;
    } else if (payload.code == "too_many_waiters") {
        code = Error::Code::CapacityExceeded;
    }
    return make_error(code, payload.message);
}

auto describe_remote_error(Error const& error) -> std::string {
    return describeError(error);
}

struct PendingWaiter {
    std::mutex                    mutex;
    std::condition_variable       cv;
    std::optional<Notification>   notification;
    std::optional<Error>          error;
    bool                          completed{false};
};

} // namespace

Expected<ValuePayload> RemoteMountManager::materializeExecutionPayload(InputData const& data) const {
    if (!data.metadata.typeInfo) {
        return std::unexpected(make_error(Error::Code::InvalidType,
                                          "Remote execution inserts require concrete return types"));
    }

    auto const& type = *data.metadata.typeInfo;

    auto wait_for_completion = [](NodeData& exec_node) {
        if (auto future = exec_node.peekFuture(); future.has_value()) {
            future->wait();
        } else if (auto any_future = exec_node.peekAnyFuture(); any_future.has_value()) {
            any_future->wait();
        }
    };

    if (type == typeid(void)) {
        NodeData exec_node(data);
        wait_for_completion(exec_node);
        ValuePayload payload;
        payload.encoding = std::string{kEncodingVoid};
        payload.type_name = type.name();
        payload.data.clear();
        return payload;
    }

    if (type == typeid(bool)) {
        return EncodeExecutionValue<bool>(data);
    }
    if (type == typeid(int) || type == typeid(std::int32_t)) {
        return EncodeExecutionValue<std::int32_t>(data);
    }
    if (type == typeid(unsigned int) || type == typeid(std::uint32_t)) {
        return EncodeExecutionValue<std::uint32_t>(data);
    }
    if (type == typeid(long) || type == typeid(std::int64_t) || type == typeid(long long)) {
        return EncodeExecutionValue<std::int64_t>(data);
    }
    if (type == typeid(unsigned long) || type == typeid(std::uint64_t)
        || type == typeid(unsigned long long)) {
        return EncodeExecutionValue<std::uint64_t>(data);
    }
    if (type == typeid(float)) {
        return EncodeExecutionValue<float>(data);
    }
    if (type == typeid(double)) {
        return EncodeExecutionValue<double>(data);
    }

    if (auto encoder = RemoteExecutionEncoderRegistry::instance().find(std::type_index(type));
        encoder.has_value()) {
        return (*encoder)(data);
    }

    return std::unexpected(make_error(
        Error::Code::InvalidType,
        "Remote execution inserts currently support std::string, bool, numeric, or registered result types"));
}

struct RemoteMountManager::MirrorAssignment {
    RemoteMountClientOptions::MirrorMode   mode{RemoteMountClientOptions::MirrorMode::TreeSnapshot};
    RemoteMountClientOptions::MirrorTarget target{RemoteMountClientOptions::MirrorTarget::RootSpace};
    PathSpace*                             target_space{nullptr};
    std::string                            remote_root;
    std::string                            local_root;
    std::size_t                            max_depth{VisitOptions::kUnlimitedDepth};
    std::size_t                            max_children{VisitOptions::kDefaultMaxChildren};
    std::size_t                            max_nodes{256};
    std::chrono::milliseconds              interval{std::chrono::milliseconds{500}};
    std::string                            last_child;
    std::chrono::steady_clock::time_point  next_run;
};

struct RemoteMountManager::MountState {
    RemoteMountClientOptions            options;
    std::string                         normalized_export_root;
    std::string                         mount_path;
    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    std::chrono::milliseconds           heartbeat_interval{kDefaultHeartbeat};
    std::chrono::system_clock::time_point lease_deadline{};
    std::atomic<bool>                   stop_requested{false};
    std::thread                         heartbeat_thread;
    std::thread                         notification_thread;
    RemoteMountStatus                   status;
    std::uint64_t                       total_latency_ms{0};
    std::uint64_t                       heartbeat_sequence{0};
    RemoteMountSpace*                   space{nullptr};
    std::mutex                          session_mutex;
    std::unordered_map<std::string, std::deque<ValuePayload>> cached_takes;
    std::mutex                          cache_mutex;
    std::mutex                          waiters_mutex;
    std::unordered_map<std::string, std::shared_ptr<PendingWaiter>> waiters;
    std::vector<MirrorAssignment>       mirrors;
    std::thread                         mirror_thread;
};

class RemoteMountManager::RemoteMountSpace final : public PathSpaceBase {
public:
    RemoteMountSpace(RemoteMountManager* manager, MountState* state)
        : manager_(manager)
        , state_(state) {}

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        if (!manager_ || !state_) {
            InsertReturn ret;
            ret.errors.emplace_back(
                make_error(Error::Code::InvalidPermissions, "Remote mount unavailable"));
            return ret;
        }
        auto relative = relativePath(path);
        return manager_->performInsert(*state_, relative, data);
    }

    auto out(Iterator const&       path,
             InputMetadata const&  metadata,
             Out const&            options,
             void*                 obj) -> std::optional<Error> override;

    void notify(std::string const&) override {}

    auto visit(PathVisitor const&, VisitOptions const&) -> Expected<void> override {
        return std::unexpected(
            make_error(Error::Code::NotSupported, "Remote mounts do not support visit() yet"));
    }

    void shutdown() override { state_ = nullptr; }

private:
    [[nodiscard]] static auto relativePath(Iterator iterator) -> std::string {
        if (iterator.isAtEnd()) {
            return std::string{"/"};
        }
        std::string result;
        while (true) {
            auto component = iterator.currentComponent();
            if (!component.empty()) {
                result.push_back('/');
                result.append(component);
            }
            if (iterator.isAtFinalComponent()) {
                break;
            }
            iterator = iterator.next();
        }
        if (result.empty()) {
            result = "/";
        }
        return result;
    }

    RemoteMountManager* manager_{nullptr};
    MountState*         state_{nullptr};
};

RemoteMountManager::RemoteMountManager(RemoteMountManagerOptions options,
                                       std::shared_ptr<RemoteMountSessionFactory> factory)
    : options_(std::move(options))
    , factory_(std::move(factory)) {
    if (!options_.payload_compatibility.has_value()) {
        options_.payload_compatibility = defaultRemotePayloadCompatibility();
    }
    payload_mode_ = *options_.payload_compatibility;
    if (allowLegacyPayloads(payload_mode_)) {
        sp_log(
            "RemoteMountManager allowing legacy remote payload decoding (set PATHSPACE_REMOTE_TYPED_PAYLOADS=1 to disable)",
            "RemoteMountManager");
    }
}

RemoteMountManager::~RemoteMountManager() {
    stop();
}

void RemoteMountManager::start() {
    if (!options_.root_space || !factory_) {
        sp_log("RemoteMountManager start skipped (missing root space or factory)", "RemoteMountManager");
        return;
    }
    if (running_.exchange(true)) {
        return;
    }

    mounts_.clear();
    mounts_.reserve(options_.mounts.size());

    for (auto const& mount : options_.mounts) {
        auto state = std::make_unique<MountState>();
        state->options = mount;
        state->status.alias = mount.alias;

        if (auto alias_ok = validate_alias(mount.alias); !alias_ok.has_value()) {
            state->status.message = describe_remote_error(alias_ok.error());
            mounts_.push_back(std::move(state));
            continue;
        }

        auto normalized_root = normalize_absolute_path(mount.export_root);
        if (!normalized_root) {
            state->status.message = describe_remote_error(normalized_root.error());
            mounts_.push_back(std::move(state));
            continue;
        }
        state->normalized_export_root = std::move(*normalized_root);

        if (!mount.mount_path.empty()) {
            auto normalized_mount = normalize_absolute_path(mount.mount_path);
            if (!normalized_mount) {
                state->status.message = describe_remote_error(normalized_mount.error());
                mounts_.push_back(std::move(state));
                continue;
            }
            state->mount_path = std::move(*normalized_mount);
        } else {
            state->mount_path = build_mount_path(mount.alias);
        }

        auto remote_space = std::make_unique<RemoteMountSpace>(this, state.get());
        state->space      = remote_space.get();
        auto inserted     = options_.root_space->insert(state->mount_path, std::move(remote_space));
        if (!inserted.errors.empty()) {
            state->status.message = describe_remote_error(inserted.errors.front());
            state->space          = nullptr;
        }

        configureMirrors(*state);

        mounts_.push_back(std::move(state));
    }

    for (auto& state : mounts_) {
        if (state->space == nullptr) {
            continue;
        }
        auto ensure = ensureSession(*state);
        if (!ensure.has_value()) {
            state->status.message = describe_remote_error(ensure.error());
        }
        startMirrorThread(*state);
        startNotificationThread(*state);
    }
}

void RemoteMountManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    for (auto& state : mounts_) {
        state->stop_requested = true;
        if (state->heartbeat_thread.joinable()) {
            state->heartbeat_thread.join();
        }
        stopMirrorThread(*state);
        stopNotificationThread(*state);
        {
            std::lock_guard<std::mutex> wait_lk(state->waiters_mutex);
            for (auto& [_, waiter] : state->waiters) {
                if (!waiter) {
                    continue;
                }
                std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
                waiter->error     = make_error(Error::Code::Timeout, "Remote mount stopping");
                waiter->completed = true;
                waiter->cv.notify_all();
            }
            state->waiters.clear();
        }
        {
            std::lock_guard<std::mutex> lk(state->session_mutex);
            state->session.reset();
            state->session_id.clear();
        }
        state->status.connected = false;
        if (state->space) {
            state->space->shutdown();
            state->space = nullptr;
        }
        {
            std::lock_guard<std::mutex> cache_lk(state->cache_mutex);
            state->cached_takes.clear();
        }
    }
}

auto RemoteMountManager::statuses() const -> std::vector<RemoteMountStatus> {
    std::vector<RemoteMountStatus> status_list;
    status_list.reserve(mounts_.size());
    for (auto const& state : mounts_) {
        status_list.push_back(state->status);
    }
    return status_list;
}

bool RemoteMountManager::running() const {
    return running_.load();
}

auto RemoteMountManager::RemoteMountSpace::out(Iterator const&       path,
                                               InputMetadata const&  metadata,
                                               Out const&            options,
                                               void*                 obj)
    -> std::optional<Error> {
    if (!manager_ || !state_) {
        return make_error(Error::Code::InvalidPermissions, "Remote mount unavailable");
    }

    auto relative = relativePath(path);
    if (options.doPop) {
        return manager_->performTake(*state_, relative, metadata, options, obj);
    }
    if (options.doBlock) {
        return manager_->performWait(*state_, relative, metadata, options, obj);
    }
    return manager_->performRead(*state_, relative, metadata, options, obj);
}

[[nodiscard]] Expected<void> RemoteMountManager::ensureSession(MountState& state) {
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        if (state.session && state.status.connected) {
            return {};
        }
    }
    return openSession(state);
}

[[nodiscard]] Expected<void> RemoteMountManager::openSession(MountState& state) {
    if (!factory_) {
        return std::unexpected(make_error(Error::Code::InvalidPermissions,
                                          "Remote session factory unavailable"));
    }

    auto sessionExpected = factory_->create(state.options);
    if (!sessionExpected.has_value()) {
        return std::unexpected(sessionExpected.error());
    }
    auto session = sessionExpected.value();

    MountOpenRequest request;
    request.version      = ProtocolVersion{1, 0};
    request.request_id   = std::string{"open-"} + std::to_string(request_counter_.fetch_add(1));
    request.client_id    = state.options.client_id.empty() ? std::string{"pathspace-client"}
                                                          : state.options.client_id;
    request.alias        = state.options.alias;
    request.export_root  = state.normalized_export_root;
    request.capabilities = state.options.capabilities;
    request.auth         = state.options.auth;

    auto response = session->open(request);
    if (!response.has_value()) {
        return std::unexpected(response.error());
    }
    if (!response->accepted) {
        if (response->error) {
            return std::unexpected(convert_error_payload(*response->error));
        }
        return std::unexpected(make_error(Error::Code::InvalidPermissions, "Mount rejected"));
    }

    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        state.session     = std::move(session);
        state.session_id  = response->session_id;
        state.status.connected = true;
        state.status.message.clear();
        state.heartbeat_interval = response->heartbeat_interval.count() > 0
                                       ? response->heartbeat_interval
                                       : kDefaultHeartbeat;
        state.lease_deadline = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(response->lease_expires_ms));
    }

    if (!state.stop_requested && !state.heartbeat_thread.joinable()) {
        state.heartbeat_thread = std::thread([this, &state]() { heartbeatLoop(&state); });
    }

    return {};
}

void RemoteMountManager::heartbeatLoop(MountState* state) {
    while (!state->stop_requested.load()) {
        auto interval = state->heartbeat_interval;
        if (interval.count() <= 0) {
            interval = kDefaultHeartbeat;
        }
        auto sleep_chunks = interval / kNotificationPoll;
        for (std::chrono::milliseconds elapsed{0}; elapsed < interval;
             elapsed += kNotificationPoll) {
            if (state->stop_requested.load()) {
                return;
            }
            std::this_thread::sleep_for(kNotificationPoll);
        }

        if (state->stop_requested.load()) {
            break;
        }

        auto result = sendHeartbeat(*state);
        if (result.has_value()) {
            state->status.message = describe_remote_error(*result);
            state->status.connected = false;
            std::lock_guard<std::mutex> lk(state->session_mutex);
            state->session.reset();
            state->session_id.clear();
        }
    }
}

std::optional<Error> RemoteMountManager::sendHeartbeat(MountState& state) {
    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        return ensure.error();
    }

    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session) {
        return make_error(Error::Code::InvalidPermissions, "Remote mount not connected");
    }

    Heartbeat heartbeat;
    heartbeat.session_id = session_id;
    heartbeat.sequence   = ++state.heartbeat_sequence;

    auto hb = session->heartbeat(heartbeat);
    if (!hb.has_value()) {
        return hb.error();
    }
    state.status.connected = true;
    return std::nullopt;
}

void RemoteMountManager::publishMetrics(MountState const& state) const {
    if (!options_.metrics_space) {
        return;
    }
    std::string base = options_.metrics_root;
    if (base.empty()) {
        base = "/inspector/metrics/remotes";
    }
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(state.status.alias);

    options_.metrics_space->insert(base + "/client/connected",
                                   state.status.connected ? 1 : 0);
    options_.metrics_space->insert(base + "/client/message", state.status.message);
    options_.metrics_space->insert(base + "/latency/last_ms",
                                   static_cast<std::int64_t>(state.status.last_latency.count()));
    options_.metrics_space->insert(base + "/latency/max_ms",
                                   static_cast<std::int64_t>(state.status.max_latency.count()));
    options_.metrics_space->insert(base + "/latency/avg_ms",
                                   static_cast<std::int64_t>(state.status.average_latency.count()));
    options_.metrics_space->insert(base + "/requests/success",
                                   static_cast<std::int64_t>(state.status.success_count));
    options_.metrics_space->insert(base + "/requests/errors",
                                   static_cast<std::int64_t>(state.status.error_count));
    options_.metrics_space->insert(base + "/waiters/current",
                                   static_cast<std::int64_t>(state.status.waiter_depth));
    options_.metrics_space->insert(base + "/notifications/pending",
                                   static_cast<std::int64_t>(state.status.queued_notifications));
    options_.metrics_space->insert(base + "/notifications/dropped",
                                   static_cast<std::int64_t>(state.status.dropped_notifications));
    options_.metrics_space->insert(base + "/notifications/throttled",
                                   state.status.throttled ? 1 : 0);
    options_.metrics_space->insert(base + "/notifications/retry_after_ms",
                                   static_cast<std::int64_t>(
                                       state.status.retry_after_hint.count()));
}

void RemoteMountManager::recordSuccess(MountState& state, std::chrono::milliseconds latency) {
    state.status.connected     = true;
    state.status.last_latency  = latency;
    state.status.success_count += 1;
    state.total_latency_ms += static_cast<std::uint64_t>(latency.count());
    if (state.status.success_count > 0) {
        auto avg = state.total_latency_ms / state.status.success_count;
        state.status.average_latency = std::chrono::milliseconds{static_cast<std::int64_t>(avg)};
    }
    if (latency > state.status.max_latency) {
        state.status.max_latency = latency;
    }
    state.status.consecutive_errors = 0;
    state.status.message.clear();
    state.status.throttled        = false;
    state.status.retry_after_hint = std::chrono::milliseconds{0};
    publishMetrics(state);
}

void RemoteMountManager::recordError(MountState& state,
                                     Error const&    error,
                                     bool            connection_issue) {
    state.status.error_count += 1;
    state.status.message      = describe_remote_error(error);
    if (connection_issue) {
        state.status.connected = false;
        state.status.consecutive_errors += 1;
        std::lock_guard<std::mutex> lk(state.session_mutex);
        state.session.reset();
        state.session_id.clear();
    }
    publishMetrics(state);
}

std::string RemoteMountManager::buildRemotePath(MountState const& state,
                                                std::string const& relative) const {
    return join_paths(state.normalized_export_root, relative);
}

std::optional<ValuePayload> RemoteMountManager::popCachedValue(MountState& state,
                                                               std::string const& remote_path) {
    std::lock_guard<std::mutex> lk(state.cache_mutex);
    auto                        it = state.cached_takes.find(remote_path);
    if (it == state.cached_takes.end() || it->second.empty()) {
        return std::nullopt;
    }
    ValuePayload payload = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) {
        state.cached_takes.erase(it);
    }
    return payload;
}

void RemoteMountManager::cacheValues(MountState& state,
                                     std::string const& remote_path,
                                     std::span<ValuePayload> payloads) {
    if (payloads.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(state.cache_mutex);
    auto&                       queue = state.cached_takes[remote_path];
    for (auto& payload : payloads) {
        queue.push_back(std::move(payload));
    }
}

std::optional<Error> RemoteMountManager::applyValuePayload(ValuePayload const& payload,
                                                           InputMetadata const& metadata,
                                                           void*               obj) {
    if (!obj) {
        return make_error(Error::Code::InvalidType, "Destination buffer missing");
    }
    auto decoded = decode_base64(payload.data);
    if (!decoded.has_value()) {
        return decoded.error();
    }
    auto legacy_disabled = [&]() -> std::optional<Error> {
        return make_error(
            Error::Code::InvalidType,
            "Legacy remote payload encodings are disabled (set PATHSPACE_REMOTE_TYPED_PAYLOADS=0 to re-enable temporarily)"
        );
    };
    auto legacy_allowed = allowLegacyPayloads(payload_mode_);
    if (payload.encoding == kEncodingString) {
        if (!legacy_allowed) {
            return legacy_disabled();
        }
        auto const& raw = *decoded;
        if (!metadata.typeInfo || *metadata.typeInfo != typeid(std::string)) {
            return make_error(Error::Code::InvalidType,
                              "String payload cannot be applied to non-string destination");
        }
        auto* target = static_cast<std::string*>(obj);
        target->assign(reinterpret_cast<char const*>(raw.data()), raw.size());
        return std::nullopt;
    }
    auto raw_bytes = std::move(*decoded);
    if (payload.encoding == kEncodingTypedSlidingBuffer) {
        if (!metadata.deserialize) {
            return make_error(Error::Code::InvalidType, "Type is not deserializable");
        }
        if (payload.type_name.empty()) {
            return make_error(Error::Code::InvalidType, "Typed payload missing type name");
        }
        if (metadata.typeInfo && payload.type_name != metadata.typeInfo->name()) {
            return make_error(Error::Code::InvalidType, "Typed payload type mismatch");
        }
        SlidingBuffer buffer;
        buffer.assignRaw(std::move(raw_bytes), 0);
        try {
            metadata.deserialize(obj, buffer);
        } catch (...) {
            return make_error(Error::Code::InvalidType, "Typed payload decode failed");
        }
        return std::nullopt;
    }
    if (payload.encoding == kEncodingVoid) {
        return std::nullopt;
    }

    return make_error(Error::Code::InvalidType, "Unsupported remote payload encoding");
}

InsertReturn RemoteMountManager::performInsert(MountState& state,
                                               std::string const& relative,
                                               InputData const& data) {
    InsertReturn ret;
    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        recordError(state, ensure.error(), true);
        ret.errors.push_back(ensure.error());
        return ret;
    }

    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session) {
        ret.errors.emplace_back(
            make_error(Error::Code::InvalidPermissions, "Remote mount unavailable"));
        return ret;
    }

    InsertRequest request;
    request.request_id = std::string{"insert-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id = session_id;
    request.path       = buildRemotePath(state, relative);

    auto const type_info = data.metadata.typeInfo;
    if (type_info) {
        request.type_name = type_info->name();
    }
    if (request.type_name.empty()) {
        ret.errors.emplace_back(
            make_error(Error::Code::InvalidType, "Remote insert missing type metadata"));
        return ret;
    }
    if (data.metadata.dataCategory == DataCategory::Execution) {
        auto payload = materializeExecutionPayload(data);
        if (!payload.has_value()) {
            std::string error_message = describe_remote_error(payload.error());
            if (type_info) {
                sp_log("Remote execution insert failed for type " + std::string(type_info->name())
                           + ": " + error_message,
                       "RemoteMountManager");
            } else {
                sp_log("Remote execution insert failed: " + error_message, "RemoteMountManager");
            }
            ret.errors.emplace_back(payload.error());
            return ret;
        }
        request.value        = std::move(*payload);
        if (request.value.type_name.empty()) {
            request.value.type_name = request.type_name;
        }
        ret.nbrTasksInserted = 1;
    } else {
        if (data.metadata.dataCategory == DataCategory::UniquePtr) {
            ret.errors.emplace_back(make_error(Error::Code::InvalidType,
                                               "Remote mounts cannot serialize nested PathSpaces"));
            return ret;
        }
        NodeData serialized(data);
        if (serialized.hasExecutionPayload()) {
            ret.errors.emplace_back(make_error(Error::Code::InvalidType,
                                               "Execution payloads cannot be forwarded remotely"));
            return ret;
        }
        auto bytes = serialized.frontSerializedValueBytes();
        if (!bytes) {
            ret.errors.emplace_back(make_error(Error::Code::InvalidType,
                                               "Unable to encode remote payload"));
            return ret;
        }
        request.value.encoding  = std::string{kEncodingTypedSlidingBuffer};
        request.value.type_name = request.type_name;
        request.value.data      = detail::Base64Encode(*bytes);
    }

    auto start    = std::chrono::steady_clock::now();
    auto response = session->insert(request);
    if (!response.has_value()) {
        ret.errors.push_back(response.error());
        recordError(state, response.error(), true);
        return ret;
    }
    if (!response->success) {
        if (response->error) {
            auto error = convert_error_payload(*response->error);
            ret.errors.push_back(error);
            recordError(state, error, false);
        } else {
            auto error = make_error(Error::Code::UnknownError, "Remote insert rejected");
            ret.errors.push_back(error);
            recordError(state, error, false);
        }
        return ret;
    }

    ret.nbrValuesInserted = response->values_inserted;
    ret.nbrSpacesInserted = response->spaces_inserted;
    if (ret.nbrTasksInserted == 0) {
        ret.nbrTasksInserted = response->tasks_inserted;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    recordSuccess(state, elapsed);
    return ret;
}

std::optional<Error> RemoteMountManager::performRead(MountState&      state,
                                                     std::string const& relative,
                                                     InputMetadata const& metadata,
                                                     Out const&          options,
                                                     void*               obj) {
    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        recordError(state, ensure.error(), true);
        return ensure.error();
    }

    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session) {
        auto error = make_error(Error::Code::InvalidPermissions, "Remote mount unavailable");
        recordError(state, error, true);
        return error;
    }

    ReadRequest request;
    request.request_id       = std::string{"read-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id       = session_id;
    request.path             = buildRemotePath(state, relative);
    request.include_value    = true;
    request.include_children = false;

    auto start = std::chrono::steady_clock::now();
    auto reply = session->read(request);
    if (!reply.has_value()) {
        recordError(state, reply.error(), true);
        return reply.error();
    }
    if (reply->error) {
        auto error = convert_error_payload(*reply->error);
        recordError(state, error, false);
        return error;
    }
    if (!reply->value.has_value()) {
        auto error = make_error(Error::Code::NoObjectFound, "Remote path has no value");
        recordError(state, error, false);
        return error;
    }

    auto raw = decode_base64(reply->value->data);
    if (!raw.has_value()) {
        recordError(state, raw.error(), false);
        return raw.error();
    }

    auto snapshot_bytes = std::span<const std::uint8_t>(raw->data(), raw->size());
    auto snapshot = NodeData::deserializeSnapshot(std::as_bytes(snapshot_bytes));
    if (!snapshot.has_value()) {
        auto error = make_error(Error::Code::InvalidType, "Failed to decode remote value");
        recordError(state, error, false);
        return error;
    }
    if (auto error = snapshot->deserialize(obj, metadata); error.has_value()) {
        recordError(state, *error, false);
        return error;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    recordSuccess(state, elapsed);
    return std::nullopt;
}

std::optional<Error> RemoteMountManager::performTake(MountState&      state,
                                                     std::string const& relative,
                                                     InputMetadata const& metadata,
                                                     Out const&          options,
                                                     void*               obj) {
    if (!metadata.typeInfo) {
        return make_error(Error::Code::InvalidType,
                          "Remote take requires concrete destination metadata");
    }
    if (!obj) {
        return make_error(Error::Code::InvalidType, "Remote take missing destination buffer");
    }

    auto remote_path = buildRemotePath(state, relative);

    if (auto cached = popCachedValue(state, remote_path)) {
        if (auto error = applyValuePayload(*cached, metadata, obj); error.has_value()) {
            recordError(state, *error, false);
            return error;
        }
        return std::nullopt;
    }

    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        recordError(state, ensure.error(), true);
        return ensure.error();
    }

    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session) {
        auto error = make_error(Error::Code::InvalidPermissions, "Remote mount unavailable");
        recordError(state, error, true);
        return error;
    }

    TakeRequest request;
    request.request_id = std::string{"take-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id = session_id;
    request.path       = remote_path;
    request.type_name  = metadata.typeInfo->name();
    request.do_block   = options.doBlock;
    request.timeout    = options.timeout;
    auto batch         = state.options.take_batch_size == 0 ? 1U : state.options.take_batch_size;
    request.max_items  = std::clamp(batch, 1U, kMaxTakeBatch);

    bool track_waiter = request.do_block;
    if (track_waiter) {
        state.status.waiter_depth += 1;
        if (state.status.waiter_depth > state.status.max_waiter_depth) {
            state.status.max_waiter_depth = state.status.waiter_depth;
        }
        publishMetrics(state);
    }

    auto start    = std::chrono::steady_clock::now();
    auto response = session->take(request);
    if (track_waiter) {
        state.status.waiter_depth -= 1;
        publishMetrics(state);
    }
    if (!response.has_value()) {
        recordError(state, response.error(), true);
        return response.error();
    }
    if (!response->success || response->values.empty()) {
        auto error = response->error ? convert_error_payload(*response->error)
                                     : make_error(Error::Code::UnknownError, "Remote take failed");
        recordError(state, error, false);
        return error;
    }

    auto values = std::move(response->values);
    auto first  = std::move(values.front());
    if (values.size() > 1) {
        cacheValues(state,
                    remote_path,
                    std::span<ValuePayload>(values.data() + 1, values.size() - 1));
    }

    if (auto error = applyValuePayload(first, metadata, obj); error.has_value()) {
        recordError(state, *error, false);
        return error;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    recordSuccess(state, elapsed);
    return std::nullopt;
}

std::optional<Error> RemoteMountManager::performWait(MountState&      state,
                                                     std::string const& relative,
                                                     InputMetadata const& metadata,
                                                     Out const&          options,
                                                     void*               obj) {
    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        recordError(state, ensure.error(), true);
        return ensure.error();
    }

    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session) {
        auto error = make_error(Error::Code::InvalidPermissions, "Remote mount unavailable");
        recordError(state, error, true);
        return error;
    }

    auto start = std::chrono::steady_clock::now();

    state.status.waiter_depth += 1;
    if (state.status.waiter_depth > state.status.max_waiter_depth) {
        state.status.max_waiter_depth = state.status.waiter_depth;
    }
    publishMetrics(state);

    WaitSubscriptionRequest request;
    request.request_id      = std::string{"wait-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id      = session_id;
    request.subscription_id = request.request_id + std::string{"-"} + state.options.alias;
    request.path            = buildRemotePath(state, relative);
    request.include_value   = true;

    auto ack = session->waitSubscribe(request);
    if (!ack.has_value()) {
        state.status.waiter_depth -= 1;
        publishMetrics(state);
        recordError(state, ack.error(), true);
        return ack.error();
    }
    if (!ack->accepted) {
        state.status.waiter_depth -= 1;
        if (ack->error && ack->error->code == "notify_backpressure") {
            state.status.throttled        = true;
            state.status.retry_after_hint = ack->error->retry_after;
        }
        publishMetrics(state);
        if (ack->error) {
            auto error = convert_error_payload(*ack->error);
            recordError(state, error, false);
            return error;
        }
        auto error = make_error(Error::Code::InvalidPermissions, "Remote wait rejected");
        recordError(state, error, false);
        return error;
    }

    auto deadline     = std::chrono::steady_clock::now() + options.timeout;
    bool has_deadline = options.timeout.count() < SP::DEFAULT_TIMEOUT.count();

    auto waiter = std::make_shared<PendingWaiter>();
    {
        std::lock_guard<std::mutex> wait_lk(state.waiters_mutex);
        state.waiters.emplace(request.subscription_id, waiter);
        state.status.queued_notifications = state.waiters.size();
    }
    state.status.throttled        = false;
    state.status.retry_after_hint = std::chrono::milliseconds{0};
    publishMetrics(state);

    auto cleanup_waiter = [&]() {
        std::lock_guard<std::mutex> wait_lk(state.waiters_mutex);
        state.waiters.erase(request.subscription_id);
        state.status.queued_notifications = state.waiters.size();
    };

    auto notification_result = [&]() -> Expected<Notification> {
        std::unique_lock<std::mutex> waiter_lock(waiter->mutex);
        auto predicate = [&]() {
            return waiter->completed || state.stop_requested.load();
        };
        if (has_deadline) {
            while (!predicate()) {
                if (waiter->cv.wait_until(waiter_lock, deadline, predicate)) {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
            }
        } else {
            waiter->cv.wait(waiter_lock, predicate);
        }
        if (state.stop_requested.load()) {
            return std::unexpected(make_error(Error::Code::Timeout, "Remote mount stopping"));
        }
        if (!waiter->completed) {
            return std::unexpected(make_error(Error::Code::Timeout, "Remote wait timed out"));
        }
        if (waiter->error.has_value()) {
            return std::unexpected(*waiter->error);
        }
        if (!waiter->notification.has_value()) {
            return std::unexpected(
                make_error(Error::Code::NoObjectFound, "Remote notification missing value"));
        }
        return *waiter->notification;
    }();

    cleanup_waiter();

    state.status.waiter_depth -= 1;
    publishMetrics(state);

    if (!notification_result.has_value()) {
        recordError(state, notification_result.error(), false);
        return notification_result.error();
    }

    auto const& remote_note = notification_result.value();
    if (!remote_note.value.has_value()) {
        auto error = make_error(Error::Code::NoObjectFound, "Remote notification missing value");
        recordError(state, error, false);
        return error;
    }
    auto raw = decode_base64(remote_note.value->data);
    if (!raw.has_value()) {
        recordError(state, raw.error(), false);
        return raw.error();
    }
    auto snapshot = NodeData::deserializeSnapshot(
        std::as_bytes(std::span<const std::uint8_t>(raw->data(), raw->size())));
    if (!snapshot.has_value()) {
        auto error = make_error(Error::Code::InvalidType, "Failed to decode remote value");
        recordError(state, error, false);
        return error;
    }
    if (auto error = snapshot->deserialize(obj, metadata); error.has_value()) {
        recordError(state, *error, false);
        return error;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    recordSuccess(state, elapsed);
    return std::nullopt;
}

void RemoteMountManager::configureMirrors(MountState& state) {
    state.mirrors.clear();

    auto configured = state.options.mirrors;
    auto has_mirror = [&](RemoteMountClientOptions::MirrorTarget target,
                         RemoteMountClientOptions::MirrorMode   mode) {
        for (auto const& mirror : configured) {
            if (!mirror.enabled) {
                continue;
            }
            if (mirror.target == target && mirror.mode == mode) {
                return true;
            }
        }
        return false;
    };

    if (!has_mirror(RemoteMountClientOptions::MirrorTarget::RootSpace,
                    RemoteMountClientOptions::MirrorMode::AppendOnly)
        && options_.root_space != nullptr) {
        RemoteMountClientOptions::MirrorPathOptions diagnostics;
        diagnostics.mode        = RemoteMountClientOptions::MirrorMode::AppendOnly;
        diagnostics.target      = RemoteMountClientOptions::MirrorTarget::RootSpace;
        diagnostics.remote_root = "/diagnostics/errors/live";
        diagnostics.local_root
            = join_paths(options_.diagnostics_root.empty() ? std::string{"/diagnostics/errors/live/remotes"}
                                                           : options_.diagnostics_root,
                         state.options.alias);
        diagnostics.max_depth     = 1;
        diagnostics.max_children  = VisitOptions::kDefaultMaxChildren;
        diagnostics.max_nodes     = VisitOptions::kDefaultMaxChildren;
        diagnostics.interval      = std::chrono::milliseconds{750};
        diagnostics.enabled       = true;
        configured.push_back(std::move(diagnostics));
    }

    if (!has_mirror(RemoteMountClientOptions::MirrorTarget::MetricsSpace,
                    RemoteMountClientOptions::MirrorMode::TreeSnapshot)
        && options_.metrics_space != nullptr) {
        RemoteMountClientOptions::MirrorPathOptions metrics;
        metrics.mode        = RemoteMountClientOptions::MirrorMode::TreeSnapshot;
        metrics.target      = RemoteMountClientOptions::MirrorTarget::MetricsSpace;
        metrics.remote_root = std::string{"/inspector/metrics/remotes/"} + state.options.alias
            + "/server";
        metrics.local_root = join_paths(options_.metrics_root,
                                        state.options.alias + std::string{"/server"});
        metrics.max_depth     = VisitOptions::kUnlimitedDepth;
        metrics.max_children  = VisitOptions::kDefaultMaxChildren;
        metrics.max_nodes     = 512;
        metrics.interval      = std::chrono::milliseconds{1000};
        metrics.enabled       = true;
        configured.push_back(std::move(metrics));
    }

    auto try_add_assignment = [&](RemoteMountClientOptions::MirrorPathOptions const& mirror) {
        if (!mirror.enabled) {
            return;
        }

        PathSpace* target_space = nullptr;
        switch (mirror.target) {
        case RemoteMountClientOptions::MirrorTarget::RootSpace:
            target_space = options_.root_space;
            break;
        case RemoteMountClientOptions::MirrorTarget::MetricsSpace:
            target_space = options_.metrics_space;
            break;
        }
        if (target_space == nullptr) {
            sp_log("RemoteMountManager mirror skipped (target space unavailable)", "RemoteMountManager");
            return;
        }

        auto substituted_remote = substitute_alias_tokens(mirror.remote_root, state.options.alias);
        if (substituted_remote.empty()) {
            sp_log("RemoteMountManager mirror skipped (empty remote root)", "RemoteMountManager");
            return;
        }
        auto normalized_remote = normalize_absolute_path(substituted_remote);
        if (!normalized_remote.has_value()) {
            sp_log("RemoteMountManager mirror skipped (invalid remote root)", "RemoteMountManager");
            return;
        }

        std::string local_pattern = mirror.local_root.empty() ? *normalized_remote
                                                              : substitute_alias_tokens(mirror.local_root,
                                                                                       state.options.alias);
        auto normalized_local = normalize_absolute_path(local_pattern);
        if (!normalized_local.has_value()) {
            sp_log("RemoteMountManager mirror skipped (invalid local root)", "RemoteMountManager");
            return;
        }

        MirrorAssignment assignment;
        assignment.mode         = mirror.mode;
        assignment.target       = mirror.target;
        assignment.target_space = target_space;
        assignment.remote_root  = *normalized_remote;
        assignment.local_root   = *normalized_local;
        assignment.max_depth
            = mirror.max_depth == 0 ? VisitOptions::kUnlimitedDepth : mirror.max_depth;
        assignment.max_children
            = mirror.max_children == 0 ? VisitOptions::kDefaultMaxChildren : mirror.max_children;
        assignment.max_nodes = mirror.max_nodes == 0 ? 256 : mirror.max_nodes;
        assignment.interval  = mirror.interval.count() <= 0 ? std::chrono::milliseconds{500}
                                                           : mirror.interval;
        assignment.next_run  = std::chrono::steady_clock::now();
        state.mirrors.push_back(std::move(assignment));
    };

    for (auto const& mirror : configured) {
        try_add_assignment(mirror);
    }
}

void RemoteMountManager::startMirrorThread(MountState& state) {
    if (state.mirrors.empty()) {
        return;
    }
    if (state.mirror_thread.joinable()) {
        return;
    }
    state.mirror_thread = std::thread([this, &state]() { mirrorLoop(&state); });
}

void RemoteMountManager::stopMirrorThread(MountState& state) {
    if (state.mirror_thread.joinable()) {
        state.mirror_thread.join();
    }
}

void RemoteMountManager::startNotificationThread(MountState& state) {
    if (state.stop_requested.load()) {
        return;
    }
    if (state.notification_thread.joinable()) {
        return;
    }
    state.notification_thread = std::thread([this, &state]() { notificationLoop(&state); });
}

void RemoteMountManager::stopNotificationThread(MountState& state) {
    if (state.notification_thread.joinable()) {
        state.notification_thread.join();
    }
}

void RemoteMountManager::notificationLoop(MountState* state) {
    if (state == nullptr) {
        return;
    }
    while (!state->stop_requested.load()) {
        auto ensure = ensureSession(*state);
        if (!ensure.has_value()) {
            recordError(*state, ensure.error(), true);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            continue;
        }
        std::shared_ptr<RemoteMountSession> session;
        std::string                         session_id;
        {
            std::lock_guard<std::mutex> lk(state->session_mutex);
            session    = state->session;
            session_id = state->session_id;
        }
        if (!session || session_id.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            continue;
        }
        auto notifications = session->streamNotifications(
            session_id, kNotificationStreamTimeout, kNotificationBatch);
        if (!notifications.has_value()) {
            recordError(*state, notifications.error(), true);
            failPendingWaiters(*state, notifications.error());
            publishMetrics(*state);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            continue;
        }
        if (notifications->empty()) {
            continue;
        }
        for (auto const& notification : *notifications) {
            deliverNotification(*state, notification);
            if (state->stop_requested.load()) {
                break;
            }
        }
    }
}

void RemoteMountManager::deliverNotification(MountState& state,
                                             Notification const& notification) {
    std::shared_ptr<PendingWaiter> waiter;
    {
        std::lock_guard<std::mutex> lk(state.waiters_mutex);
        auto                        it = state.waiters.find(notification.subscription_id);
        if (it != state.waiters.end()) {
            waiter = it->second;
            state.waiters.erase(it);
        } else {
            state.status.dropped_notifications += 1;
        }
        state.status.queued_notifications = state.waiters.size();
    }
    if (waiter) {
        {
            std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
            waiter->notification = notification;
            waiter->completed    = true;
        }
        waiter->cv.notify_all();
    }
    publishMetrics(state);
}

void RemoteMountManager::failPendingWaiters(MountState& state, Error const& error) {
    std::lock_guard<std::mutex> wait_lk(state.waiters_mutex);
    for (auto& [_, waiter] : state.waiters) {
        if (!waiter) {
            continue;
        }
        {
            std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
            waiter->error     = error;
            waiter->completed = true;
        }
        waiter->cv.notify_all();
    }
    state.waiters.clear();
    state.status.queued_notifications = 0;
}

void RemoteMountManager::mirrorLoop(MountState* state) {
    if (state == nullptr) {
        return;
    }
    constexpr std::chrono::milliseconds kSleep{50};
    while (!state->stop_requested.load()) {
        auto now = std::chrono::steady_clock::now();
        for (auto& assignment : state->mirrors) {
            if (assignment.target_space == nullptr) {
                continue;
            }
            if (assignment.next_run > now) {
                continue;
            }
            runMirrorAssignment(*state, assignment);
            assignment.next_run = std::chrono::steady_clock::now() + assignment.interval;
        }
        std::this_thread::sleep_for(kSleep);
    }
}

void RemoteMountManager::runMirrorAssignment(MountState& state,
                                             MirrorAssignment& assignment) {
    if (assignment.target_space == nullptr) {
        return;
    }
    auto ensure = ensureSession(state);
    if (!ensure.has_value()) {
        recordError(state, ensure.error(), true);
        return;
    }
    std::shared_ptr<RemoteMountSession> session;
    std::string                         session_id;
    {
        std::lock_guard<std::mutex> lk(state.session_mutex);
        session    = state.session;
        session_id = state.session_id;
    }
    if (!session || session_id.empty()) {
        recordError(state, make_error(Error::Code::InvalidPermissions, "Remote session unavailable"),
                    false);
        return;
    }

    std::optional<Error> error;
    if (assignment.mode == RemoteMountClientOptions::MirrorMode::AppendOnly) {
        error = mirrorAppendOnly(state, assignment, session, session_id);
    } else {
        error = mirrorTreeSnapshot(state, assignment, session, session_id);
    }
    if (error.has_value()) {
        recordError(state, *error, false);
    }
}

std::optional<Error> RemoteMountManager::mirrorAppendOnly(
    MountState& state,
    MirrorAssignment& assignment,
    std::shared_ptr<RemoteMountSession> const& session,
    std::string const& session_id) {
    ReadRequest request;
    request.request_id       = std::string{"mirror-list-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id       = session_id;
    request.path             = assignment.remote_root;
    request.include_value    = false;
    request.include_children = true;

    auto response = session->read(request);
    if (!response.has_value()) {
        return response.error();
    }
    if (response->error) {
        return convert_error_payload(*response->error);
    }

    auto children = response->children;
    std::sort(children.begin(), children.end());
    if (assignment.max_children != 0 && children.size() > assignment.max_children) {
        children.resize(assignment.max_children);
    }

    std::size_t mirrored = 0;
    for (auto const& child : children) {
        if (!assignment.last_child.empty() && child <= assignment.last_child) {
            continue;
        }
        auto remote_child = join_paths(assignment.remote_root, child);
        auto local_child  = join_paths(assignment.local_root, child);
        if (auto error = copyRemoteNode(state, assignment, session, session_id, remote_child, local_child)) {
            return error;
        }
        assignment.last_child = child;
        mirrored += 1;
        if (assignment.max_nodes != 0 && mirrored >= assignment.max_nodes) {
            break;
        }
    }
    return std::nullopt;
}

std::optional<Error> RemoteMountManager::mirrorTreeSnapshot(
    MountState& state,
    MirrorAssignment& assignment,
    std::shared_ptr<RemoteMountSession> const& session,
    std::string const& session_id) {
    struct QueueEntry {
        std::string remote_path;
        std::string local_path;
        std::size_t depth;
    };

    std::deque<QueueEntry> queue;
    queue.push_back({assignment.remote_root, assignment.local_root, 0});

    std::size_t processed = 0;
    while (!queue.empty() && !state.stop_requested.load()) {
        auto entry = queue.front();
        queue.pop_front();

        bool include_children = assignment.max_depth == VisitOptions::kUnlimitedDepth
            || entry.depth + 1 < assignment.max_depth;

        ReadRequest request;
        request.request_id       = std::string{"mirror-node-"} + std::to_string(request_counter_.fetch_add(1));
        request.session_id       = session_id;
        request.path             = entry.remote_path;
        request.include_value    = true;
        request.include_children = include_children;

        auto response = session->read(request);
        if (!response.has_value()) {
            return response.error();
        }
        if (response->error) {
            auto error = convert_error_payload(*response->error);
            if (error.code == Error::Code::NoSuchPath) {
                continue;
            }
            return error;
        }

        if (response->value.has_value()) {
            if (auto error = mirrorSingleNode(*assignment.target_space, entry.local_path, *response->value)) {
                return error;
            }
        }

        if (include_children) {
            auto children = response->children;
            if (assignment.max_children != 0 && children.size() > assignment.max_children) {
                children.resize(assignment.max_children);
            }
            for (auto const& child : children) {
                queue.push_back({join_paths(entry.remote_path, child),
                                 join_paths(entry.local_path, child),
                                 entry.depth + 1});
            }
        }

        processed += 1;
        if (assignment.max_nodes != 0 && processed >= assignment.max_nodes) {
            break;
        }
    }
    return std::nullopt;
}

std::optional<Error> RemoteMountManager::copyRemoteNode(
    MountState& state,
    MirrorAssignment const& assignment,
    std::shared_ptr<RemoteMountSession> const& session,
    std::string const& session_id,
    std::string const& remote_path,
    std::string const& local_path) {
    ReadRequest request;
    request.request_id    = std::string{"mirror-value-"} + std::to_string(request_counter_.fetch_add(1));
    request.session_id    = session_id;
    request.path          = remote_path;
    request.include_value = true;

    auto response = session->read(request);
    if (!response.has_value()) {
        return response.error();
    }
    if (response->error) {
        auto error = convert_error_payload(*response->error);
        if (error.code == Error::Code::NoSuchPath) {
            return std::nullopt;
        }
        return error;
    }
    if (!response->value.has_value() || assignment.target_space == nullptr) {
        return std::nullopt;
    }
    return mirrorSingleNode(*assignment.target_space, local_path, *response->value);
}

std::optional<Error> RemoteMountManager::mirrorSingleNode(PathSpace& space,
                                                          std::string const& local_path,
                                                          ValuePayload const& payload) {
    auto decoded = decode_base64(payload.data);
    if (!decoded.has_value()) {
        return decoded.error();
    }
    auto raw_bytes = std::move(*decoded);
    auto legacy_allowed = allowLegacyPayloads(payload_mode_);
    auto legacy_disabled = [&]() -> std::optional<Error> {
        return make_error(
            Error::Code::InvalidType,
            "Legacy remote payload encodings are disabled (set PATHSPACE_REMOTE_TYPED_PAYLOADS=0 to re-enable temporarily)"
        );
    };

    if (payload.encoding == kEncodingTypedSlidingBuffer) {
        if (payload.type_name.empty()) {
            return make_error(Error::Code::InvalidType, "Mirrored payload missing type");
        }
        auto span_u8 = std::span<const std::uint8_t>(raw_bytes.data(), raw_bytes.size());
        auto bytes   = std::as_bytes(span_u8);
        auto inserted = insertTypedPayloadFromBytes(space, local_path, payload.type_name, bytes);
        if (!inserted) {
            return inserted.error();
        }
        return std::nullopt;
    }

    if (payload.encoding == kEncodingString) {
        if (!legacy_allowed) {
            return legacy_disabled();
        }
        std::string value(reinterpret_cast<char const*>(raw_bytes.data()), raw_bytes.size());
        space.insert(local_path, value);
        return std::nullopt;
    }

    if (payload.encoding == kEncodingVoid) {
        return std::nullopt;
    }

    return make_error(Error::Code::InvalidType, "Unsupported mirrored payload encoding");
}

} // namespace SP::Distributed
