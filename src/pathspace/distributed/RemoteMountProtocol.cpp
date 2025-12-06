#include "pathspace/distributed/RemoteMountProtocol.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace SP::Distributed {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kMaxTakeBatch = 64;

[[nodiscard]] auto normalize_flag(std::string_view raw) -> std::string {
    std::string normalized;
    normalized.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (std::isspace(ch) != 0) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

[[nodiscard]] auto read_typed_payload_env() -> RemotePayloadCompatibility {
    if (auto* raw = std::getenv("PATHSPACE_REMOTE_TYPED_PAYLOADS")) {
        auto normalized = normalize_flag(raw);
        if (normalized.empty()) {
            return RemotePayloadCompatibility::TypedOnly;
        }
        if (normalized == "0" || normalized == "false" || normalized == "legacy"
            || normalized == "compat" || normalized == "compatibility") {
            return RemotePayloadCompatibility::LegacyCompatible;
        }
        if (normalized == "1" || normalized == "true" || normalized == "typed") {
            return RemotePayloadCompatibility::TypedOnly;
        }
    }
    return RemotePayloadCompatibility::TypedOnly;
}

[[nodiscard]] auto make_error(Error::Code code, std::string_view message) -> Error {
    return Error{code, std::string(message)};
}

[[nodiscard]] auto make_error(Error::Code code,
                              std::string_view field,
                              std::string_view detail) -> Error {
    std::string message;
    message.reserve(field.size() + detail.size() + 2);
    message.append(field);
    if (!detail.empty()) {
        message.append(": ");
        message.append(detail);
    }
    return Error{code, std::move(message)};
}

[[nodiscard]] auto ensure_non_empty(std::string_view value, std::string_view field) -> Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, field, "must not be empty"));
    }
    return {};
}

[[nodiscard]] auto validate_alias(std::string_view alias) -> Expected<void> {
    if (alias.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "alias", "must not be empty"));
    }
    for (unsigned char ch : alias) {
        if (std::isalnum(ch) != 0 || ch == '_' || ch == '-') {
            continue;
        }
        return std::unexpected(make_error(Error::Code::MalformedInput, "alias",
                                          "contains invalid characters"));
    }
    return {};
}

[[nodiscard]] auto validate_identifier(std::string_view value, std::string_view field) -> Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, field, "must not be empty"));
    }
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == ':' || ch == '.') {
            continue;
        }
        return std::unexpected(make_error(Error::Code::MalformedInput, field,
                                          "contains invalid characters"));
    }
    return {};
}

[[nodiscard]] auto is_control(char ch) -> bool {
    return std::iscntrl(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] auto ensure_object(Json const& json, std::string_view context) -> Expected<void> {
    if (!json.is_object()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, context,
                                          "must be a JSON object"));
    }
    return {};
}

[[nodiscard]] auto read_string(Json const& json, char const* key) -> Expected<std::string> {
    if (auto it = json.find(key); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(make_error(Error::Code::MalformedInput, key, "must be a string"));
        }
        return it->get<std::string>();
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, key, "is required"));
}

[[nodiscard]] auto read_optional_string(Json const& json, char const* key)
    -> std::optional<std::string> {
    if (auto it = json.find(key); it != json.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

[[nodiscard]] auto read_boolean(Json const& json, char const* key, bool default_value)
    -> Expected<bool> {
    if (auto it = json.find(key); it != json.end()) {
        if (!it->is_boolean()) {
            return std::unexpected(make_error(Error::Code::MalformedInput, key, "must be a bool"));
        }
        return it->get<bool>();
    }
    return default_value;
}

[[nodiscard]] auto read_uint64(Json const& json, char const* key) -> Expected<std::uint64_t> {
    if (auto it = json.find(key); it != json.end()) {
        if (it->is_number_unsigned()) {
            return it->get<std::uint64_t>();
        }
        if (it->is_number_integer()) {
            auto value = it->get<std::int64_t>();
            if (value < 0) {
                return std::unexpected(
                    make_error(Error::Code::MalformedInput, key, "must be non-negative"));
            }
            return static_cast<std::uint64_t>(value);
        }
        return std::unexpected(make_error(Error::Code::MalformedInput, key, "must be an integer"));
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, key, "is required"));
}

[[nodiscard]] auto read_optional_uint64(Json const& json, char const* key)
    -> Expected<std::optional<std::uint64_t>> {
    if (auto it = json.find(key); it != json.end()) {
        if (it->is_null()) {
            return std::optional<std::uint64_t>{std::nullopt};
        }
        if (it->is_number_unsigned()) {
            return std::optional<std::uint64_t>{it->get<std::uint64_t>()};
        }
        if (it->is_number_integer()) {
            auto value = it->get<std::int64_t>();
            if (value < 0) {
                return std::unexpected(make_error(Error::Code::MalformedInput, key,
                                                  "must be non-negative"));
            }
            return std::optional<std::uint64_t>{static_cast<std::uint64_t>(value)};
        }
        return std::unexpected(make_error(Error::Code::MalformedInput, key, "must be an integer"));
    }
    return std::optional<std::uint64_t>{std::nullopt};
}

[[nodiscard]] auto to_uint64(std::chrono::milliseconds value) -> Expected<std::uint64_t> {
    if (value.count() < 0) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "duration", "must be non-negative"));
    }
    return static_cast<std::uint64_t>(value.count());
}

[[nodiscard]] auto parse_duration(Json const& json, char const* key)
    -> Expected<std::chrono::milliseconds> {
    auto value = read_uint64(json, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(*value)};
}

[[nodiscard]] auto parse_optional_duration(Json const& json, char const* key)
    -> Expected<std::optional<std::chrono::milliseconds>> {
    auto value = read_optional_uint64(json, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!value->has_value()) {
        return std::optional<std::chrono::milliseconds>{std::nullopt};
    }
    return std::optional<std::chrono::milliseconds>{
        std::chrono::milliseconds{static_cast<std::int64_t>(value->value())}};
}

[[nodiscard]] auto auth_kind_to_string(AuthKind kind) -> std::string_view {
    switch (kind) {
    case AuthKind::MutualTls:
        return "mtls";
    case AuthKind::BearerToken:
        return "bearer";
    }
    return "mtls";
}

[[nodiscard]] auto parse_auth_kind(std::string_view name) -> Expected<AuthKind> {
    if (name == "mtls") {
        return AuthKind::MutualTls;
    }
    if (name == "bearer") {
        return AuthKind::BearerToken;
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, "auth.kind",
                                      "must be 'mtls' or 'bearer'"));
}

[[nodiscard]] auto consistency_mode_to_string(ReadConsistencyMode mode) -> std::string_view {
    switch (mode) {
    case ReadConsistencyMode::Latest:
        return "latest";
    case ReadConsistencyMode::AtLeastVersion:
        return "at_least_version";
    }
    return "latest";
}

[[nodiscard]] auto parse_consistency_mode(std::string_view name) -> Expected<ReadConsistencyMode> {
    if (name == "latest") {
        return ReadConsistencyMode::Latest;
    }
    if (name == "at_least_version") {
        return ReadConsistencyMode::AtLeastVersion;
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, "consistency.mode",
                                      "must be 'latest' or 'at_least_version'"));
}

[[nodiscard]] auto version_to_json(ProtocolVersion const& version) -> Json {
    return Json{{"major", version.major}, {"minor", version.minor}};
}

[[nodiscard]] auto version_from_json(Json const& json) -> Expected<ProtocolVersion> {
    if (!json.is_object()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "version",
                                          "must be an object"));
    }
    ProtocolVersion version{};
    version.major = static_cast<std::uint16_t>(json.value("major", 0));
    version.minor = static_cast<std::uint16_t>(json.value("minor", 0));
    return version;
}

[[nodiscard]] auto auth_to_json(AuthContext const& auth) -> Expected<Json> {
    auto subject_check = ensure_non_empty(auth.subject, "auth.subject");
    if (!subject_check) {
        return std::unexpected(subject_check.error());
    }
    auto proof_check = ensure_non_empty(auth.proof, "auth.proof");
    if (!proof_check) {
        return std::unexpected(proof_check.error());
    }
    Json json{{"kind", auth_kind_to_string(auth.kind)},
              {"subject", auth.subject},
              {"proof", auth.proof},
              {"issued_at_ms", auth.issued_at_ms},
              {"expires_at_ms", auth.expires_at_ms}};
    if (!auth.audience.empty()) {
        json["audience"] = auth.audience;
    }
    if (!auth.fingerprint.empty()) {
        json["fingerprint"] = auth.fingerprint;
    }
    return json;
}

[[nodiscard]] auto auth_from_json(Json const& json) -> Expected<AuthContext> {
    auto ensure = ensure_object(json, "auth");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    AuthContext auth{};
    auto kind_str = read_string(json, "kind");
    if (!kind_str) {
        return std::unexpected(kind_str.error());
    }
    auto kind = parse_auth_kind(*kind_str);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    auth.kind = *kind;
    auto subject = read_string(json, "subject");
    if (!subject) {
        return std::unexpected(subject.error());
    }
    auth.subject = std::move(*subject);
    auth.audience = json.value("audience", std::string{});
    auto proof = read_string(json, "proof");
    if (!proof) {
        return std::unexpected(proof.error());
    }
    auth.proof = std::move(*proof);
    auth.fingerprint = json.value("fingerprint", std::string{});
    auth.issued_at_ms = json.value("issued_at_ms", static_cast<std::uint64_t>(0));
    auth.expires_at_ms = json.value("expires_at_ms", static_cast<std::uint64_t>(0));
    return auth;
}

[[nodiscard]] auto capability_to_json(CapabilityRequest const& capability) -> Expected<Json> {
    auto check = ensure_non_empty(capability.name, "capability.name");
    if (!check) {
        return std::unexpected(check.error());
    }
    Json json{{"name", capability.name}};
    Json params = Json::array();
    for (auto const& parameter : capability.parameters) {
        params.push_back(parameter);
    }
    json["parameters"] = std::move(params);
    return json;
}

[[nodiscard]] auto capability_from_json(Json const& json) -> Expected<CapabilityRequest> {
    auto ensure = ensure_object(json, "capability");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    CapabilityRequest capability;
    auto name = read_string(json, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    capability.name = std::move(*name);
    if (auto it = json.find("parameters"); it != json.end()) {
        if (!it->is_array()) {
            return std::unexpected(make_error(Error::Code::MalformedInput, "parameters",
                                              "must be an array"));
        }
        for (auto const& value : *it) {
            if (!value.is_string()) {
                return std::unexpected(
                    make_error(Error::Code::MalformedInput, "parameters",
                               "must contain only strings"));
            }
            capability.parameters.push_back(value.get<std::string>());
        }
    }
    return capability;
}

[[nodiscard]] auto error_to_json(ErrorPayload const& error) -> Expected<Json> {
    auto code_check = ensure_non_empty(error.code, "error.code");
    if (!code_check) {
        return std::unexpected(code_check.error());
    }
    Json json{{"code", error.code}, {"message", error.message}, {"retryable", error.retryable}};
    if (auto retry = to_uint64(error.retry_after); retry) {
        if (*retry > 0) {
            json["retry_after_ms"] = *retry;
        }
    }
    return json;
}

[[nodiscard]] auto error_from_json(Json const& json) -> Expected<ErrorPayload> {
    auto ensure = ensure_object(json, "error");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    ErrorPayload error{};
    auto code = read_string(json, "code");
    if (!code) {
        return std::unexpected(code.error());
    }
    error.code = std::move(*code);
    error.message = json.value("message", std::string{});
    auto retryable = read_boolean(json, "retryable", false);
    if (!retryable) {
        return std::unexpected(retryable.error());
    }
    error.retryable = *retryable;
    auto retry_after = parse_optional_duration(json, "retry_after_ms");
    if (!retry_after) {
        return std::unexpected(retry_after.error());
    }
    if (retry_after->has_value()) {
        error.retry_after = retry_after->value();
    }
    return error;
}

[[nodiscard]] auto value_to_json(ValuePayload const& value) -> Expected<Json> {
    auto encoding_check = ensure_non_empty(value.encoding, "value.encoding");
    if (!encoding_check) {
        return std::unexpected(encoding_check.error());
    }
    auto type_check = ensure_non_empty(value.type_name, "value.type_name");
    if (!type_check) {
        return std::unexpected(type_check.error());
    }
    Json json{{"encoding", value.encoding},
              {"data", value.data},
              {"type_name", value.type_name}};
    if (value.schema_hint && !value.schema_hint->empty()) {
        json["schema_hint"] = *value.schema_hint;
    }
    return json;
}

[[nodiscard]] auto value_from_json(Json const& json) -> Expected<ValuePayload> {
    auto ensure = ensure_object(json, "value");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    ValuePayload value{};
    auto encoding = read_string(json, "encoding");
    if (!encoding) {
        return std::unexpected(encoding.error());
    }
    value.encoding = std::move(*encoding);
    auto data = read_string(json, "data");
    if (!data) {
        return std::unexpected(data.error());
    }
    value.data = std::move(*data);
    auto type_name = read_string(json, "type_name");
    if (!type_name) {
        return std::unexpected(type_name.error());
    }
    value.type_name = std::move(*type_name);
    if (auto it = json.find("schema_hint"); it != json.end() && it->is_string()) {
        value.schema_hint = it->get<std::string>();
    }
    return value;
}

[[nodiscard]] auto consistency_to_json(ReadConsistency const& consistency) -> Json {
    Json json{{"mode", consistency_mode_to_string(consistency.mode)}};
    if (consistency.mode == ReadConsistencyMode::AtLeastVersion && consistency.at_least_version) {
        json["version"] = *consistency.at_least_version;
    }
    return json;
}

[[nodiscard]] auto consistency_from_json(Json const& json) -> Expected<ReadConsistency> {
    auto ensure = ensure_object(json, "consistency");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    ReadConsistency consistency;
    auto mode_str = read_string(json, "mode");
    if (!mode_str) {
        return std::unexpected(mode_str.error());
    }
    auto mode = parse_consistency_mode(*mode_str);
    if (!mode) {
        return std::unexpected(mode.error());
    }
    consistency.mode = *mode;
    if (consistency.mode == ReadConsistencyMode::AtLeastVersion) {
        auto version = read_uint64(json, "version");
        if (!version) {
            return std::unexpected(version.error());
        }
        consistency.at_least_version = *version;
    }
    return consistency;
}

[[nodiscard]] auto mount_open_request_to_json(MountOpenRequest const& request) -> Expected<Json> {
    auto alias_check = validate_alias(request.alias);
    if (!alias_check) {
        return std::unexpected(alias_check.error());
    }
    auto root_check = validateAbsolutePath(request.export_root);
    if (!root_check) {
        return std::unexpected(root_check.error());
    }
    auto client_check = ensure_non_empty(request.client_id, "client_id");
    if (!client_check) {
        return std::unexpected(client_check.error());
    }
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    Json json{{"version", version_to_json(request.version)},
              {"request_id", request.request_id},
              {"client_id", request.client_id},
              {"mount_alias", request.alias},
              {"export_root", request.export_root}};
    Json caps = Json::array();
    for (auto const& capability : request.capabilities) {
        auto cap_json = capability_to_json(capability);
        if (!cap_json) {
            return std::unexpected(cap_json.error());
        }
        caps.push_back(std::move(*cap_json));
    }
    json["capabilities"] = std::move(caps);
    auto auth_json = auth_to_json(request.auth);
    if (!auth_json) {
        return std::unexpected(auth_json.error());
    }
    json["auth"] = std::move(*auth_json);
    return json;
}

[[nodiscard]] auto mount_open_request_from_json(Json const& json) -> Expected<MountOpenRequest> {
    auto ensure = ensure_object(json, "MountOpenRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    MountOpenRequest request;
    auto version = version_from_json(json.value("version", Json::object()));
    if (!version) {
        return std::unexpected(version.error());
    }
    request.version = *version;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto client_id = read_string(json, "client_id");
    if (!client_id) {
        return std::unexpected(client_id.error());
    }
    request.client_id = std::move(*client_id);
    auto alias = read_string(json, "mount_alias");
    if (!alias) {
        return std::unexpected(alias.error());
    }
    request.alias = std::move(*alias);
    auto root = read_string(json, "export_root");
    if (!root) {
        return std::unexpected(root.error());
    }
    request.export_root = std::move(*root);
    if (auto caps = json.find("capabilities"); caps != json.end()) {
        if (!caps->is_array()) {
            return std::unexpected(make_error(Error::Code::MalformedInput, "capabilities",
                                              "must be an array"));
        }
        for (auto const& cap_json : *caps) {
            auto capability = capability_from_json(cap_json);
            if (!capability) {
                return std::unexpected(capability.error());
            }
            request.capabilities.push_back(std::move(*capability));
        }
    }
    auto auth_json = json.find("auth");
    if (auth_json == json.end()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "auth", "is required"));
    }
    auto auth = auth_from_json(*auth_json);
    if (!auth) {
        return std::unexpected(auth.error());
    }
    request.auth = std::move(*auth);
    auto alias_check = validate_alias(request.alias);
    if (!alias_check) {
        return std::unexpected(alias_check.error());
    }
    auto root_check = validateAbsolutePath(request.export_root);
    if (!root_check) {
        return std::unexpected(root_check.error());
    }
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    return request;
}

[[nodiscard]] auto mount_open_response_to_json(MountOpenResponse const& response) -> Expected<Json> {
    auto request_id_check = validate_identifier(response.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    Json json{{"version", version_to_json(response.version)},
              {"request_id", response.request_id},
              {"accepted", response.accepted},
              {"session_id", response.session_id},
              {"lease_expires_ms", response.lease_expires_ms}};
    auto interval = to_uint64(response.heartbeat_interval);
    if (!interval) {
        return std::unexpected(interval.error());
    }
    json["heartbeat_interval_ms"] = *interval;
    Json granted = Json::array();
    for (auto const& capability : response.granted_capabilities) {
        granted.push_back(capability);
    }
    json["granted_capabilities"] = std::move(granted);
    if (response.error) {
        auto error_json = error_to_json(*response.error);
        if (!error_json) {
            return std::unexpected(error_json.error());
        }
        json["error"] = std::move(*error_json);
    }
    return json;
}

[[nodiscard]] auto mount_open_response_from_json(Json const& json) -> Expected<MountOpenResponse> {
    auto ensure = ensure_object(json, "MountOpenResponse");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    MountOpenResponse response;
    auto version = version_from_json(json.value("version", Json::object()));
    if (!version) {
        return std::unexpected(version.error());
    }
    response.version = *version;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    response.request_id = std::move(*request_id);
    auto accepted = read_boolean(json, "accepted", false);
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    response.accepted = *accepted;
    response.session_id = json.value("session_id", std::string{});
    response.lease_expires_ms = json.value("lease_expires_ms", static_cast<std::uint64_t>(0));
    auto heartbeat = read_uint64(json, "heartbeat_interval_ms");
    if (!heartbeat) {
        return std::unexpected(heartbeat.error());
    }
    response.heartbeat_interval = std::chrono::milliseconds{static_cast<std::int64_t>(*heartbeat)};
    if (auto granted = json.find("granted_capabilities"); granted != json.end()) {
        if (!granted->is_array()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "granted_capabilities", "must be an array"));
        }
        for (auto const& cap : *granted) {
            if (!cap.is_string()) {
                return std::unexpected(make_error(Error::Code::MalformedInput,
                                                  "granted_capabilities",
                                                  "must contain only strings"));
            }
            response.granted_capabilities.push_back(cap.get<std::string>());
        }
    }
    if (auto error_json = json.find("error"); error_json != json.end()) {
        auto error = error_from_json(*error_json);
        if (!error) {
            return std::unexpected(error.error());
        }
        response.error = std::move(*error);
    }
    auto request_id_check = validate_identifier(response.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    return response;
}

[[nodiscard]] auto read_request_to_json(ReadRequest const& request) -> Expected<Json> {
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    Json json{{"request_id", request.request_id},
              {"session_id", request.session_id},
              {"path", request.path},
              {"include_value", request.include_value},
              {"include_children", request.include_children},
              {"include_diagnostics", request.include_diagnostics}};
    if (request.type_name && !request.type_name->empty()) {
        json["type_name"] = *request.type_name;
    }
    if (request.consistency) {
        json["consistency"] = consistency_to_json(*request.consistency);
    }
    return json;
}

[[nodiscard]] auto read_request_from_json(Json const& json) -> Expected<ReadRequest> {
    auto ensure = ensure_object(json, "ReadRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    ReadRequest request;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    request.session_id = std::move(*session_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    request.path = std::move(*path);
    auto include_value = read_boolean(json, "include_value", true);
    if (!include_value) {
        return std::unexpected(include_value.error());
    }
    request.include_value = *include_value;
    auto include_children = read_boolean(json, "include_children", false);
    if (!include_children) {
        return std::unexpected(include_children.error());
    }
    request.include_children = *include_children;
    auto include_diag = read_boolean(json, "include_diagnostics", false);
    if (!include_diag) {
        return std::unexpected(include_diag.error());
    }
    request.include_diagnostics = *include_diag;
    if (auto consistency_json = json.find("consistency"); consistency_json != json.end()) {
        auto consistency = consistency_from_json(*consistency_json);
        if (!consistency) {
            return std::unexpected(consistency.error());
        }
        request.consistency = std::move(*consistency);
    }
    if (auto it = json.find("type_name"); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must be a string"));
        }
        request.type_name = it->get<std::string>();
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    return request;
}

[[nodiscard]] auto read_response_to_json(ReadResponse const& response) -> Expected<Json> {
    auto request_id_check = validate_identifier(response.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto path_check = validateAbsolutePath(response.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    Json json{{"request_id", response.request_id},
              {"path", response.path},
              {"version", response.version}};
    bool success = !response.error.has_value();
    json["success"] = success;
    if (response.value) {
        auto value_json = value_to_json(*response.value);
        if (!value_json) {
            return std::unexpected(value_json.error());
        }
        json["value"] = std::move(*value_json);
    }
    if (response.children_included) {
        Json children = Json::array();
        for (auto const& child : response.children) {
            children.push_back(child);
        }
        json["children"] = std::move(children);
    }
    if (response.error) {
        auto error_json = error_to_json(*response.error);
        if (!error_json) {
            return std::unexpected(error_json.error());
        }
        json["error"] = std::move(*error_json);
    }
    return json;
}

[[nodiscard]] auto read_response_from_json(Json const& json) -> Expected<ReadResponse> {
    auto ensure = ensure_object(json, "ReadResponse");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    ReadResponse response;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    response.request_id = std::move(*request_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    response.path = std::move(*path);
    auto version = read_uint64(json, "version");
    if (!version) {
        return std::unexpected(version.error());
    }
    response.version = *version;
    if (auto value_json = json.find("value"); value_json != json.end()) {
        auto value = value_from_json(*value_json);
        if (!value) {
            return std::unexpected(value.error());
        }
        response.value = std::move(*value);
    }
    if (auto children_json = json.find("children"); children_json != json.end()) {
        if (!children_json->is_array()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "children",
                                              "must be an array"));
        }
        response.children_included = true;
        for (auto const& child : *children_json) {
            if (!child.is_string()) {
                return std::unexpected(make_error(Error::Code::MalformedInput,
                                                  "children",
                                                  "all entries must be strings"));
            }
            response.children.push_back(child.get<std::string>());
        }
    }
    if (auto error_json = json.find("error"); error_json != json.end()) {
        auto error = error_from_json(*error_json);
        if (!error) {
            return std::unexpected(error.error());
        }
        response.error = std::move(*error);
    }
    return response;
}

[[nodiscard]] auto insert_request_to_json(InsertRequest const& request) -> Expected<Json> {
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    if (request.type_name.empty() && !request.value.type_name.empty()) {
        // Mirror the value payload's type when the request metadata was left blank.
        const_cast<InsertRequest&>(request).type_name = request.value.type_name;
    }
    auto type_check = ensure_non_empty(request.type_name, "type_name");
    if (!type_check) {
        return std::unexpected(type_check.error());
    }
    if (request.value.type_name.empty()) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "value.type_name", "is required"));
    }
    Json json{{"request_id", request.request_id},
              {"session_id", request.session_id},
              {"path", request.path},
              {"type_name", request.type_name}};
    auto value_json = value_to_json(request.value);
    if (!value_json) {
        return std::unexpected(value_json.error());
    }
    json["value"] = std::move(*value_json);
    return json;
}

[[nodiscard]] auto insert_request_from_json(Json const& json) -> Expected<InsertRequest> {
    auto ensure = ensure_object(json, "InsertRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    InsertRequest request;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    request.session_id = std::move(*session_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    request.path = std::move(*path);
    auto type_name = read_optional_string(json, "type_name");
    if (type_name && type_name->empty()) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "type_name", "must not be empty"));
    }
    auto value_json = json.find("value");
    if (value_json == json.end()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "value", "is required"));
    }
    auto value = value_from_json(*value_json);
    if (!value) {
        return std::unexpected(value.error());
    }
    request.value = std::move(*value);
    if (request.value.type_name.empty()) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "value.type_name", "is required"));
    }
    if (type_name) {
        request.type_name = std::move(*type_name);
        if (request.value.type_name != request.type_name) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must match value.type_name"));
        }
    } else {
        request.type_name = request.value.type_name;
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    return request;
}

[[nodiscard]] auto insert_response_to_json(InsertResponse const& response) -> Expected<Json> {
    auto request_id_check = validate_identifier(response.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    Json json{{"request_id", response.request_id},
              {"success", response.success},
              {"values_inserted", response.values_inserted},
              {"spaces_inserted", response.spaces_inserted},
              {"tasks_inserted", response.tasks_inserted}};
    if (response.error) {
        auto error_json = error_to_json(*response.error);
        if (!error_json) {
            return std::unexpected(error_json.error());
        }
        json["error"] = std::move(*error_json);
    }
    return json;
}

[[nodiscard]] auto insert_response_from_json(Json const& json) -> Expected<InsertResponse> {
    auto ensure = ensure_object(json, "InsertResponse");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    InsertResponse response;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    response.request_id = std::move(*request_id);
    auto success = read_boolean(json, "success", false);
    if (!success) {
        return std::unexpected(success.error());
    }
    response.success = *success;
    response.values_inserted = static_cast<std::uint32_t>(json.value("values_inserted", 0));
    response.spaces_inserted = static_cast<std::uint32_t>(json.value("spaces_inserted", 0));
    response.tasks_inserted  = static_cast<std::uint32_t>(json.value("tasks_inserted", 0));
    if (auto error_json = json.find("error"); error_json != json.end()) {
        auto error = error_from_json(*error_json);
        if (!error) {
            return std::unexpected(error.error());
        }
        response.error = std::move(*error);
    }
    return response;
}

[[nodiscard]] auto take_request_to_json(TakeRequest const& request) -> Expected<Json> {
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    if (request.type_name && request.type_name->empty()) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "type_name", "must not be empty"));
    }
    auto timeout_value = to_uint64(request.timeout);
    if (!timeout_value) {
        return std::unexpected(timeout_value.error());
    }
    auto batch = std::clamp<std::uint32_t>(request.max_items, 1U, kMaxTakeBatch);
    Json json{{"request_id", request.request_id},
              {"session_id", request.session_id},
              {"path", request.path},
              {"max_items", batch},
              {"do_block", request.do_block},
              {"timeout_ms", *timeout_value}};
    if (request.type_name && !request.type_name->empty()) {
        json["type_name"] = *request.type_name;
    }
    return json;
}

[[nodiscard]] auto take_request_from_json(Json const& json) -> Expected<TakeRequest> {
    auto ensure = ensure_object(json, "TakeRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    TakeRequest request;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    request.session_id = std::move(*session_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    request.path = std::move(*path);
    if (auto it = json.find("type_name"); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must be a string"));
        }
        auto parsed = it->get<std::string>();
        if (parsed.empty()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must not be empty"));
        }
        request.type_name = std::move(parsed);
    }
    auto do_block = read_boolean(json, "do_block", false);
    if (!do_block) {
        return std::unexpected(do_block.error());
    }
    request.do_block = *do_block;
    auto timeout = parse_duration(json, "timeout_ms");
    if (!timeout) {
        return std::unexpected(timeout.error());
    }
    request.timeout = *timeout;
    auto max_items = read_optional_uint64(json, "max_items");
    if (!max_items) {
        return std::unexpected(max_items.error());
    }
    auto requested = max_items->value_or(1);
    if (requested == 0) {
        requested = 1;
    }
    request.max_items = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(requested, kMaxTakeBatch));
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    return request;
}

[[nodiscard]] auto take_response_to_json(TakeResponse const& response) -> Expected<Json> {
    auto request_id_check = validate_identifier(response.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    Json json{{"request_id", response.request_id}, {"success", response.success}};
    if (!response.values.empty()) {
        Json values = Json::array();
        for (auto const& value : response.values) {
            auto value_json = value_to_json(value);
            if (!value_json) {
                return std::unexpected(value_json.error());
            }
            values.push_back(*value_json);
        }
        json["values"] = std::move(values);
        if (response.values.size() == 1) {
            json["value"] = json["values"].front();
        }
    }
    if (response.error) {
        auto error_json = error_to_json(*response.error);
        if (!error_json) {
            return std::unexpected(error_json.error());
        }
        json["error"] = std::move(*error_json);
    }
    return json;
}

[[nodiscard]] auto take_response_from_json(Json const& json) -> Expected<TakeResponse> {
    auto ensure = ensure_object(json, "TakeResponse");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    TakeResponse response;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    response.request_id = std::move(*request_id);
    auto success = read_boolean(json, "success", false);
    if (!success) {
        return std::unexpected(success.error());
    }
    response.success = *success;
    if (auto values_json = json.find("values"); values_json != json.end()) {
        if (!values_json->is_array()) {
            return std::unexpected(
                make_error(Error::Code::MalformedInput, "values", "must be an array"));
        }
        for (auto const& entry : *values_json) {
            auto value = value_from_json(entry);
            if (!value) {
                return std::unexpected(value.error());
            }
            response.values.push_back(std::move(*value));
        }
    }
    if (response.values.empty()) {
        if (auto value_json = json.find("value"); value_json != json.end()) {
            auto value = value_from_json(*value_json);
            if (!value) {
                return std::unexpected(value.error());
            }
            response.values.push_back(std::move(*value));
        }
    }
    if (auto error_json = json.find("error"); error_json != json.end()) {
        auto error = error_from_json(*error_json);
        if (!error) {
            return std::unexpected(error.error());
        }
        response.error = std::move(*error);
    }
    return response;
}

[[nodiscard]] auto wait_request_to_json(WaitSubscriptionRequest const& request) -> Expected<Json> {
    auto subscription_check = validate_identifier(request.subscription_id, "subscription_id");
    if (!subscription_check) {
        return std::unexpected(subscription_check.error());
    }
    auto request_id_check = validate_identifier(request.request_id, "request_id");
    if (!request_id_check) {
        return std::unexpected(request_id_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    Json json{{"request_id", request.request_id},
              {"session_id", request.session_id},
              {"subscription_id", request.subscription_id},
              {"path", request.path},
              {"include_value", request.include_value},
              {"include_children", request.include_children}};
    if (request.after_version) {
        json["after_version"] = *request.after_version;
    }
    return json;
}

[[nodiscard]] auto wait_request_from_json(Json const& json) -> Expected<WaitSubscriptionRequest> {
    auto ensure = ensure_object(json, "WaitSubscriptionRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    WaitSubscriptionRequest request;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    request.session_id = std::move(*session_id);
    auto subscription_id = read_string(json, "subscription_id");
    if (!subscription_id) {
        return std::unexpected(subscription_id.error());
    }
    request.subscription_id = std::move(*subscription_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    request.path = std::move(*path);
    auto include_value = read_boolean(json, "include_value", false);
    if (!include_value) {
        return std::unexpected(include_value.error());
    }
    request.include_value = *include_value;
    auto include_children = read_boolean(json, "include_children", false);
    if (!include_children) {
        return std::unexpected(include_children.error());
    }
    request.include_children = *include_children;
    auto after_version = read_optional_uint64(json, "after_version");
    if (!after_version) {
        return std::unexpected(after_version.error());
    }
    request.after_version = after_version->has_value() ? after_version->value() : std::optional<std::uint64_t>{};
    auto path_check = validateAbsolutePath(request.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    return request;
}

[[nodiscard]] auto wait_ack_to_json(WaitSubscriptionAck const& ack) -> Expected<Json> {
    auto subscription_check = validate_identifier(ack.subscription_id, "subscription_id");
    if (!subscription_check) {
        return std::unexpected(subscription_check.error());
    }
    Json json{{"subscription_id", ack.subscription_id}, {"accepted", ack.accepted}};
    if (ack.error) {
        auto error_json = error_to_json(*ack.error);
        if (!error_json) {
            return std::unexpected(error_json.error());
        }
        json["error"] = std::move(*error_json);
    }
    return json;
}

[[nodiscard]] auto wait_ack_from_json(Json const& json) -> Expected<WaitSubscriptionAck> {
    auto ensure = ensure_object(json, "WaitSubscriptionAck");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    WaitSubscriptionAck ack;
    auto subscription_id = read_string(json, "subscription_id");
    if (!subscription_id) {
        return std::unexpected(subscription_id.error());
    }
    ack.subscription_id = std::move(*subscription_id);
    auto accepted = read_boolean(json, "accepted", false);
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    ack.accepted = *accepted;
    if (auto error_json = json.find("error"); error_json != json.end()) {
        auto error = error_from_json(*error_json);
        if (!error) {
            return std::unexpected(error.error());
        }
        ack.error = std::move(*error);
    }
    return ack;
}

[[nodiscard]] auto notification_to_json(Notification const& notification) -> Expected<Json> {
    auto subscription_check = validate_identifier(notification.subscription_id, "subscription_id");
    if (!subscription_check) {
        return std::unexpected(subscription_check.error());
    }
    auto path_check = validateAbsolutePath(notification.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    Json json{{"subscription_id", notification.subscription_id},
              {"path", notification.path},
              {"version", notification.version},
              {"deleted", notification.deleted}};
    if (notification.type_name && !notification.type_name->empty()) {
        json["type_name"] = *notification.type_name;
    }
    if (notification.value) {
        auto value_json = value_to_json(*notification.value);
        if (!value_json) {
            return std::unexpected(value_json.error());
        }
        json["value"] = std::move(*value_json);
    }
    return json;
}

[[nodiscard]] auto notification_from_json(Json const& json) -> Expected<Notification> {
    auto ensure = ensure_object(json, "Notification");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    Notification notification;
    auto subscription_id = read_string(json, "subscription_id");
    if (!subscription_id) {
        return std::unexpected(subscription_id.error());
    }
    notification.subscription_id = std::move(*subscription_id);
    auto path = read_string(json, "path");
    if (!path) {
        return std::unexpected(path.error());
    }
    notification.path = std::move(*path);
    auto version = read_uint64(json, "version");
    if (!version) {
        return std::unexpected(version.error());
    }
    notification.version = *version;
    auto deleted = read_boolean(json, "deleted", false);
    if (!deleted) {
        return std::unexpected(deleted.error());
    }
    notification.deleted = *deleted;
    if (auto value_json = json.find("value"); value_json != json.end()) {
        auto value = value_from_json(*value_json);
        if (!value) {
            return std::unexpected(value.error());
        }
        notification.value = std::move(*value);
    }
    if (auto it = json.find("type_name"); it != json.end()) {
        if (!it->is_string()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must be a string"));
        }
        auto parsed = it->get<std::string>();
        if (parsed.empty()) {
            return std::unexpected(make_error(Error::Code::MalformedInput,
                                              "type_name",
                                              "must not be empty"));
        }
        notification.type_name = std::move(parsed);
    }
    auto path_check = validateAbsolutePath(notification.path);
    if (!path_check) {
        return std::unexpected(path_check.error());
    }
    return notification;
}

[[nodiscard]] auto notification_stream_request_to_json(NotificationStreamRequest const& request)
    -> Expected<Json> {
    auto request_check = validate_identifier(request.request_id, "request_id");
    if (!request_check) {
        return std::unexpected(request_check.error());
    }
    auto session_check = validate_identifier(request.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    if (request.max_batch == 0 || request.max_batch > kMaxTakeBatch) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "max_batch", "out of supported range"));
    }
    auto timeout_ms = to_uint64(request.timeout);
    if (!timeout_ms) {
        return std::unexpected(timeout_ms.error());
    }
    Json json{{"request_id", request.request_id},
              {"session_id", request.session_id},
              {"timeout_ms", *timeout_ms},
              {"max_batch", request.max_batch}};
    return json;
}

[[nodiscard]] auto notification_stream_request_from_json(Json const& json)
    -> Expected<NotificationStreamRequest> {
    auto ensure = ensure_object(json, "NotificationStreamRequest");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    NotificationStreamRequest request;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    request.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    request.session_id = std::move(*session_id);
    auto timeout = parse_duration(json, "timeout_ms");
    if (!timeout) {
        return std::unexpected(timeout.error());
    }
    request.timeout = *timeout;
    auto max_batch_value = read_uint64(json, "max_batch");
    if (!max_batch_value) {
        return std::unexpected(max_batch_value.error());
    }
    if (*max_batch_value == 0 || *max_batch_value > kMaxTakeBatch) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "max_batch", "out of supported range"));
    }
    request.max_batch = static_cast<std::size_t>(*max_batch_value);
    return request;
}

[[nodiscard]] auto notification_stream_response_to_json(NotificationStreamResponse const& response)
    -> Expected<Json> {
    auto request_check = validate_identifier(response.request_id, "request_id");
    if (!request_check) {
        return std::unexpected(request_check.error());
    }
    auto session_check = validate_identifier(response.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    Json json{{"request_id", response.request_id},
              {"session_id", response.session_id}};
    Json notifications = Json::array();
    for (auto const& note : response.notifications) {
        auto note_json = notification_to_json(note);
        if (!note_json) {
            return std::unexpected(note_json.error());
        }
        notifications.push_back(std::move(*note_json));
    }
    json["notifications"] = std::move(notifications);
    if (response.error) {
        auto err_json = error_to_json(*response.error);
        if (!err_json) {
            return std::unexpected(err_json.error());
        }
        json["error"] = std::move(*err_json);
    }
    return json;
}

[[nodiscard]] auto notification_stream_response_from_json(Json const& json)
    -> Expected<NotificationStreamResponse> {
    auto ensure = ensure_object(json, "NotificationStreamResponse");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    NotificationStreamResponse response;
    auto request_id = read_string(json, "request_id");
    if (!request_id) {
        return std::unexpected(request_id.error());
    }
    response.request_id = std::move(*request_id);
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    response.session_id = std::move(*session_id);
    auto notes_it = json.find("notifications");
    if (notes_it == json.end() || !notes_it->is_array()) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "notifications", "must be an array"));
    }
    for (auto const& entry : *notes_it) {
        auto note = notification_from_json(entry);
        if (!note) {
            return std::unexpected(note.error());
        }
        response.notifications.push_back(std::move(*note));
    }
    if (auto err = json.find("error"); err != json.end()) {
        auto payload = error_from_json(*err);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        response.error = std::move(*payload);
    }
    return response;
}

[[nodiscard]] auto heartbeat_to_json(Heartbeat const& heartbeat) -> Expected<Json> {
    auto session_check = validate_identifier(heartbeat.session_id, "session_id");
    if (!session_check) {
        return std::unexpected(session_check.error());
    }
    Json json{{"session_id", heartbeat.session_id}, {"sequence", heartbeat.sequence}};
    return json;
}

[[nodiscard]] auto heartbeat_from_json(Json const& json) -> Expected<Heartbeat> {
    auto ensure = ensure_object(json, "Heartbeat");
    if (!ensure) {
        return std::unexpected(ensure.error());
    }
    Heartbeat heartbeat;
    auto session_id = read_string(json, "session_id");
    if (!session_id) {
        return std::unexpected(session_id.error());
    }
    heartbeat.session_id = std::move(*session_id);
    auto sequence = read_uint64(json, "sequence");
    if (!sequence) {
        return std::unexpected(sequence.error());
    }
    heartbeat.sequence = *sequence;
    return heartbeat;
}

template <typename T>
[[nodiscard]] auto require_payload(RemoteFrame const& frame, FrameKind expected_kind)
    -> Expected<T const*> {
    if (frame.kind != expected_kind) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "frame",
                                          "kind does not match payload"));
    }
    if (!std::holds_alternative<T>(frame.payload)) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "frame",
                                          "payload variant mismatch"));
    }
    return &std::get<T>(frame.payload);
}

[[nodiscard]] auto build_payload(RemoteFrame const& frame) -> Expected<Json> {
    switch (frame.kind) {
    case FrameKind::MountOpenRequest: {
        auto payload = require_payload<MountOpenRequest>(frame, FrameKind::MountOpenRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return mount_open_request_to_json(**payload);
    }
    case FrameKind::MountOpenResponse: {
        auto payload = require_payload<MountOpenResponse>(frame, FrameKind::MountOpenResponse);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return mount_open_response_to_json(**payload);
    }
    case FrameKind::ReadRequest: {
        auto payload = require_payload<ReadRequest>(frame, FrameKind::ReadRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return read_request_to_json(**payload);
    }
    case FrameKind::ReadResponse: {
        auto payload = require_payload<ReadResponse>(frame, FrameKind::ReadResponse);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return read_response_to_json(**payload);
    }
    case FrameKind::InsertRequest: {
        auto payload = require_payload<InsertRequest>(frame, FrameKind::InsertRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return insert_request_to_json(**payload);
    }
    case FrameKind::InsertResponse: {
        auto payload = require_payload<InsertResponse>(frame, FrameKind::InsertResponse);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return insert_response_to_json(**payload);
    }
    case FrameKind::TakeRequest: {
        auto payload = require_payload<TakeRequest>(frame, FrameKind::TakeRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return take_request_to_json(**payload);
    }
    case FrameKind::TakeResponse: {
        auto payload = require_payload<TakeResponse>(frame, FrameKind::TakeResponse);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return take_response_to_json(**payload);
    }
    case FrameKind::WaitSubscribeRequest: {
        auto payload =
            require_payload<WaitSubscriptionRequest>(frame, FrameKind::WaitSubscribeRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return wait_request_to_json(**payload);
    }
    case FrameKind::WaitSubscribeAck: {
        auto payload = require_payload<WaitSubscriptionAck>(frame, FrameKind::WaitSubscribeAck);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return wait_ack_to_json(**payload);
    }
    case FrameKind::Notification: {
        auto payload = require_payload<Notification>(frame, FrameKind::Notification);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return notification_to_json(**payload);
    }
    case FrameKind::NotificationStreamRequest: {
        auto payload = require_payload<NotificationStreamRequest>(frame,
                                                                  FrameKind::NotificationStreamRequest);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return notification_stream_request_to_json(**payload);
    }
    case FrameKind::NotificationStreamResponse: {
        auto payload = require_payload<NotificationStreamResponse>(
            frame, FrameKind::NotificationStreamResponse);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return notification_stream_response_to_json(**payload);
    }
    case FrameKind::Heartbeat: {
        auto payload = require_payload<Heartbeat>(frame, FrameKind::Heartbeat);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return heartbeat_to_json(**payload);
    }
    case FrameKind::Error: {
        auto payload = require_payload<ErrorPayload>(frame, FrameKind::Error);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return error_to_json(**payload);
    }
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, "frame", "unsupported kind"));
}

[[nodiscard]] auto parse_payload(FrameKind kind, Json const& json) -> Expected<std::variant<MountOpenRequest,
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
                                                                                             ErrorPayload>> {
    switch (kind) {
    case FrameKind::MountOpenRequest: {
        auto payload = mount_open_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::MountOpenResponse: {
        auto payload = mount_open_response_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::ReadRequest: {
        auto payload = read_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::ReadResponse: {
        auto payload = read_response_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::InsertRequest: {
        auto payload = insert_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::InsertResponse: {
        auto payload = insert_response_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::TakeRequest: {
        auto payload = take_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::TakeResponse: {
        auto payload = take_response_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::WaitSubscribeRequest: {
        auto payload = wait_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::WaitSubscribeAck: {
        auto payload = wait_ack_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::Notification: {
        auto payload = notification_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::NotificationStreamRequest: {
        auto payload = notification_stream_request_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::NotificationStreamResponse: {
        auto payload = notification_stream_response_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::Heartbeat: {
        auto payload = heartbeat_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    case FrameKind::Error: {
        auto payload = error_from_json(json);
        if (!payload) {
            return std::unexpected(payload.error());
        }
        return *payload;
    }
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, "frame", "unsupported kind"));
}

} // namespace

RemotePayloadCompatibility defaultRemotePayloadCompatibility() {
    static RemotePayloadCompatibility mode = read_typed_payload_env();
    return mode;
}

bool allowLegacyPayloads(RemotePayloadCompatibility mode) {
    return mode == RemotePayloadCompatibility::LegacyCompatible;
}

auto frameKindToString(FrameKind kind) -> std::string_view {
    switch (kind) {
    case FrameKind::MountOpenRequest:
        return "MountOpenRequest";
    case FrameKind::MountOpenResponse:
        return "MountOpenResponse";
    case FrameKind::ReadRequest:
        return "ReadRequest";
    case FrameKind::ReadResponse:
        return "ReadResponse";
    case FrameKind::InsertRequest:
        return "InsertRequest";
    case FrameKind::InsertResponse:
        return "InsertResponse";
    case FrameKind::TakeRequest:
        return "TakeRequest";
    case FrameKind::TakeResponse:
        return "TakeResponse";
    case FrameKind::WaitSubscribeRequest:
        return "WaitSubscribeRequest";
    case FrameKind::WaitSubscribeAck:
        return "WaitSubscribeAck";
    case FrameKind::Notification:
        return "Notification";
    case FrameKind::NotificationStreamRequest:
        return "NotificationStreamRequest";
    case FrameKind::NotificationStreamResponse:
        return "NotificationStreamResponse";
    case FrameKind::Heartbeat:
        return "Heartbeat";
    case FrameKind::Error:
        return "Error";
    }
    return "Unknown";
}

auto parseFrameKind(std::string_view name) -> Expected<FrameKind> {
    if (name == "MountOpenRequest") {
        return FrameKind::MountOpenRequest;
    }
    if (name == "MountOpenResponse") {
        return FrameKind::MountOpenResponse;
    }
    if (name == "ReadRequest") {
        return FrameKind::ReadRequest;
    }
    if (name == "ReadResponse") {
        return FrameKind::ReadResponse;
    }
    if (name == "InsertRequest") {
        return FrameKind::InsertRequest;
    }
    if (name == "InsertResponse") {
        return FrameKind::InsertResponse;
    }
    if (name == "TakeRequest") {
        return FrameKind::TakeRequest;
    }
    if (name == "TakeResponse") {
        return FrameKind::TakeResponse;
    }
    if (name == "WaitSubscribeRequest") {
        return FrameKind::WaitSubscribeRequest;
    }
    if (name == "WaitSubscribeAck") {
        return FrameKind::WaitSubscribeAck;
    }
    if (name == "Notification") {
        return FrameKind::Notification;
    }
    if (name == "NotificationStreamRequest") {
        return FrameKind::NotificationStreamRequest;
    }
    if (name == "NotificationStreamResponse") {
        return FrameKind::NotificationStreamResponse;
    }
    if (name == "Heartbeat") {
        return FrameKind::Heartbeat;
    }
    if (name == "Error") {
        return FrameKind::Error;
    }
    return std::unexpected(make_error(Error::Code::MalformedInput, "type",
                                      "unknown frame type"));
}

auto serializeFrame(RemoteFrame const& frame) -> Expected<std::string> {
    auto payload = build_payload(frame);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto sent_at = frame.sent_at.count();
    if (sent_at < 0) {
        return std::unexpected(
            make_error(Error::Code::MalformedInput, "sent_at_ms", "must be non-negative"));
    }
    Json json{{"type", frameKindToString(frame.kind)},
              {"sent_at_ms", static_cast<std::uint64_t>(sent_at)},
              {"payload", std::move(*payload)}};
    return json.dump();
}

auto deserializeFrame(std::string_view payload) -> Expected<RemoteFrame> {
    auto json = Json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "frame",
                                          "invalid JSON payload"));
    }
    if (!json.is_object()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "frame",
                                          "must be a JSON object"));
    }
    auto type = read_string(json, "type");
    if (!type) {
        return std::unexpected(type.error());
    }
    auto kind = parseFrameKind(*type);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    auto sent_at_ms = read_optional_uint64(json, "sent_at_ms");
    if (!sent_at_ms) {
        return std::unexpected(sent_at_ms.error());
    }
    auto payload_json_it = json.find("payload");
    if (payload_json_it == json.end()) {
        return std::unexpected(make_error(Error::Code::MalformedInput, "payload", "is required"));
    }
    auto payload_variant = parse_payload(*kind, *payload_json_it);
    if (!payload_variant) {
        return std::unexpected(payload_variant.error());
    }
    RemoteFrame frame;
    frame.kind = *kind;
    if (sent_at_ms->has_value()) {
        frame.sent_at = std::chrono::milliseconds{static_cast<std::int64_t>(sent_at_ms->value())};
    } else {
        frame.sent_at = std::chrono::milliseconds{0};
    }
    frame.payload = std::move(*payload_variant);
    return frame;
}

auto validateAbsolutePath(std::string_view path) -> Expected<void> {
    if (path.empty()) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path", "must not be empty"));
    }
    if (path.front() != '/') {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path",
                                          "must start with '/'"));
    }
    for (std::size_t idx = 0; idx < path.size(); ++idx) {
        char ch = path[idx];
        if (is_control(ch)) {
            return std::unexpected(make_error(Error::Code::InvalidPath, "path",
                                              "contains control characters"));
        }
        if (ch == '\\') {
            return std::unexpected(make_error(Error::Code::InvalidPath, "path",
                                              "contains unsupported separator"));
        }
    }
    if (path.find("..") != std::string_view::npos) {
        return std::unexpected(make_error(Error::Code::InvalidPath, "path",
                                          "must not contain '..'"));
    }
    return {};
}

} // namespace SP::Distributed
