#include "inspector/InspectorRemoteMount.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "inspector/InspectorMetricUtils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#include "httplib.h"

namespace SP::Inspector {
namespace {

constexpr std::string_view kRemoteRoot{"/remote"};
constexpr std::chrono::milliseconds kSleepSlice{50};

[[nodiscard]] auto to_millis_since_epoch(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    if (tp.time_since_epoch().count() <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
}

[[nodiscard]] auto clamp_latency(std::chrono::milliseconds latency) -> std::chrono::milliseconds {
    if (latency.count() < 0) {
        return std::chrono::milliseconds{0};
    }
    return latency;
}

[[nodiscard]] auto compute_health(bool online,
                                  std::uint64_t consecutive_errors,
                                  std::uint64_t error_count) -> std::string {
    if (!online) {
        return error_count > 0 ? std::string{"offline"} : std::string{"initializing"};
    }
    if (consecutive_errors > 0) {
        return std::string{"degraded"};
    }
    return std::string{"healthy"};
}

auto normalize_root(std::string root) -> std::string {
    if (root.empty()) {
        return std::string{"/"};
    }
    if (root.front() != '/') {
        root.insert(root.begin(), '/');
    }
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return root;
}

auto join_alias_path(std::string const& alias_root, std::string const& relative) -> std::string {
    if (relative.empty() || relative == "/") {
        return alias_root;
    }
    if (alias_root == "/") {
        return relative;
    }
    if (relative.front() == '/') {
        if (alias_root.back() == '/') {
            return alias_root + relative.substr(1);
        }
        return alias_root + relative;
    }
    if (alias_root.back() == '/') {
        return alias_root + relative;
    }
    return alias_root + "/" + relative;
}

auto strip_prefix(std::string const& path, std::string const& prefix) -> std::string {
    if (prefix.empty() || prefix == "/") {
        return path;
    }
    if (path == prefix) {
        return std::string{"/"};
    }
    if (path.rfind(prefix, 0) != 0) {
        return path;
    }
    auto remainder = path.substr(prefix.size());
    if (remainder.empty()) {
        return std::string{"/"};
    }
    if (remainder.front() != '/') {
        return std::string{"/"} + remainder;
    }
    return remainder;
}

InspectorNodeSummary prefix_summary(InspectorNodeSummary const& node,
                                    std::string const& alias_root,
                                    std::string const& remote_root) {
    InspectorNodeSummary copy = node;
    copy.path = join_alias_path(alias_root, strip_prefix(node.path, remote_root));
    copy.children.clear();
    copy.children.reserve(node.children.size());
    for (auto const& child : node.children) {
        copy.children.push_back(prefix_summary(child, alias_root, remote_root));
    }
    return copy;
}

InspectorNodeSummary make_placeholder_node(std::string const& alias_root,
                                           RemoteMountStatus const& status) {
    InspectorNodeSummary summary;
    summary.path       = alias_root;
    summary.value_type = "remote";
    std::string value_summary = status.health.empty()
                                    ? (status.connected ? "connected" : "unavailable")
                                    : status.health;
    if (!status.message.empty()) {
        if (!value_summary.empty()) {
            value_summary.append(" — ");
        }
        value_summary.append(status.message);
    }
    if (!status.access_hint.empty()) {
        if (!value_summary.empty()) {
            value_summary.append(" — ");
        }
        value_summary.append(status.access_hint);
    }
    summary.value_summary  = std::move(value_summary);
    summary.child_count    = 0;
    summary.children.clear();
    summary.children_truncated = false;
    return summary;
}

void strip_values_if_needed(InspectorNodeSummary& node, bool include_values) {
    if (!include_values) {
        node.value_summary.clear();
    }
    for (auto& child : node.children) {
        strip_values_if_needed(child, include_values);
    }
}

auto find_node(InspectorNodeSummary const& node, std::string const& path)
    -> InspectorNodeSummary const* {
    if (node.path == path) {
        return &node;
    }
    for (auto const& child : node.children) {
        if (auto found = find_node(child, path)) {
            return found;
        }
    }
    return nullptr;
}

auto url_encode(std::string_view value) -> std::string {
    std::string encoded;
    encoded.reserve(value.size() * 2);
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[(ch >> 4U) & 0x0F]);
        encoded.push_back(hex[ch & 0x0F]);
    }
    return encoded;
}

auto build_request_path(RemoteMountOptions const& options) -> std::string {
    std::string path = "/inspector/tree?root=";
    path.append(url_encode(options.root.empty() ? std::string{"/"} : options.root));
    path.append("&depth=");
    path.append(std::to_string(options.snapshot.max_depth));
    path.append("&max_children=");
    path.append(std::to_string(options.snapshot.max_children));
    path.append("&include_values=");
    path.append(options.snapshot.include_values ? "1" : "0");
    return path;
}

auto to_timeout_pair(std::chrono::milliseconds ms) -> std::pair<time_t, long> {
    auto seconds      = std::chrono::duration_cast<std::chrono::seconds>(ms);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(ms - seconds);
    return {static_cast<time_t>(seconds.count()), static_cast<long>(microseconds.count())};
}

auto fetch_snapshot(RemoteMountOptions const& options) -> Expected<InspectorSnapshot> {
    if (options.alias.empty()) {
        return std::unexpected(Error{Error::Code::MalformedInput, "remote mount alias missing"});
    }

    std::unique_ptr<httplib::Client> client;
    if (options.use_tls) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client = std::make_unique<httplib::SSLClient>(options.host, options.port);
#else
        return std::unexpected(Error{Error::Code::NotSupported,
                                     "TLS remote mounts require CPPHTTPLIB_OPENSSL_SUPPORT"});
#endif
    } else {
        client = std::make_unique<httplib::Client>(options.host, options.port);
    }

    auto [sec, usec] = to_timeout_pair(options.request_timeout);
    client->set_connection_timeout(sec, usec);
    client->set_read_timeout(sec, usec);
    client->set_write_timeout(sec, usec);

    auto response = client->Get(build_request_path(options).c_str());
    if (!response) {
        return std::unexpected(Error{Error::Code::Timeout,
                                     "remote mount request failed or timed out"});
    }

    if (response->status != 200) {
        return std::unexpected(Error{Error::Code::UnknownError,
                                     std::string{"remote mount returned HTTP "}
                                         + std::to_string(response->status)});
    }

    return ParseInspectorSnapshot(response->body);
}

auto format_status(RemoteMountStatus const& status) -> std::string {
    std::ostringstream oss;
    auto const* state = status.health.empty()
                            ? (status.connected ? "connected" : "unavailable")
                            : status.health.c_str();
    oss << "remote mount " << status.alias << ": " << state;
    if (status.last_latency.count() > 0) {
        oss << " @" << status.last_latency.count() << "ms";
    }
    if (status.waiter_depth > 0) {
        oss << " waiters=" << status.waiter_depth;
    }
    if (!status.message.empty()) {
        oss << " (" << status.message << ")";
    }
    if (!status.access_hint.empty()) {
        oss << " [" << status.access_hint << "]";
    }
    return oss.str();
}

auto split_alias_and_tail(std::string const& root) -> std::pair<std::string, std::string> {
    if (root.rfind(kRemoteRoot, 0) != 0) {
        return {{}, {}};
    }
    auto remainder = root.substr(kRemoteRoot.size());
    if (remainder.empty() || remainder == "/") {
        return {{}, {}};
    }
    if (remainder.front() == '/') {
        remainder.erase(remainder.begin());
    }
    auto slash = remainder.find('/');
    if (slash == std::string::npos) {
        return {remainder, std::string{}};
    }
    auto alias = remainder.substr(0, slash);
    auto tail  = remainder.substr(slash);
    if (tail.empty()) {
        tail = "/";
    }
    return {alias, tail};
}

auto join_remote_path(RemoteMountOptions const& options, std::string const& tail) -> std::string {
    auto remote_root = options.root.empty() ? std::string{"/"} : options.root;
    if (tail.empty() || tail == "/") {
        return remote_root;
    }
    if (remote_root == "/") {
        return normalize_root(tail);
    }
    if (tail.front() == '/') {
        if (remote_root.back() == '/') {
            return remote_root + tail.substr(1);
        }
        return remote_root + tail;
    }
    if (remote_root.back() == '/') {
        return remote_root + tail;
    }
    return remote_root + "/" + tail;
}

} // namespace

RemoteMountRegistry::RemoteMountRegistry(PathSpace* metrics_space, std::string metrics_root)
    : metrics_space_(metrics_space)
    , metrics_root_(std::move(metrics_root)) {
    if (metrics_root_.empty()) {
        metrics_root_ = "/inspector/metrics/remotes";
    }
}

RemoteMountRegistry::RemoteMountRegistry(std::vector<RemoteMountOptions> options,
                                         PathSpace*                     metrics_space,
                                         std::string                    metrics_root)
    : RemoteMountRegistry(metrics_space, std::move(metrics_root)) {
    setOptions(std::move(options));
}

void RemoteMountRegistry::setOptions(std::vector<RemoteMountOptions> options) {
    std::unique_lock lock(mutex_);
    mounts_.clear();
    mounts_.reserve(options.size());
    for (auto& opt : options) {
        MountData data;
        data.options = std::move(opt);
        data.options.root = normalize_root(data.options.root);
        mounts_.push_back(std::move(data));
        publish_metrics_locked(mounts_.back());
    }
}

auto RemoteMountRegistry::findMount(std::string const& alias) -> MountData* {
    auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](MountData const& data) {
        return data.options.alias == alias;
    });
    if (it == mounts_.end()) {
        return nullptr;
    }
    return &*it;
}

auto RemoteMountRegistry::findMount(std::string const& alias) const -> MountData const* {
    auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](MountData const& data) {
        return data.options.alias == alias;
    });
    if (it == mounts_.end()) {
        return nullptr;
    }
    return &*it;
}

auto RemoteMountRegistry::aliasRoot(std::string const& alias) -> std::string {
    if (alias.empty()) {
        return std::string{kRemoteRoot};
    }
    return std::string{kRemoteRoot} + "/" + alias;
}

auto RemoteMountRegistry::buildStatus(MountData const& mount) const -> RemoteMountStatus {
    RemoteMountStatus status;
    status.alias       = mount.options.alias;
    status.connected   = mount.connected && mount.snapshot.has_value();
    status.message     = status.connected ? std::string{} : mount.last_error;
    status.last_update = mount.last_update;
    status.path        = aliasRoot(mount.options.alias);
    status.access_hint = mount.options.access_hint;
    status.last_latency = mount.last_latency;
    status.average_latency = mount.average_latency;
    status.max_latency     = mount.max_latency;
    status.success_count   = mount.success_count;
    status.error_count     = mount.error_count;
    status.consecutive_errors = mount.consecutive_errors;
    status.waiter_depth    = mount.waiter_depth;
    status.max_waiter_depth = mount.max_waiter_depth;
    status.last_error_time = mount.last_error_time;
    status.health          = compute_health(status.connected,
                                 mount.consecutive_errors,
                                 mount.error_count);
    return status;
}

void RemoteMountRegistry::publish_metrics_locked(MountData const& mount) const {
    if (metrics_space_ == nullptr) {
        return;
    }
    auto build_path = [&](std::string_view suffix) {
        std::string path = metrics_root_;
        if (path.empty()) {
            path = "/inspector/metrics/remotes";
        }
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        auto const& alias = mount.options.alias;
        if (alias.empty()) {
            path.append("_");
        } else {
            path.append(alias);
        }
        if (!suffix.empty()) {
            if (suffix.front() != '/') {
                path.push_back('/');
            }
            path.append(suffix.begin(), suffix.end());
        }
        return path;
    };

    auto online = mount.connected && mount.snapshot.has_value();
    auto health = compute_health(online, mount.consecutive_errors, mount.error_count);

    auto publish_metric = [&](std::string_view suffix, auto const& value) {
        auto path = build_path(suffix);
        if (auto replaced = Detail::ReplaceMetricValue(*metrics_space_, path, value); !replaced) {
            (void)replaced;
        }
    };

    publish_metric("status/connected", static_cast<std::uint64_t>(online));
    publish_metric("status/health", health);
    publish_metric("latency/last_ms", static_cast<std::uint64_t>(mount.last_latency.count()));
    publish_metric("latency/average_ms", static_cast<std::uint64_t>(mount.average_latency.count()));
    publish_metric("latency/max_ms", static_cast<std::uint64_t>(mount.max_latency.count()));
    publish_metric("requests/success_total", mount.success_count);
    publish_metric("requests/error_total", mount.error_count);
    publish_metric("requests/consecutive_errors", mount.consecutive_errors);
    publish_metric("waiters/current", mount.waiter_depth);
    publish_metric("waiters/max", mount.max_waiter_depth);
    publish_metric("timestamps/last_update_ms", to_millis_since_epoch(mount.last_update));
    publish_metric("timestamps/last_error_ms", to_millis_since_epoch(mount.last_error_time));
    publish_metric("meta/root", mount.options.root);
    if (!mount.options.access_hint.empty()) {
        publish_metric("meta/access_hint", mount.options.access_hint);
    }
    if (!mount.last_error.empty()) {
        publish_metric("status/last_error", mount.last_error);
    }
}

void RemoteMountRegistry::updateSnapshot(std::string const& alias,
                                         InspectorSnapshot  snapshot,
                                         std::chrono::milliseconds latency) {
    std::unique_lock lock(mutex_);
    if (auto* mount = findMount(alias)) {
        mount->snapshot    = std::move(snapshot);
        mount->connected   = true;
        mount->last_error.clear();
        mount->last_update = std::chrono::system_clock::now();
        ++mount->version;
        auto clamped_latency = clamp_latency(latency);
        mount->last_latency  = clamped_latency;
        if (clamped_latency > mount->max_latency) {
            mount->max_latency = clamped_latency;
        }
        auto delta_ms = static_cast<std::uint64_t>(clamped_latency.count());
        mount->total_latency_ms += delta_ms;
        ++mount->success_count;
        if (mount->success_count > 0) {
            mount->average_latency
                = std::chrono::milliseconds(mount->total_latency_ms / mount->success_count);
        } else {
            mount->average_latency = std::chrono::milliseconds{0};
        }
        mount->consecutive_errors = 0;
        publish_metrics_locked(*mount);
    }
}

void RemoteMountRegistry::updateError(std::string const& alias,
                                      std::string        message,
                                      std::chrono::milliseconds latency) {
    std::unique_lock lock(mutex_);
    if (auto* mount = findMount(alias)) {
        mount->connected   = false;
        mount->last_error  = std::move(message);
        mount->last_update = std::chrono::system_clock::now();
        mount->last_error_time = mount->last_update;
        ++mount->error_count;
        ++mount->consecutive_errors;
        mount->last_latency = clamp_latency(latency);
        publish_metrics_locked(*mount);
    }
}

void RemoteMountRegistry::incrementWaiters(std::string const& alias) {
    std::unique_lock lock(mutex_);
    if (auto* mount = findMount(alias)) {
        ++mount->waiter_depth;
        if (mount->waiter_depth > mount->max_waiter_depth) {
            mount->max_waiter_depth = mount->waiter_depth;
        }
        publish_metrics_locked(*mount);
    }
}

void RemoteMountRegistry::decrementWaiters(std::string const& alias) {
    std::unique_lock lock(mutex_);
    if (auto* mount = findMount(alias)) {
        if (mount->waiter_depth > 0) {
            --mount->waiter_depth;
        }
        publish_metrics_locked(*mount);
    }
}

bool RemoteMountRegistry::empty() const {
    std::shared_lock lock(mutex_);
    return mounts_.empty();
}

auto RemoteMountRegistry::classifyRoot(std::string const& root) const -> RootKind {
    auto normalized = normalize_root(root);
    if (normalized.rfind(kRemoteRoot, 0) != 0) {
        return RootKind::Local;
    }
    if (normalized == kRemoteRoot) {
        return RootKind::RemoteContainer;
    }
    auto trimmed = normalized.substr(kRemoteRoot.size());
    if (trimmed.empty() || trimmed == "/") {
        return RootKind::RemoteContainer;
    }
    if (trimmed.front() != '/') {
        return RootKind::Local;
    }
    trimmed.erase(trimmed.begin());
    auto slash = trimmed.find('/');
    if (slash == std::string::npos || slash == trimmed.size() - 1) {
        return RootKind::RemoteMount;
    }
    return RootKind::RemoteSubtree;
}

auto RemoteMountRegistry::buildRemoteSnapshot(InspectorSnapshotOptions const& options) const
    -> std::optional<Expected<InspectorSnapshot>> {
    auto kind = classifyRoot(options.root);
    if (kind == RootKind::Local) {
        return std::nullopt;
    }

    std::shared_lock lock(mutex_);
    if (mounts_.empty()) {
        return Expected<InspectorSnapshot>{std::unexpected(
            Error{Error::Code::NoSuchPath, "no remote mounts configured"})};
    }

    if (kind == RootKind::RemoteContainer) {
        std::vector<RemoteMountStatus> status_vec;
        status_vec.reserve(mounts_.size());
        for (auto const& mount : mounts_) {
            status_vec.push_back(buildStatus(mount));
        }

        InspectorSnapshot snapshot;
        snapshot.options           = options;
        snapshot.options.root      = std::string{kRemoteRoot};
        snapshot.root.path         = std::string{kRemoteRoot};
        snapshot.root.value_type   = "remote_mounts";
        snapshot.root.child_count  = mounts_.size();
        snapshot.root.children.clear();
        for (auto const& mount : mounts_) {
            auto alias_root = aliasRoot(mount.options.alias);
            if (mount.snapshot) {
                snapshot.root.children.push_back(
                    prefix_summary(mount.snapshot->root, alias_root, mount.snapshot->options.root));
            } else {
                auto status = buildStatus(mount);
                snapshot.root.children.push_back(make_placeholder_node(alias_root, status));
            }
        }
        snapshot.root.children_truncated = false;
        for (auto const& status : status_vec) {
            snapshot.diagnostics.push_back(format_status(status));
        }
        return snapshot;
    }

    auto [alias, tail] = split_alias_and_tail(options.root);
    if (alias.empty()) {
        return Expected<InspectorSnapshot>{std::unexpected(
            Error{Error::Code::NoSuchPath, "remote alias not specified"})};
    }

    auto const* mount = findMount(alias);
    if (mount == nullptr) {
        return Expected<InspectorSnapshot>{std::unexpected(
            Error{Error::Code::NoSuchPath, "unknown remote mount alias"})};
    }
    if (!mount->snapshot) {
        return Expected<InspectorSnapshot>{std::unexpected(
            Error{Error::Code::NoObjectFound, "remote mount offline"})};
    }

    auto remote_path = tail.empty() ? mount->snapshot->options.root
                                    : join_remote_path(mount->options, tail);
    auto* node = find_node(mount->snapshot->root, remote_path);
    if (node == nullptr) {
        return Expected<InspectorSnapshot>{std::unexpected(
            Error{Error::Code::NoSuchPath, "remote path not found"})};
    }

    auto alias_root = aliasRoot(alias);
    InspectorSnapshot snapshot;
    snapshot.options      = options;
    snapshot.options.root = normalize_root(options.root);
    snapshot.root         = prefix_summary(*node, alias_root, mount->snapshot->options.root);
    strip_values_if_needed(snapshot.root, options.include_values);
    snapshot.diagnostics  = mount->snapshot->diagnostics;
    auto status = buildStatus(*mount);
    status.message = mount->last_error;
    snapshot.diagnostics.push_back(format_status(status));
    return snapshot;
}

void RemoteMountRegistry::augmentLocalSnapshot(InspectorSnapshot& snapshot) const {
    if (classifyRoot(snapshot.options.root) != RootKind::Local) {
        return;
    }

    std::shared_lock lock(mutex_);
    if (mounts_.empty()) {
        return;
    }

    std::vector<RemoteMountStatus> status_vec;
    status_vec.reserve(mounts_.size());
    for (auto const& mount : mounts_) {
        status_vec.push_back(buildStatus(mount));
    }

    InspectorNodeSummary remote_root;
    remote_root.path          = std::string{kRemoteRoot};
    remote_root.value_type    = "remote_mounts";
    remote_root.child_count   = mounts_.size();
    remote_root.children.clear();
    for (auto const& mount : mounts_) {
        auto alias_root = aliasRoot(mount.options.alias);
        if (mount.snapshot) {
            remote_root.children.push_back(
                prefix_summary(mount.snapshot->root, alias_root, mount.snapshot->options.root));
        } else {
            auto status = buildStatus(mount);
            remote_root.children.push_back(make_placeholder_node(alias_root, status));
        }
    }
    remote_root.children_truncated = false;

    auto& children = snapshot.root.children;
    auto  existing = std::find_if(children.begin(), children.end(), [](InspectorNodeSummary const& node) {
        return node.path == kRemoteRoot;
    });
    if (existing != children.end()) {
        *existing = remote_root;
    } else {
        children.push_back(remote_root);
    }
    snapshot.root.child_count = children.size();

    for (auto const& status : status_vec) {
        snapshot.diagnostics.push_back(format_status(status));
    }
}

auto RemoteMountRegistry::statuses() const -> std::vector<RemoteMountStatus> {
    std::shared_lock lock(mutex_);
    std::vector<RemoteMountStatus> status;
    status.reserve(mounts_.size());
    for (auto const& mount : mounts_) {
        status.push_back(buildStatus(mount));
    }
    return status;
}

RemoteMountManager::RemoteMountManager(std::vector<RemoteMountOptions> options,
                                       PathSpace*                     metrics_space,
                                       std::string                    metrics_root)
    : options_(std::move(options))
    , registry_(options_, metrics_space, std::move(metrics_root)) {}

RemoteMountManager::~RemoteMountManager() {
    stop();
}

void RemoteMountManager::start() {
    if (options_.empty() || running_.exchange(true)) {
        return;
    }
    for (auto const& option : options_) {
        launchWorker(option);
    }
}

void RemoteMountManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    for (auto const& worker : workers_) {
        worker->stop.store(true, std::memory_order_release);
    }
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    workers_.clear();
}

bool RemoteMountManager::hasMounts() const {
    return !options_.empty();
}

auto RemoteMountManager::classifyRoot(std::string const& root) const
    -> RemoteMountRegistry::RootKind {
    return registry_.classifyRoot(root);
}

auto RemoteMountManager::aliasForRoot(std::string const& root) const -> std::optional<std::string> {
    auto normalized = normalize_root(root);
    auto [alias, ignore] = split_alias_and_tail(normalized);
    if (alias.empty()) {
        return std::nullopt;
    }
    return alias;
}

auto RemoteMountManager::buildRemoteSnapshot(InspectorSnapshotOptions const& options) const
    -> std::optional<Expected<InspectorSnapshot>> {
    return registry_.buildRemoteSnapshot(options);
}

void RemoteMountManager::augmentLocalSnapshot(InspectorSnapshot& snapshot) const {
    registry_.augmentLocalSnapshot(snapshot);
}

auto RemoteMountManager::statuses() const -> std::vector<RemoteMountStatus> {
    return registry_.statuses();
}

void RemoteMountManager::incrementWaiters(std::string const& alias) {
    registry_.incrementWaiters(alias);
}

void RemoteMountManager::decrementWaiters(std::string const& alias) {
    registry_.decrementWaiters(alias);
}

void RemoteMountManager::updateSnapshotForTest(std::string const& alias,
                                               InspectorSnapshot  snapshot,
                                               std::chrono::milliseconds latency) {
    registry_.updateSnapshot(alias, std::move(snapshot), latency);
}

void RemoteMountManager::launchWorker(RemoteMountOptions const& options) {
    auto worker       = std::make_shared<MountWorker>();
    worker->options   = options;
    worker->thread    = std::thread([this, worker]() { pollLoop(worker); });
    workers_.push_back(worker);
}

void RemoteMountManager::pollLoop(std::shared_ptr<MountWorker> worker) {
    while (!worker->stop.load(std::memory_order_acquire)) {
        auto start    = std::chrono::steady_clock::now();
        auto snapshot = fetch_snapshot(worker->options);
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (snapshot) {
            registry_.updateSnapshot(worker->options.alias, std::move(*snapshot), latency);
        } else {
            registry_.updateError(worker->options.alias,
                                  describeError(snapshot.error()),
                                  latency);
        }

        auto waited = std::chrono::milliseconds{0};
        while (waited < worker->options.refresh_interval
               && !worker->stop.load(std::memory_order_acquire)) {
            auto slice = std::min(kSleepSlice, worker->options.refresh_interval - waited);
            std::this_thread::sleep_for(slice);
            waited += slice;
        }
    }
}

} // namespace SP::Inspector
