#include "third_party/doctest.h"

#include <chrono>
#include <typeinfo>

#include <pathspace/distributed/RemoteMountProtocol.hpp>

using namespace SP::Distributed;
using SP::Error;

namespace {

[[nodiscard]] auto make_auth() -> AuthContext {
    AuthContext auth;
    auth.kind        = AuthKind::MutualTls;
    auth.subject     = "CN=client-alpha";
    auth.audience    = "pathspace-dev";
    auth.proof       = "sha256:fingerprint";
    auth.fingerprint = "sha256:cert";
    auth.issued_at_ms  = 100;
    auth.expires_at_ms = 200;
    return auth;
}

} // namespace

TEST_SUITE("distributed.remotemount.protocol") {
TEST_CASE("RemoteMountProtocol roundtrips MountOpenRequest frames") {
    RemoteFrame frame;
    frame.kind   = FrameKind::MountOpenRequest;
    frame.sent_at = std::chrono::milliseconds{1500};

    MountOpenRequest request;
    request.request_id = "req-1";
    request.client_id  = "client-alpha";
    request.alias      = "alpha";
    request.export_root = "/users/demo/system/applications/demo";
    request.auth        = make_auth();
    request.capabilities.push_back(CapabilityRequest{.name = "read", .parameters = {"wait"}});
    frame.payload = request;

    auto json_or = serializeFrame(frame);
    REQUIRE(json_or);

    auto parsed_or = deserializeFrame(*json_or);
    REQUIRE(parsed_or);
    CHECK_EQ(parsed_or->kind, FrameKind::MountOpenRequest);
    CHECK_EQ(parsed_or->sent_at, std::chrono::milliseconds{1500});

    auto const* parsed = std::get_if<MountOpenRequest>(&parsed_or->payload);
    REQUIRE(parsed != nullptr);
    CHECK_EQ(parsed->alias, "alpha");
    CHECK_EQ(parsed->export_root, request.export_root);
    REQUIRE_EQ(parsed->capabilities.size(), 1U);
    CHECK_EQ(parsed->capabilities.front().name, "read");
    CHECK_EQ(parsed->auth.subject, request.auth.subject);
}

TEST_CASE("RemoteMountProtocol supports read, wait, and notification frames") {
    RemoteFrame read_frame;
    read_frame.kind    = FrameKind::ReadRequest;
    read_frame.sent_at = std::chrono::milliseconds{42};

    ReadRequest read_request;
    read_request.request_id        = "req-9";
    read_request.session_id        = "sess-77";
    read_request.path              = "/remote/alpha/state";
    read_request.include_children  = true;
    read_request.include_value     = true;
    read_request.include_diagnostics = false;
    read_request.consistency = ReadConsistency{.mode = ReadConsistencyMode::AtLeastVersion,
                                               .at_least_version = std::uint64_t{12}};
    read_request.type_name = typeid(std::string).name();
    read_frame.payload     = read_request;

    auto read_json = serializeFrame(read_frame);
    REQUIRE(read_json);
    auto read_roundtrip = deserializeFrame(*read_json);
    REQUIRE(read_roundtrip);
    auto const* parsed_read = std::get_if<ReadRequest>(&read_roundtrip->payload);
    REQUIRE(parsed_read != nullptr);
    CHECK(parsed_read->consistency.has_value());
    CHECK(parsed_read->consistency->at_least_version.has_value());
    CHECK_EQ(*parsed_read->consistency->at_least_version, 12U);
    REQUIRE(parsed_read->type_name.has_value());
    CHECK_EQ(parsed_read->type_name.value(), read_request.type_name.value());

    RemoteFrame notify_frame;
    notify_frame.kind = FrameKind::Notification;
    Notification notification;
    notification.subscription_id = "sub-1";
    notification.path            = "/remote/alpha/state";
    notification.version         = 44;
    notification.deleted         = false;
    notification.type_name = std::string{typeid(std::string).name()};
    notification.value     = ValuePayload{.encoding  = std::string{kEncodingTypedSlidingBuffer},
                                      .type_name = notification.type_name.value(),
                                      .data      = "ZGVtbw=="};
    notify_frame.payload = notification;

    auto notify_json = serializeFrame(notify_frame);
    REQUIRE(notify_json);
    auto notify_roundtrip = deserializeFrame(*notify_json);
    REQUIRE(notify_roundtrip);
    auto const* parsed_notification =
        std::get_if<Notification>(&notify_roundtrip->payload);
    REQUIRE(parsed_notification != nullptr);
    CHECK(parsed_notification->value.has_value());
    REQUIRE(parsed_notification->type_name.has_value());
    CHECK_EQ(parsed_notification->type_name.value(), notification.type_name.value());
    CHECK_EQ(parsed_notification->value->data, "ZGVtbw==");
    CHECK_EQ(parsed_notification->value->type_name, notification.type_name.value());
}

TEST_CASE("RemoteMountProtocol encodes read response children") {
    ReadResponse response;
    response.request_id        = "req-child";
    response.path              = "/remote/alpha/root";
    response.version           = 5;
    response.children_included = true;
    response.children          = {"one", "two"};

    RemoteFrame frame;
    frame.kind    = FrameKind::ReadResponse;
    frame.payload = response;

    auto serialized = serializeFrame(frame);
    REQUIRE(serialized);
    auto parsed = deserializeFrame(*serialized);
    REQUIRE(parsed);
    auto const* parsed_response = std::get_if<ReadResponse>(&parsed->payload);
    REQUIRE(parsed_response != nullptr);
    CHECK(parsed_response->children_included);
    REQUIRE_EQ(parsed_response->children.size(), 2U);
    CHECK_EQ(parsed_response->children.front(), "one");
}

TEST_CASE("RemoteMountProtocol rejects relative paths") {
    RemoteFrame frame;
    frame.kind = FrameKind::ReadRequest;
    ReadRequest request;
    request.request_id = "req-err";
    request.session_id = "sess";
    request.path       = "relative/path";
    frame.payload      = request;

    auto json_or = serializeFrame(frame);
    REQUIRE_FALSE(json_or);
    CHECK_EQ(json_or.error().code, Error::Code::InvalidPath);
}

TEST_CASE("RemoteMountProtocol validates wait subscriptions") {
    RemoteFrame frame;
    frame.kind = FrameKind::WaitSubscribeRequest;

    WaitSubscriptionRequest wait_request;
    wait_request.request_id      = "req-wait";
    wait_request.session_id      = "sess-wait";
    wait_request.subscription_id = "sub-wait";
    wait_request.path            = "/remote/alpha/events";
    wait_request.include_value   = true;
    wait_request.after_version   = std::uint64_t{3};
    frame.payload                = wait_request;

    auto serialized = serializeFrame(frame);
    REQUIRE(serialized);
    auto parsed = deserializeFrame(*serialized);
    REQUIRE(parsed);
    auto const* parsed_wait = std::get_if<WaitSubscriptionRequest>(&parsed->payload);
    REQUIRE(parsed_wait != nullptr);
    CHECK(parsed_wait->after_version.has_value());
    CHECK_EQ(*parsed_wait->after_version, 3U);
}
}
