#pragma once

#include "core/Error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace SP::Distributed {

inline constexpr std::string_view kEncodingTypedSlidingBuffer{"typed/slidingbuffer"};
inline constexpr std::string_view kEncodingString{"string/base64"};
inline constexpr std::string_view kEncodingVoid{"void/sentinel"};

enum class RemotePayloadCompatibility {
    TypedOnly,
    LegacyCompatible,
};

[[nodiscard]] RemotePayloadCompatibility defaultRemotePayloadCompatibility();
[[nodiscard]] bool allowLegacyPayloads(RemotePayloadCompatibility mode);

enum class AuthKind {
    MutualTls,
    BearerToken,
};

enum class FrameKind {
    MountOpenRequest,
    MountOpenResponse,
    ReadRequest,
    ReadResponse,
    InsertRequest,
    InsertResponse,
    TakeRequest,
    TakeResponse,
    WaitSubscribeRequest,
    WaitSubscribeAck,
    Notification,
    NotificationStreamRequest,
    NotificationStreamResponse,
    Heartbeat,
    Error,
};

struct ProtocolVersion {
    std::uint16_t major{1};
    std::uint16_t minor{1};
};

struct AuthContext {
    AuthKind     kind{AuthKind::MutualTls};
    std::string  subject;
    std::string  audience;
    std::string  proof;
    std::string  fingerprint;
    std::uint64_t issued_at_ms{0};
    std::uint64_t expires_at_ms{0};
};

struct CapabilityRequest {
    std::string              name;
    std::vector<std::string> parameters;
};

struct ErrorPayload {
    std::string                code;
    std::string                message;
    bool                       retryable{false};
    std::chrono::milliseconds  retry_after{std::chrono::milliseconds{0}};
};

struct ValuePayload {
    std::string              encoding{std::string(kEncodingTypedSlidingBuffer)};
    std::string              type_name;
    std::optional<std::string> schema_hint;
    std::string              data;
};

enum class ReadConsistencyMode {
    Latest,
    AtLeastVersion,
};

struct ReadConsistency {
    ReadConsistencyMode mode{ReadConsistencyMode::Latest};
    std::optional<std::uint64_t> at_least_version;
};

struct MountOpenRequest {
    ProtocolVersion              version{};
    std::string                  request_id;
    std::string                  client_id;
    std::string                  alias;
    std::string                  export_root;
    std::vector<CapabilityRequest> capabilities;
    AuthContext                  auth;
};

struct MountOpenResponse {
    ProtocolVersion             version{};
    std::string                 request_id;
    bool                        accepted{false};
    std::string                 session_id;
    std::vector<std::string>    granted_capabilities;
    std::uint64_t               lease_expires_ms{0};
    std::chrono::milliseconds   heartbeat_interval{std::chrono::milliseconds{0}};
    std::optional<ErrorPayload> error;
};

struct ReadRequest {
    std::string                      request_id;
    std::string                      session_id;
    std::string                      path;
    bool                             include_value{true};
    bool                             include_children{false};
    bool                             include_diagnostics{false};
    std::optional<ReadConsistency>   consistency;
    std::optional<std::string>       type_name;
};

struct ReadResponse {
    std::string                 request_id;
    std::string                 path;
    std::uint64_t               version{0};
    std::optional<ValuePayload> value;
    std::vector<std::string>    children;
    bool                        children_included{false};
    std::optional<ErrorPayload> error;
};

struct InsertRequest {
    std::string request_id;
    std::string session_id;
    std::string path;
    std::string type_name;
    ValuePayload value;
};

struct InsertResponse {
    std::string                 request_id;
    bool                        success{false};
    std::uint32_t               values_inserted{0};
    std::uint32_t               spaces_inserted{0};
    std::uint32_t               tasks_inserted{0};
    std::optional<ErrorPayload> error;
};

struct TakeRequest {
    std::string               request_id;
    std::string               session_id;
    std::string               path;
    std::optional<std::string> type_name;
    std::uint32_t             max_items{1};
    bool                      do_block{false};
    std::chrono::milliseconds timeout{std::chrono::milliseconds{0}};
};

struct TakeResponse {
    std::string                 request_id;
    bool                        success{false};
    std::vector<ValuePayload>   values;
    std::optional<ErrorPayload> error;
};

struct WaitSubscriptionRequest {
    std::string                    request_id;
    std::string                    session_id;
    std::string                    subscription_id;
    std::string                    path;
    bool                           include_value{false};
    bool                           include_children{false};
    std::optional<std::uint64_t>   after_version;
};

struct WaitSubscriptionAck {
    std::string                    subscription_id;
    bool                           accepted{false};
    std::optional<ErrorPayload>    error;
};

struct Notification {
    std::string                 subscription_id;
    std::string                 path;
    std::uint64_t               version{0};
    bool                        deleted{false};
    std::optional<std::string>  type_name;
    std::optional<ValuePayload> value;
};

struct Heartbeat {
    std::string   session_id;
    std::uint64_t sequence{0};
};

struct NotificationStreamRequest {
    std::string               request_id;
    std::string               session_id;
    std::chrono::milliseconds timeout{std::chrono::milliseconds{0}};
    std::size_t               max_batch{32};
};

struct NotificationStreamResponse {
    std::string                    request_id;
    std::string                    session_id;
    std::vector<Notification>      notifications;
    std::optional<ErrorPayload>    error;
};

struct RemoteFrame {
    FrameKind                                                        kind{FrameKind::Heartbeat};
    std::chrono::milliseconds                                        sent_at{std::chrono::milliseconds{0}};
    std::variant<MountOpenRequest,
                 MountOpenResponse,
                 ReadRequest,
                 ReadResponse,
                 InsertRequest,
                 InsertResponse,
                 TakeRequest,
                 TakeResponse,
                 WaitSubscriptionRequest,
                 WaitSubscriptionAck,
                 Notification,
                 NotificationStreamRequest,
                 NotificationStreamResponse,
                 Heartbeat,
                 ErrorPayload>
        payload{Heartbeat{}};
};

[[nodiscard]] auto frameKindToString(FrameKind kind) -> std::string_view;
[[nodiscard]] auto parseFrameKind(std::string_view name) -> Expected<FrameKind>;
[[nodiscard]] auto serializeFrame(RemoteFrame const& frame) -> Expected<std::string>;
[[nodiscard]] auto deserializeFrame(std::string_view payload) -> Expected<RemoteFrame>;
[[nodiscard]] auto validateAbsolutePath(std::string_view path) -> Expected<void>;

} // namespace SP::Distributed
