#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#define private public
#define protected public
#include <pathspace/distributed/RemoteMountManager.hpp>
#undef private
#undef protected
#include <pathspace/distributed/RemoteExecutionRegistry.hpp>
#include <pathspace/distributed/RemoteMountServer.hpp>
#include <pathspace/distributed/RemoteMountLoopback.hpp>
#include <pathspace/distributed/RemoteMountTls.hpp>
#include <pathspace/type/InputMetadataT.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace SP;
using namespace SP::Distributed;
namespace Loopback = SP::Distributed::Loopback;

PATHSPACE_REGISTER_REMOTE_EXECUTION_ENCODER(std::vector<int>);

namespace {

auto describe_status(RemoteMountManager const& manager) -> std::string {
    std::ostringstream oss;
    auto statuses = manager.statuses();
    if (statuses.empty()) {
        oss << "<none>";
        return oss.str();
    }
    auto const& status = statuses.front();
    oss << "connected=" << status.connected << " message=\"" << status.message << "\"";
    return oss.str();
}

void capture_status(RemoteMountManager const& manager) {
    CAPTURE(describe_status(manager));
}

[[nodiscard]] auto make_auth() -> AuthContext {
    AuthContext auth;
    auth.kind        = AuthKind::MutualTls;
    auth.subject     = "CN=client";
    auth.audience    = "pathspace";
    auth.proof       = "sha256:fingerprint";
    auth.fingerprint = "sha256:cert";
    auth.issued_at_ms  = 100;
    auth.expires_at_ms = 10'000;
    return auth;
}

[[nodiscard]] auto encode_base64(std::string_view input) -> std::string {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2U) / 3U) * 4U);
    auto span = std::span<const std::byte>(reinterpret_cast<std::byte const*>(input.data()), input.size());
    std::size_t index = 0;
    while (index + 2U < span.size()) {
        auto b0 = static_cast<unsigned char>(span[index]);
        auto b1 = static_cast<unsigned char>(span[index + 1U]);
        auto b2 = static_cast<unsigned char>(span[index + 2U]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
        encoded.push_back(kAlphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)]);
        encoded.push_back(kAlphabet[b2 & 0x3FU]);
        index += 3U;
    }
    if (index < span.size()) {
        auto b0 = static_cast<unsigned char>(span[index]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        if (index + 1U < span.size()) {
            auto b1 = static_cast<unsigned char>(span[index + 1U]);
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

[[nodiscard]] auto make_server(PathSpace& space,
                               PathSpace& metrics,
                               PathSpace& diagnostics,
                               std::string export_root = "/apps/demo")
    -> std::shared_ptr<RemoteMountServer> {
    RemoteMountServerOptions options;
    RemoteMountExportOptions export_opts;
    export_opts.alias       = "alpha";
    export_opts.export_root = std::move(export_root);
    export_opts.space       = &space;
    export_opts.capabilities = {"read", "wait", "insert", "take"};
    options.exports.push_back(export_opts);
    options.metrics_space     = &metrics;
    options.diagnostics_space = &diagnostics;
    return std::make_shared<RemoteMountServer>(options);
}

[[nodiscard]] auto make_options(PathSpace& local,
                                PathSpace& metrics,
                                RemoteMountClientOptions mount) -> RemoteMountManagerOptions {
    RemoteMountManagerOptions options;
    options.root_space   = &local;
    options.metrics_space = &metrics;
    options.mounts.push_back(std::move(mount));
    return options;
}

class CountingSession final : public RemoteMountSession {
public:
    CountingSession(std::shared_ptr<RemoteMountServer> server,
                    std::shared_ptr<std::atomic<int>> counter)
        : server_(std::move(server))
        , counter_(std::move(counter)) {}

    auto open(MountOpenRequest const& request) -> Expected<MountOpenResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleMountOpen(request);
    }

    auto read(ReadRequest const& request) -> Expected<ReadResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleRead(request);
    }

    auto insert(InsertRequest const& request) -> Expected<InsertResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleInsert(request);
    }

    auto take(TakeRequest const& request) -> Expected<TakeResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        counter_->fetch_add(1, std::memory_order_relaxed);
        return server_->handleTake(request);
    }

    auto waitSubscribe(WaitSubscriptionRequest const& request)
        -> Expected<WaitSubscriptionAck> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleWaitSubscribe(request);
    }

    auto nextNotification(std::string const& subscription_id,
                          std::chrono::milliseconds timeout)
        -> Expected<std::optional<Notification>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (auto note = server_->nextNotification(subscription_id); note.has_value()) {
                return note;
            }
            if (timeout.count() == 0) {
                return std::optional<Notification>{};
            }
            if (timeout.count() > 0
                && std::chrono::steady_clock::now() - start >= timeout) {
                return std::optional<Notification>{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    auto heartbeat(Heartbeat const& heartbeat) -> Expected<void> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleHeartbeat(heartbeat);
    }

    auto streamNotifications(std::string const&    session_id,
                             std::chrono::milliseconds timeout,
                             std::size_t             max_batch)
        -> Expected<std::vector<Notification>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleNotificationStream(session_id, timeout, max_batch);
    }

private:
    std::shared_ptr<RemoteMountServer>   server_;
    std::shared_ptr<std::atomic<int>>    counter_;
};

class CountingFactory final : public RemoteMountSessionFactory {
public:
    CountingFactory(std::shared_ptr<RemoteMountServer> server,
                    std::shared_ptr<std::atomic<int>> counter)
        : server_(std::move(server))
        , counter_(std::move(counter)) {}

    auto create(RemoteMountClientOptions const&)
        -> Expected<std::shared_ptr<RemoteMountSession>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return std::make_shared<CountingSession>(server_, counter_);
    }

private:
    std::shared_ptr<RemoteMountServer> server_;
    std::shared_ptr<std::atomic<int>>  counter_;
};

[[nodiscard]] auto make_counting_factory(std::shared_ptr<RemoteMountServer> server,
                                         std::shared_ptr<std::atomic<int>> counter)
    -> std::shared_ptr<RemoteMountSessionFactory> {
    return std::make_shared<CountingFactory>(std::move(server), std::move(counter));
}

[[nodiscard]] auto fixture_path(std::string_view relative) -> std::string {
#ifdef PATHSPACE_SOURCE_DIR
    std::string base{PATHSPACE_SOURCE_DIR};
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(relative.begin(), relative.end());
    return base;
#else
    return std::string(relative);
#endif
}

} // namespace

TEST_SUITE("distributed.remotemount.manager") {
TEST_CASE("RemoteMountManager reads remote values") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    remote.insert("/apps/demo/state", std::string{"demo"});

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    CAPTURE(describe_status(manager));
    auto value = local.read<std::string>("/remote/alpha/state");
    REQUIRE_MESSAGE(value.has_value(), SP::describeError(value.error()));
    CHECK(value.value() == "demo");

    auto connected = metrics.read<int>("/inspector/metrics/remotes/alpha/client/connected");
    REQUIRE(connected.has_value());
    CHECK(connected.value() == 1);
}

TEST_CASE("RemoteMountManager waits for remote notifications") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    capture_status(manager);
    std::thread inserter([&] {
        std::this_thread::sleep_for(50ms);
        remote.insert("/apps/demo/events", std::string{"event"});
    });

    auto waited = local.read<std::string>(
        "/remote/alpha/events", Out{} & Block{std::chrono::milliseconds{500}});

    inserter.join();
    REQUIRE(waited.has_value());
    CHECK(waited.value() == "event");
}

TEST_CASE("RemoteMountManager inserts remote values") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    auto insert = local.insert("/remote/alpha/state", std::string{"from_local"});
    CHECK(insert.errors.empty());

    auto remote_value = remote.read<std::string>("/apps/demo/state");
    REQUIRE(remote_value.has_value());
    CHECK(remote_value.value() == "from_local");

    auto insert_int = local.insert("/remote/alpha/counter", 42);
    CHECK(insert_int.errors.empty());

    auto remote_counter = remote.read<int>("/apps/demo/counter");
    REQUIRE(remote_counter.has_value());
    CHECK(remote_counter.value() == 42);
}

TEST_CASE("RemoteMountManager forwards execution inserts with string results") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    capture_status(manager);
    auto result = local.insert("/remote/alpha/generated", []() -> std::string { return std::string{"remote-task"}; });
    CHECK(result.errors.empty());
    CHECK(result.nbrTasksInserted == 1);

    auto remote_value = remote.read<std::string>("/apps/demo/generated");
    REQUIRE(remote_value.has_value());
    CHECK(remote_value.value() == "remote-task");
}

TEST_CASE("RemoteMountManager forwards execution inserts with numeric results") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    auto result = local.insert("/remote/alpha/count", []() -> std::int64_t { return 64; });
    CHECK(result.errors.empty());
    CHECK(result.nbrTasksInserted == 1);

    auto remote_count = remote.read<std::int64_t>("/apps/demo/count");
    REQUIRE(remote_count.has_value());
    CHECK(remote_count.value() == 64);

    auto result_double = local.insert("/remote/alpha/fraction", []() -> double { return 3.25; });
    CHECK(result_double.errors.empty());
    CHECK(result_double.nbrTasksInserted == 1);

    auto remote_fraction = remote.read<double>("/apps/demo/fraction");
    REQUIRE(remote_fraction.has_value());
    CHECK(remote_fraction.value() == doctest::Approx(3.25));
}

TEST_CASE("RemoteMountManager forwards execution inserts for registered types") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    capture_status(manager);
    auto result = local.insert(
        "/remote/alpha/vector",
        []() -> std::vector<int> { return std::vector<int>{5, 8, 13, 21}; });
    CHECK(result.errors.empty());
    CHECK(result.nbrTasksInserted == 1);

    auto remote_value = remote.read<std::vector<int>>("/apps/demo/vector");
    REQUIRE(remote_value.has_value());
    CHECK(remote_value.value() == std::vector<int>{5, 8, 13, 21});
}

TEST_CASE("RemoteMountManager takes remote values") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    remote.insert("/apps/demo/state", std::string{"queued"});

    auto server  = make_server(remote, metrics, diagnostics);
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    auto taken = local.take<std::string>("/remote/alpha/state");
   REQUIRE(taken.has_value());
   CHECK(taken.value() == "queued");

   auto missing = remote.read<std::string>("/apps/demo/state");
   CHECK(!missing.has_value());

    remote.insert("/apps/demo/counter", 17);
    auto taken_int = local.take<int>("/remote/alpha/counter");
    REQUIRE(taken_int.has_value());
    CHECK(taken_int.value() == 17);

    auto remote_missing = remote.read<int>("/apps/demo/counter");
    CHECK(!remote_missing.has_value());
}

TEST_CASE("RemoteMountManager batches take requests") {
    PathSpace remote;
    PathSpace local;
    PathSpace metrics;
    PathSpace diagnostics;

    for (int i = 0; i < 5; ++i) {
        remote.insert("/apps/demo/queue", i);
    }

    auto server  = make_server(remote, metrics, diagnostics);
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto factory = make_counting_factory(server, counter);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.take_batch_size = 3;
    mount.auth            = make_auth();

    RemoteMountManager manager(make_options(local, metrics, mount), factory);
    manager.start();

    for (int i = 0; i < 5; ++i) {
        auto taken = local.take<int>("/remote/alpha/queue");
        REQUIRE(taken.has_value());
        CHECK(taken.value() == i);
    }

    CHECK(counter->load() == 2);
}

TEST_CASE("RemoteMountManager mirrors diagnostics events into local namespace") {
    PathSpace remote;
    PathSpace local;
    PathSpace client_metrics;
    PathSpace diagnostics;

    remote.insert("/diagnostics/errors/live/202512060001", std::string{"remote diag"});

    auto server  = make_server(remote, remote, diagnostics, "/");
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.auth = make_auth();

    RemoteMountManagerOptions options = make_options(local, client_metrics, mount);
    options.diagnostics_root          = "/diagnostics/errors/live/remotes";

    RemoteMountManager manager(options, factory);
    manager.start();

    auto target_path = std::string{"/diagnostics/errors/live/remotes/alpha/202512060001"};
    std::optional<std::string> mirrored;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = local.read<std::string, std::string>(target_path);
        if (value.has_value()) {
            mirrored = value.value();
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    manager.stop();

    REQUIRE(mirrored.has_value());
    CHECK(mirrored.value() == "remote diag");
}

TEST_CASE("RemoteMountManager mirrors server metrics subtree") {
    PathSpace remote;
    PathSpace local;
    PathSpace client_metrics;
    PathSpace diagnostics;

    remote.insert("/inspector/metrics/remotes/alpha/server/sessions", 5);

    auto server  = make_server(remote, remote, diagnostics, "/");
    auto factory = Loopback::makeFactory(server);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/";
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.auth = make_auth();

    RemoteMountManagerOptions options = make_options(local, client_metrics, mount);
    options.metrics_root              = "/inspector/metrics/remotes";

    RemoteMountManager manager(options, factory);
    manager.start();

    auto metric_path = std::string{"/inspector/metrics/remotes/alpha/server/sessions"};
    std::optional<int> replicated;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = client_metrics.read<int>(metric_path);
        if (value.has_value() && value.value() == 5) {
            replicated = value.value();
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    manager.stop();

    REQUIRE(replicated.has_value());
    CHECK(replicated.value() == 5);
}

TEST_CASE("RemoteMountManager connects over TLS transport") {
    PathSpace remote;
    PathSpace local;
    PathSpace client_metrics;
    PathSpace server_metrics;
    PathSpace diagnostics;

    auto server = make_server(remote, server_metrics, diagnostics);

    RemoteMountTlsServerConfig tls_server_config;
    tls_server_config.bind_address              = "127.0.0.1";
    tls_server_config.port                     = 0;
    tls_server_config.certificate_path         = fixture_path("tests/data/remote_mount_tls/server.crt");
    tls_server_config.private_key_path         = fixture_path("tests/data/remote_mount_tls/server.key");
    tls_server_config.ca_cert_path             = fixture_path("tests/data/remote_mount_tls/ca.crt");
    tls_server_config.require_client_certificate = true;

    RemoteMountTlsServer tls_server(tls_server_config, server);
    REQUIRE(tls_server.start());
    REQUIRE(tls_server.port() != 0);

    RemoteMountClientOptions mount;
    mount.alias       = "alpha";
    mount.export_root = "/apps/demo";
    mount.host        = "127.0.0.1";
    mount.port        = tls_server.port();
    mount.capabilities.push_back(CapabilityRequest{.name = "read"});
    mount.capabilities.push_back(CapabilityRequest{.name = "wait"});
    mount.capabilities.push_back(CapabilityRequest{.name = "insert"});
    mount.capabilities.push_back(CapabilityRequest{.name = "take"});
    mount.auth = make_auth();
    mount.auth.subject.clear();
    mount.auth.proof.clear();
    mount.auth.fingerprint.clear();

    RemoteMountTlsClientConfig tls_client;
    tls_client.ca_cert_path     = fixture_path("tests/data/remote_mount_tls/ca.crt");
    tls_client.client_cert_path = fixture_path("tests/data/remote_mount_tls/client.crt");
    tls_client.client_key_path  = fixture_path("tests/data/remote_mount_tls/client.key");
    tls_client.sni_host         = "localhost";
    mount.tls                   = tls_client;

    remote.insert("/apps/demo/state", std::string{"tls-demo"});

    auto factory = makeTlsSessionFactory(std::nullopt);
    RemoteMountManager manager(make_options(local, client_metrics, mount), factory);
    manager.start();

    capture_status(manager);
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        auto statuses = manager.statuses();
        if (!statuses.empty() && statuses.front().connected) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(connected);

    auto value = local.read<std::string>("/remote/alpha/state");
    REQUIRE(value.has_value());
    CHECK(value.value() == "tls-demo");

    auto insert = local.insert("/remote/alpha/events", std::string{"tls-event"});
    CHECK(insert.errors.empty());

    auto remote_value = remote.read<std::string>("/apps/demo/events");
    REQUIRE(remote_value.has_value());
    CHECK(remote_value.value() == "tls-event");

    manager.stop();
    tls_server.stop();
}

TEST_CASE("RemoteMountManager applyValuePayload enforces payload compatibility") {
    PathSpace local;
    RemoteMountManagerOptions typed_options;
    typed_options.root_space             = &local;
    typed_options.payload_compatibility = RemotePayloadCompatibility::TypedOnly;
    RemoteMountManager typed_manager(typed_options, nullptr);

    ValuePayload payload;
    payload.encoding  = std::string{kEncodingString};
    payload.type_name = typeid(std::string).name();
    payload.data      = encode_base64(std::string_view{"legacy"});

    InputMetadata metadata{InputMetadataT<std::string>{}};
    std::string    decoded;
    auto           error = typed_manager.applyValuePayload(payload, metadata, &decoded);
    REQUIRE(error.has_value());
    CHECK(error->code == Error::Code::InvalidType);

    RemoteMountManagerOptions legacy_options = typed_options;
    legacy_options.payload_compatibility      = RemotePayloadCompatibility::LegacyCompatible;
    RemoteMountManager legacy_manager(legacy_options, nullptr);
    std::string        applied;
    auto               ok = legacy_manager.applyValuePayload(payload, metadata, &applied);
    CHECK_FALSE(ok.has_value());
    CHECK(applied == "legacy");
}

TEST_CASE("RemoteMountManager mirrorSingleNode enforces payload compatibility") {
    PathSpace local;
    RemoteMountManagerOptions typed_options;
    typed_options.root_space             = &local;
    typed_options.payload_compatibility = RemotePayloadCompatibility::TypedOnly;
    RemoteMountManager typed_manager(typed_options, nullptr);

    ValuePayload payload;
    payload.encoding  = std::string{kEncodingString};
    payload.type_name = typeid(std::string).name();
    payload.data      = encode_base64(std::string_view{"legacy"});

    auto error = typed_manager.mirrorSingleNode(local, "/apps/demo/legacy", payload);
    REQUIRE(error.has_value());
    CHECK(error->code == Error::Code::InvalidType);

    RemoteMountManagerOptions legacy_options = typed_options;
    legacy_options.payload_compatibility      = RemotePayloadCompatibility::LegacyCompatible;
    RemoteMountManager legacy_manager(legacy_options, nullptr);
    auto               legacy_error = legacy_manager.mirrorSingleNode(local, "/apps/demo/legacy", payload);
    CHECK_FALSE(legacy_error.has_value());
    auto stored = local.read<std::string>("/apps/demo/legacy");
    REQUIRE(stored.has_value());
    CHECK(stored.value() == "legacy");
}
}
