#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/distributed/RemoteMountServer.hpp>

#include <pathspace/core/NodeData.hpp>
#include <pathspace/type/InputMetadataT.hpp>
#include <pathspace/type/SlidingBuffer.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

using namespace SP;
using namespace SP::Distributed;

namespace {

[[nodiscard]] auto make_auth() -> AuthContext {
    AuthContext auth;
    auth.kind        = AuthKind::MutualTls;
    auth.subject     = "CN=client-alpha";
    auth.audience    = "pathspace-dev";
    auth.proof       = "sha256:fingerprint";
    auth.fingerprint = "sha256:cert";
    auth.issued_at_ms  = 10;
    auth.expires_at_ms = 10'000;
    return auth;
}

[[nodiscard]] auto decode_base64(std::string const& input) -> std::vector<std::byte> {
    static constexpr char kPad = '=';
    auto decode_char = [](char ch) -> int {
        if ('A' <= ch && ch <= 'Z') return ch - 'A';
        if ('a' <= ch && ch <= 'z') return ch - 'a' + 26;
        if ('0' <= ch && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };

    std::vector<std::byte> output;
    output.reserve((input.size() * 3) / 4);
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
        output.push_back(static_cast<std::byte>((chunk[0] << 2) | ((chunk[1] & 0x30) >> 4)));
        if (chunk[2] >= 0) {
            output.push_back(static_cast<std::byte>(((chunk[1] & 0x0F) << 4) | ((chunk[2] & 0x3C) >> 2)));
            if (chunk[3] >= 0) {
                output.push_back(static_cast<std::byte>(((chunk[2] & 0x03) << 6) | chunk[3]));
            }
        }
    }
    return output;
}

[[nodiscard]] auto encode_base64(std::string_view input) -> std::string {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((input.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    auto        span  = std::span<const std::byte>(reinterpret_cast<std::byte const*>(input.data()),
                                                input.size());
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

template <typename T>
[[nodiscard]] auto encode_typed_payload(T const& value) -> std::string {
    SlidingBuffer buffer;
    InputMetadata metadata{InputMetadataT<T>{}};
    metadata.serialize(&value, buffer);
    std::string_view raw(reinterpret_cast<char const*>(buffer.data()), buffer.size());
    return encode_base64(raw);
}

template <typename T>
[[nodiscard]] auto decode_typed_payload(ValuePayload const& payload) -> T {
    REQUIRE(payload.encoding == kEncodingTypedSlidingBuffer);
    auto bytes = decode_base64(payload.data);
    std::vector<std::uint8_t> raw(bytes.size());
    std::memcpy(raw.data(), bytes.data(), bytes.size());
    SlidingBuffer buffer;
    buffer.assignRaw(std::move(raw), 0);
    InputMetadata metadata{InputMetadataT<T>{}};
    T             value{};
    metadata.deserialize(&value, buffer);
    return value;
}

[[nodiscard]] auto decode_string(ValuePayload const& payload) -> std::string {
    auto bytes = decode_base64(payload.data);
    if (payload.encoding == kEncodingString) {
        return std::string(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    }
    if (payload.encoding == kEncodingTypedSlidingBuffer) {
        return decode_typed_payload<std::string>(payload);
    }
    auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!node.has_value()) {
        return std::string(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    }
    std::string                 value;
    InputMetadataT<std::string> metadata;
    auto                        error = node->deserialize(&value, metadata);
    REQUIRE_FALSE(error.has_value());
    return value;
}

auto make_server(PathSpace&                                   space,
                 PathSpace&                                   metrics,
                 PathSpace&                                   diagnostics,
                 std::optional<RemoteMountThrottleOptions>   throttle      = std::nullopt,
                 std::optional<RemotePayloadCompatibility> compatibility = std::nullopt)
    -> std::shared_ptr<RemoteMountServer> {
    RemoteMountServerOptions options;
    RemoteMountExportOptions export_opts;
    export_opts.alias       = "alpha";
    export_opts.export_root = "/apps/demo";
    export_opts.space       = &space;
    export_opts.capabilities = {"read", "wait", "insert", "take"};
    if (throttle) {
        export_opts.throttle = *throttle;
    }
    options.exports.push_back(export_opts);
    options.metrics_space     = &metrics;
    options.diagnostics_space = &diagnostics;
    if (compatibility.has_value()) {
        options.payload_compatibility = *compatibility;
    }
    return std::make_shared<RemoteMountServer>(options);
}

auto open_session(RemoteMountServer& server) -> MountOpenResponse {
    MountOpenRequest request;
    request.version     = ProtocolVersion{1, 0};
    request.request_id  = "req-1";
    request.client_id   = "client";
    request.alias       = "alpha";
    request.export_root = "/apps/demo";
    request.auth        = make_auth();
    request.capabilities.push_back(CapabilityRequest{.name = "read"});
    request.capabilities.push_back(CapabilityRequest{.name = "wait"});
    request.capabilities.push_back(CapabilityRequest{.name = "insert"});
    request.capabilities.push_back(CapabilityRequest{.name = "take"});
    INFO("requested capabilities: " << request.capabilities.size());
    auto response       = server.handleMountOpen(request);
    REQUIRE(response);
    REQUIRE_FALSE(response->granted_capabilities.empty());
    CHECK(std::find(response->granted_capabilities.begin(),
                    response->granted_capabilities.end(),
                    std::string{"insert"})
          != response->granted_capabilities.end());
    return *response;
}

} // namespace

TEST_CASE("RemoteMountServer handles mount open and read value") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    space.insert("/apps/demo/state", std::string{"demo"});

    auto session = open_session(*server);

    ReadRequest read;
    read.request_id        = "read-1";
    read.session_id        = session.session_id;
    read.path              = "/apps/demo/state";
    read.include_value     = true;
    read.include_children  = true;

    auto response = server->handleRead(read);
    REQUIRE(response);
    REQUIRE(response->value.has_value());
    CHECK_EQ(response->value->encoding, kEncodingTypedSlidingBuffer);
    CHECK_EQ(response->value->type_name, std::string(typeid(std::string).name()));
    CHECK_EQ(decode_string(*response->value), "demo");
    CHECK(response->children_included);
}

TEST_CASE("RemoteMountServer queues wait notifications") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    WaitSubscriptionRequest wait_req;
    wait_req.request_id      = "wait-1";
    wait_req.session_id      = session.session_id;
    wait_req.subscription_id = "sub-1";
    wait_req.path            = "/apps/demo/state";
    wait_req.include_value   = true;

    auto ack = server->handleWaitSubscribe(wait_req);
    REQUIRE(ack);
    CHECK(ack->accepted);

    space.insert("/apps/demo/state", std::string{"first"});

    auto notification = server->nextNotification("sub-1");
    INFO("subscription queue state: " << (notification ? "notification present" : "notification missing"));
    REQUIRE(notification.has_value());
    CHECK_EQ(notification->path, "/apps/demo/state");
    CHECK(notification->value.has_value());
    REQUIRE(notification->type_name.has_value());
    CHECK_EQ(notification->type_name.value(), std::string(typeid(std::string).name()));
    CHECK_EQ(decode_string(*notification->value), "first");
}

TEST_CASE("RemoteMountServer streams notifications via session queue") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    WaitSubscriptionRequest wait_req;
    wait_req.request_id      = "wait-stream";
    wait_req.session_id      = session.session_id;
    wait_req.subscription_id = "sub-stream";
    wait_req.path            = "/apps/demo/state";
    wait_req.include_value   = true;

    auto ack = server->handleWaitSubscribe(wait_req);
    REQUIRE(ack);
    CHECK(ack->accepted);

    space.insert("/apps/demo/state", std::string{"stream"});

    auto batch = server->handleNotificationStream(session.session_id,
                                                  std::chrono::milliseconds{100},
                                                  4);
    REQUIRE(batch);
    REQUIRE_EQ(batch->size(), 1);
    CHECK_EQ(batch->front().subscription_id, "sub-stream");
    REQUIRE(batch->front().value.has_value());
    REQUIRE(batch->front().type_name.has_value());
    CHECK_EQ(batch->front().type_name.value(), std::string(typeid(std::string).name()));
    CHECK_EQ(decode_string(*batch->front().value), "stream");
}

TEST_CASE("RemoteMountServer throttles wait subscriptions when backlog grows") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    WaitSubscriptionRequest wait_req;
    wait_req.request_id      = "wait-flood";
    wait_req.session_id      = session.session_id;
    wait_req.subscription_id = "sub-flood";
    wait_req.path            = "/apps/demo/state";
    wait_req.include_value   = true;

    auto ack = server->handleWaitSubscribe(wait_req);
    REQUIRE(ack);
    CHECK(ack->accepted);

    for (int idx = 0; idx < 256; ++idx) {
        space.insert("/apps/demo/state", std::string{"value-"} + std::to_string(idx));
    }

    WaitSubscriptionRequest wait_req2;
    wait_req2.request_id      = "wait-block";
    wait_req2.session_id      = session.session_id;
    wait_req2.subscription_id = "sub-block";
    wait_req2.path            = "/apps/demo/state";
    wait_req2.include_value   = true;

    auto ack2 = server->handleWaitSubscribe(wait_req2);
    REQUIRE(ack2);
    CHECK_FALSE(ack2->accepted);
    REQUIRE(ack2->error.has_value());
    CHECK_EQ(ack2->error->code, "notify_backpressure");
    CHECK(ack2->error->retryable);
    CHECK_GT(ack2->error->retry_after.count(), 0);
}

TEST_CASE("RemoteMountServer enforces per-session waiter cap") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    RemoteMountThrottleOptions throttle;
    throttle.enabled                = false;
    throttle.max_waiters_per_session = 1;
    throttle.wait_retry_after        = std::chrono::milliseconds{750};
    auto server = make_server(space, metrics, diagnostics, throttle);

    auto session = open_session(*server);

    WaitSubscriptionRequest first;
    first.request_id      = "cap-allow";
    first.session_id      = session.session_id;
    first.subscription_id = "cap-allow";
    first.path            = "/apps/demo/state";
    first.include_value   = true;

    auto ack1 = server->handleWaitSubscribe(first);
    REQUIRE(ack1);
    CHECK(ack1->accepted);

    WaitSubscriptionRequest second;
    second.request_id      = "cap-block";
    second.session_id      = session.session_id;
    second.subscription_id = "cap-block";
    second.path            = "/apps/demo/state";
    second.include_value   = true;

    auto ack2 = server->handleWaitSubscribe(second);
    REQUIRE(ack2);
    CHECK_FALSE(ack2->accepted);
    REQUIRE(ack2->error.has_value());
    CHECK_EQ(ack2->error->code, "too_many_waiters");
    CHECK(ack2->error->retryable);
    CHECK_EQ(ack2->error->retry_after, throttle.wait_retry_after);

    server->dropSubscription("cap-allow");

    WaitSubscriptionRequest third;
    third.request_id      = "cap-second";
    third.session_id      = session.session_id;
    third.subscription_id = "cap-second";
    third.path            = "/apps/demo/state";
    third.include_value   = true;

    auto ack3 = server->handleWaitSubscribe(third);
    REQUIRE(ack3);
    CHECK(ack3->accepted);
}

TEST_CASE("RemoteMountServer acknowledges void sentinel inserts") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server  = make_server(space, metrics, diagnostics);
    auto      session = open_session(*server);

    InsertRequest insert;
    insert.request_id = "insert-void";
    insert.session_id = session.session_id;
    insert.path       = "/apps/demo/void";
    insert.value.encoding  = std::string{kEncodingVoid};
    insert.value.type_name = "void";

    auto response = server->handleInsert(insert);
    REQUIRE(response);
    CHECK(response->success);
    CHECK(response->tasks_inserted == 1);
    CHECK(response->values_inserted == 0);

    auto remote_value = space.read<std::string>("/apps/demo/void");
    CHECK(!remote_value.has_value());
}

TEST_CASE("RemoteMountServer handles string insert/take") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    auto const remote_value = std::string{"remote"};

    InsertRequest insert;
    insert.request_id     = "insert-1";
    insert.session_id     = session.session_id;
    insert.path           = "/apps/demo/state";
    insert.type_name      = typeid(std::string).name();
    insert.value.encoding = std::string{kEncodingTypedSlidingBuffer};
    insert.value.type_name = insert.type_name;
    insert.value.data      = encode_typed_payload(remote_value);

    auto insert_response = server->handleInsert(insert);
    INFO("insert result: " << (insert_response ? std::string{"ok"} : SP::describeError(insert_response.error())));
    REQUIRE(insert_response);
    CHECK(insert_response->success);

    auto stored = space.read<std::string>("/apps/demo/state");
    REQUIRE(stored.has_value());
    CHECK(stored.value() == "remote");

    TakeRequest take;
    take.request_id = "take-1";
    take.session_id = session.session_id;
    take.path       = "/apps/demo/state";
    take.type_name  = typeid(std::string).name();

    auto take_response = server->handleTake(take);
    REQUIRE(take_response);
    CHECK(take_response->success);
    REQUIRE_FALSE(take_response->values.empty());
    auto const& payload_value = take_response->values.front();
    CHECK_EQ(payload_value.type_name, insert.type_name);
    CHECK_EQ(payload_value.encoding, kEncodingTypedSlidingBuffer);
    CHECK_EQ(decode_string(payload_value), remote_value);

    auto missing = space.read<std::string>("/apps/demo/state");
    CHECK(!missing.has_value());
}

TEST_CASE("RemoteMountServer rejects legacy payloads when typed-only mode enabled") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    InsertRequest request;
    request.request_id     = "legacy-typed";
    request.session_id     = session.session_id;
    request.path           = "/apps/demo/legacy";
    request.type_name      = typeid(std::string).name();
    request.value.encoding = std::string{kEncodingString};
    request.value.type_name = request.type_name;
    request.value.data      = encode_base64(std::string_view{"legacy"});

    auto insert_response = server->handleInsert(request);
    REQUIRE_FALSE(insert_response);
    CHECK(insert_response.error().code == Error::Code::InvalidType);
}

TEST_CASE("RemoteMountServer accepts legacy payloads when compatibility enabled") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space,
                              metrics,
                              diagnostics,
                              std::nullopt,
                              RemotePayloadCompatibility::LegacyCompatible);

    auto session = open_session(*server);

    InsertRequest request;
    request.request_id     = "legacy-allowed";
    request.session_id     = session.session_id;
    request.path           = "/apps/demo/legacy";
    request.type_name      = typeid(std::string).name();
    request.value.encoding = std::string{kEncodingString};
    request.value.type_name = request.type_name;
    request.value.data      = encode_base64(std::string_view{"legacy"});

    auto insert_response = server->handleInsert(request);
    REQUIRE(insert_response);
    CHECK(insert_response->success);

    auto stored = space.read<std::string>("/apps/demo/legacy");
    REQUIRE(stored.has_value());
    CHECK(stored.value() == "legacy");
}

TEST_CASE("RemoteMountServer handles serialized NodeData insert") {
    PathSpace space;
    PathSpace metrics;
    PathSpace diagnostics;
    auto      server = make_server(space, metrics, diagnostics);

    auto session = open_session(*server);

    int counter = 42;

    InsertRequest request;
    request.request_id        = "insert-nd";
    request.session_id        = session.session_id;
    request.path              = "/apps/demo/counter";
    request.type_name         = typeid(int).name();
    request.value.encoding  = std::string{kEncodingTypedSlidingBuffer};
    request.value.type_name = request.type_name;
    request.value.data      = encode_typed_payload(counter);

    auto insert_response = server->handleInsert(request);
    INFO("insert result: " << (insert_response ? std::string{"ok"} : SP::describeError(insert_response.error())));
    REQUIRE(insert_response);
    CHECK(insert_response->success);

    auto stored = space.read<int>("/apps/demo/counter");
    REQUIRE(stored.has_value());
    CHECK(stored.value() == counter);

    TakeRequest take;
    take.request_id = "take-nd";
    take.session_id = session.session_id;
    take.path       = "/apps/demo/counter";
    take.type_name  = typeid(int).name();

    auto take_response = server->handleTake(take);
    REQUIRE(take_response);
    CHECK(take_response->success);
    REQUIRE_FALSE(take_response->values.empty());
    auto const& payload_value = take_response->values.front();
    CHECK_EQ(payload_value.type_name, request.type_name);
    CHECK_EQ(payload_value.encoding, kEncodingTypedSlidingBuffer);
    auto extracted = decode_typed_payload<int>(payload_value);
    CHECK_EQ(extracted, counter);
}
