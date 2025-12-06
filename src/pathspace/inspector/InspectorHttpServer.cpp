#include "inspector/InspectorHttpServer.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "inspector/InspectorSnapshot.hpp"
#include "inspector/PaintScreenshotCard.hpp"
#include "tools/PathSpaceJsonExporter.hpp"
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <system_error>
#include <thread>
#include <utility>

#include "nlohmann/json.hpp"

namespace SP::Inspector {
namespace {

[[nodiscard]] auto parse_unsigned(std::string const& value, std::size_t fallback) -> std::size_t {
    if (value.empty()) {
        return fallback;
    }
    std::size_t parsed = fallback;
    auto const* begin  = value.data();
    auto const* end    = value.data() + value.size();
    auto        result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return parsed;
}

[[nodiscard]] auto make_error(std::string const& message, int status) -> std::pair<int, std::string> {
    nlohmann::json json{
        {"error", message},
    };
    return {status, json.dump(2)};
}

[[nodiscard]] auto parse_bool(std::string value, bool fallback) -> bool {
    if (value.empty()) {
        return fallback;
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

[[nodiscard]] auto clamp_interval(std::size_t value,
                                  std::chrono::milliseconds fallback,
                                  std::chrono::milliseconds minimum)
    -> std::chrono::milliseconds {
    std::chrono::milliseconds candidate = value == 0 ? fallback : std::chrono::milliseconds(value);
    if (candidate < minimum) {
        candidate = minimum;
    }
    return candidate;
}

[[nodiscard]] auto to_millis_since_epoch(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    if (tp.time_since_epoch().count() <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
}

[[nodiscard]] auto make_stream_error_payload(std::string const& message) -> std::string {
    nlohmann::json json{
        {"error", "inspector_stream_failure"},
        {"message", message},
    };
    return json.dump(-1);
}

[[nodiscard]] auto make_acl_error_payload(InspectorAclDecision const& decision,
                                          std::string_view endpoint) -> std::string {
    std::string message = decision.reason.empty() ? "access denied" : decision.reason;
    if (!decision.allowed_roots.empty()) {
        message.append(" (allowed roots: ");
        for (std::size_t i = 0; i < decision.allowed_roots.size(); ++i) {
            if (i > 0) {
                message.append(", ");
            }
            message.append(decision.allowed_roots[i]);
        }
        message.push_back(')');
    }
    nlohmann::json json{{"error", "inspector_acl_denied"},
                        {"message", message},
                        {"role", decision.role},
                        {"requested_path", decision.requested_path},
                        {"endpoint", endpoint},
                        {"allowed_roots", decision.allowed_roots}};
    return json.dump(2);
}

[[nodiscard]] auto insert_json_value_for_test(PathSpace& space,
                                              std::string const& path,
                                              nlohmann::json const& value) -> Expected<void> {
    SP::InsertReturn inserted{};
    if (value.is_boolean()) {
        inserted = space.insert(path, value.get<bool>());
    } else if (value.is_number_unsigned()) {
        inserted = space.insert(path, value.get<std::uint64_t>());
    } else if (value.is_number_integer()) {
        inserted = space.insert(path, value.get<std::int64_t>());
    } else if (value.is_number_float()) {
        inserted = space.insert(path, value.get<double>());
    } else if (value.is_string()) {
        inserted = space.insert(path, value.get<std::string>());
    } else if (value.is_object() || value.is_array()) {
        inserted = space.insert(path, value.dump());
    } else {
        return std::unexpected(Error{Error::Code::MalformedInput, "unsupported value type"});
    }

    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

[[nodiscard]] auto apply_test_mutations(PathSpace& space, nlohmann::json const& payload)
    -> Expected<std::size_t> {
    bool clear_requested = false;
    if (auto clear_it = payload.find("clear"); clear_it != payload.end()) {
        if (!clear_it->is_boolean()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                        "clear must be a boolean"});
        }
        clear_requested = clear_it->get<bool>();
    }
    if (clear_requested) {
        space.clear();
    }

    auto set_it = payload.find("set");
    if (set_it == payload.end()) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                    "set array is required"});
    }
    if (!set_it->is_array()) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                    "set must be an array"});
    }

    std::size_t applied = 0;
    for (auto const& op : *set_it) {
        if (!op.is_object()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                        "set entries must be objects"});
        }
        auto path_it = op.find("path");
        auto value_it = op.find("value");
        if (path_it == op.end() || !path_it->is_string()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                        "each entry must include a path string"});
        }
        if (value_it == op.end()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                        "each entry must include a value"});
        }
        auto path = path_it->get<std::string>();
        if (path.empty()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                        "path must not be empty"});
        }
        auto inserted = insert_json_value_for_test(space, path, *value_it);
        if (!inserted) {
            return std::unexpected(inserted.error());
        }
        ++applied;
    }

    return applied;
}

[[nodiscard]] auto json_to_uint64(nlohmann::json const& value) -> std::uint64_t {
    using limit = std::numeric_limits<std::uint64_t>;
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        auto const number = value.get<std::int64_t>();
        return number <= 0 ? 0 : static_cast<std::uint64_t>(number);
    }
    if (value.is_number_float()) {
        auto const number = value.get<double>();
        if (number <= 0.0) {
            return 0;
        }
        if (number >= static_cast<double>(limit::max())) {
            return limit::max();
        }
        return static_cast<std::uint64_t>(number);
    }
    if (value.is_string()) {
        auto const text = value.get<std::string>();
        std::uint64_t parsed = 0;
        auto         result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{}) {
            return parsed;
        }
    }
    return 0;
}

[[nodiscard]] auto read_uint64(nlohmann::json const& object, char const* key) -> std::uint64_t {
    auto it = object.find(key);
    if (it == object.end()) {
        return 0;
    }
    return json_to_uint64(*it);
}

struct WatchlistRecord {
    std::string              id;
    std::string              name;
    std::vector<std::string> paths;
    std::uint64_t            created_ms = 0;
    std::uint64_t            updated_ms = 0;
};

struct WatchlistContext {
    std::string display_user;
    std::string user_id;
    std::string root;
    std::string trash_root;
};

constexpr std::size_t kMaxWatchlistIdLength = 64;

[[nodiscard]] auto build_watchlist_path(std::string const& root, std::string const& id) -> std::string;
auto persist_watchlist(PathSpace& space,
                       std::string const& path,
                       WatchlistRecord const& record) -> Expected<void>;
constexpr std::uint32_t kWatchlistSpaceVersion = 1;

[[nodiscard]] auto watchlist_space_node(std::string path) -> std::string {
    path.append("/space");
    return path;
}

[[nodiscard]] auto build_watchlist_space_path(std::string const& root, std::string const& id) -> std::string {
    return watchlist_space_node(build_watchlist_path(root, id));
}

[[nodiscard]] auto parse_watchlist_record(std::string const& payload, std::string const& id)
    -> std::optional<WatchlistRecord> {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(payload);
    } catch (...) {
        return std::nullopt;
    }
    if (!json.is_object()) {
        return std::nullopt;
    }

    WatchlistRecord record;
    record.id = json.value("id", id);
    if (record.id.empty()) {
        record.id = id;
    }
    record.name       = json.value("name", record.id);
    record.created_ms = read_uint64(json, "created_ms");
    record.updated_ms = read_uint64(json, "updated_ms");

    if (auto paths_it = json.find("paths"); paths_it != json.end() && paths_it->is_array()) {
        for (auto const& entry : *paths_it) {
            if (entry.is_string()) {
                record.paths.push_back(entry.get<std::string>());
            }
        }
    }

    return record;
}

[[nodiscard]] auto read_watchlist_legacy(PathSpace& space,
                                         std::string const& path,
                                         std::string const& id) -> std::optional<WatchlistRecord> {
    auto payload = space.read<std::string, std::string>(path);
    if (!payload) {
        return std::nullopt;
    }
    return parse_watchlist_record(*payload, id);
}

[[nodiscard]] auto read_watchlist_from_nested(PathSpace& space,
                                              std::string const& root,
                                              std::string const& id) -> std::optional<WatchlistRecord> {
    auto space_root = build_watchlist_space_path(root, id);

    auto name    = space.read<std::string, std::string>(space_root + "/meta/name");
    auto created = space.read<std::uint64_t, std::string>(space_root + "/meta/created_ms");
    auto updated = space.read<std::uint64_t, std::string>(space_root + "/meta/updated_ms");
    auto paths   = space.read<std::vector<std::string>, std::string>(space_root + "/paths");

    if (!name || !created || !updated || !paths) {
        return std::nullopt;
    }

    WatchlistRecord record;
    if (auto stored_id = space.read<std::string, std::string>(space_root + "/meta/id"); stored_id && !stored_id->empty()) {
        record.id = *stored_id;
    } else {
        record.id = id;
    }
    record.name       = *name;
    record.paths      = *paths;
    record.created_ms = *created;
    record.updated_ms = *updated;
    return record;
}

auto clear_legacy_watchlist_payload(PathSpace& space, std::string const& path) -> void {
    while (true) {
        auto removed = space.take<std::string>(path);
        if (!removed) {
            if (removed.error().code == Error::Code::NoSuchPath) {
                break;
            }
            break;
        }
    }
}

auto migrate_watchlists(PathSpace& space, std::string const& root) -> void {
    auto children = space.listChildren(SP::ConcretePathStringView{root});
    for (auto const& child : children) {
        if (child.empty() || child.front() == '.') {
            continue;
        }
        auto space_path = build_watchlist_space_path(root, child);
        auto version    = space.read<std::uint32_t, std::string>(space_path + "/meta/version");
        if (version && version.value() == kWatchlistSpaceVersion) {
            continue;
        }
        if (version && version.error().code != Error::Code::NoSuchPath) {
            continue;
        }
        auto legacy_path = build_watchlist_path(root, child);
        auto legacy      = read_watchlist_legacy(space, legacy_path, child);
        if (!legacy) {
            continue;
        }
        if (auto persisted = persist_watchlist(space, legacy_path, *legacy); !persisted) {
            continue;
        }
        clear_legacy_watchlist_payload(space, legacy_path);
    }
}

auto move_legacy_watchlist(PathSpace& space,
                           std::string const& source_base,
                           std::string const& destination_base,
                           std::string const& id) -> Expected<bool> {
    auto taken = space.take<std::string>(source_base);
    if (!taken) {
        if (taken.error().code == Error::Code::NoSuchPath) {
            return false;
        }
        return std::unexpected(taken.error());
    }

    std::string payload = std::move(*taken);
    if (auto parsed = parse_watchlist_record(payload, id)) {
        parsed->id = id;
        if (auto persisted = persist_watchlist(space, destination_base, *parsed); !persisted) {
            return std::unexpected(persisted.error());
        }
    } else {
        auto inserted = space.insert(destination_base, payload);
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }

    clear_legacy_watchlist_payload(space, source_base);
    return true;
}

[[nodiscard]] auto now_ms() -> std::uint64_t {
    return to_millis_since_epoch(std::chrono::system_clock::now());
}

[[nodiscard]] auto trim_copy(std::string_view value) -> std::string {
    auto begin = value.begin();
    auto end   = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

[[nodiscard]] auto lowercase_copy(std::string_view value) -> std::string {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

[[nodiscard]] auto sanitize_identifier(std::string_view input, std::string_view fallback) -> std::string {
    auto trimmed = trim_copy(input);
    if (trimmed.empty()) {
        trimmed = std::string(fallback);
    }

    std::string sanitized;
    sanitized.reserve(trimmed.size());
    for (char ch : trimmed) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            sanitized.push_back(static_cast<char>(std::tolower(uch)));
            continue;
        }
        if (ch == '-' || ch == '_') {
            if (!sanitized.empty()) {
                sanitized.push_back(ch);
            }
            continue;
        }
        if (std::isspace(uch) || ch == '/' || ch == '.') {
            if (!sanitized.empty() && sanitized.back() == '-') {
                continue;
            }
            sanitized.push_back('-');
        }
    }

    while (!sanitized.empty() && (sanitized.front() == '-' || sanitized.front() == '_' || sanitized.front() == '.')) {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && (sanitized.back() == '-' || sanitized.back() == '_' )) {
        sanitized.pop_back();
    }

    if (sanitized.empty()) {
        sanitized = fallback.empty() ? std::string{"entry"} : std::string{fallback};
    }
    if (sanitized.empty()) {
        sanitized = "entry";
    }
    if (!sanitized.empty() && sanitized.front() == '.') {
        sanitized.insert(sanitized.begin(), 'w');
    }
    if (sanitized.size() > kMaxWatchlistIdLength) {
        sanitized.resize(kMaxWatchlistIdLength);
    }
    return sanitized;
}

[[nodiscard]] auto sanitize_user_identifier(std::string_view raw) -> std::string {
    auto trimmed = trim_copy(raw);
    if (trimmed.empty()) {
        trimmed = "anonymous";
    }
    return sanitize_identifier(trimmed, "anonymous");
}

[[nodiscard]] auto sanitize_watchlist_identifier(std::string_view raw) -> std::string {
    return sanitize_identifier(raw, "watchlist");
}

[[nodiscard]] auto sanitize_panel_identifier(std::string_view raw) -> std::string {
    return sanitize_identifier(raw, "panel");
}

[[nodiscard]] auto watchlist_root_for_user(std::string const& user_id) -> std::string {
    std::string root = "/inspector/user/";
    root.append(user_id.empty() ? "anonymous" : user_id);
    root.append("/watchlists");
    return NormalizeInspectorPath(std::move(root));
}

[[nodiscard]] auto watchlist_trash_root_for_user(std::string const& user_id) -> std::string {
    std::string root = "/inspector/user/";
    root.append(user_id.empty() ? "anonymous" : user_id);
    root.append("/watchlists_trash");
    return NormalizeInspectorPath(std::move(root));
}

[[nodiscard]] auto join_path(std::string root, std::string_view leaf) -> std::string {
    if (root.empty()) {
        root = "/";
    }
    if (root.back() != '/') {
        root.push_back('/');
    }
    root.append(leaf);
    return NormalizeInspectorPath(std::move(root));
}

[[nodiscard]] auto build_watchlist_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(root, id);
}

auto ensure_placeholder(PathSpace& space, std::string const& root) -> Expected<void> {
    auto placeholder = join_path(root, ".keep");
    auto inserted    = space.insert(placeholder, std::uint64_t{0});
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

[[nodiscard]] auto canonicalize_watch_path(std::string_view path) -> std::optional<std::string> {
    auto cleaned = trim_copy(path);
    if (cleaned.empty() || cleaned.front() != '/') {
        return std::nullopt;
    }
    auto normalized = NormalizeInspectorPath(cleaned);
    if (normalized.empty()) {
        return std::nullopt;
    }
    return normalized;
}

[[nodiscard]] auto deduplicate_paths(std::vector<std::string> paths) -> std::vector<std::string> {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

struct BoolValueState {
    bool value   = false;
    bool existed = false;
};

struct WriteToggleApplyResult {
    bool previous = false;
    bool current  = false;
};

struct WriteToggleRequestContext {
    std::string role;
    std::string user;
    std::string client;
};

struct WriteToggleAuditEvent {
    std::string  action_id;
    std::string  action_label;
    std::string  path;
    std::string  kind;
    std::string  role;
    std::string  user;
    std::string  client;
    std::string  note;
    std::string  message;
    bool         previous_value = false;
    bool         new_value      = false;
    bool         success        = false;
    std::uint64_t timestamp_ms  = 0;
};

[[nodiscard]] auto is_role_allowed(std::vector<std::string> const& allowed_roles,
                                   std::string const& candidate) -> bool {
    if (allowed_roles.empty()) {
        return candidate == "root";
    }
    return std::find(allowed_roles.begin(), allowed_roles.end(), candidate) != allowed_roles.end();
}

[[nodiscard]] auto inspector_write_kind_string(InspectorWriteToggleKind kind) -> std::string_view {
    switch (kind) {
    case InspectorWriteToggleKind::ToggleBool:
        return "toggle_bool";
    case InspectorWriteToggleKind::SetBool:
        return "set_bool";
    }
    return "unknown";
}

[[nodiscard]] auto read_bool_state(PathSpace& space,
                                   std::string const& path,
                                   bool fallback) -> Expected<BoolValueState> {
    auto value = space.read<bool, std::string>(path);
    if (value) {
        return BoolValueState{*value, true};
    }
    auto const& error = value.error();
    if (error.code == Error::Code::NoObjectFound || error.code == Error::Code::NoSuchPath) {
        return BoolValueState{fallback, false};
    }
    return std::unexpected(error);
}

[[nodiscard]] auto clear_bool_values(PathSpace& space, std::string const& path) -> Expected<void> {
    while (true) {
        auto existing = space.take<bool, std::string>(path);
        if (existing) {
            continue;
        }
        auto const code = existing.error().code;
        if (code == Error::Code::NoSuchPath || code == Error::Code::NoObjectFound) {
            break;
        }
        return std::unexpected(existing.error());
    }
    return {};
}

[[nodiscard]] auto apply_write_toggle_action(PathSpace& space,
                                             InspectorWriteToggleAction const& action)
    -> Expected<WriteToggleApplyResult> {
    auto state = read_bool_state(space, action.path, action.default_state);
    if (!state) {
        return std::unexpected(state.error());
    }
    auto cleared = clear_bool_values(space, action.path);
    if (!cleared) {
        return std::unexpected(cleared.error());
    }
    bool desired = action.kind == InspectorWriteToggleKind::ToggleBool ? !state->value
                                                                       : action.default_state;
    auto inserted = space.insert(action.path, desired);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return WriteToggleApplyResult{state->value, desired};
}

[[nodiscard]] auto write_confirmation_present(httplib::Request const& req,
                                              InspectorWriteToggleOptions const& options) -> bool {
    if (options.confirmation_header.empty()) {
        return true;
    }
    auto header = req.get_header_value(options.confirmation_header);
    if (header.empty()) {
        return false;
    }
    if (options.confirmation_token.empty()) {
        return true;
    }
    return header == options.confirmation_token;
}

auto record_write_audit_event(PathSpace& space,
                              InspectorWriteToggleOptions const& options,
                              WriteToggleAuditEvent const& event) -> void {
    if (options.audit_root.empty()) {
        return;
    }
    auto        root       = NormalizeInspectorPath(options.audit_root);
    std::string total_path = join_path(root, "total");
    auto        total      = space.read<std::uint64_t, std::string>(total_path);
    std::uint64_t next_total = total ? (*total + 1ULL) : 1ULL;
    auto          total_insert = space.insert(total_path, next_total);
    if (!total_insert.errors.empty()) {
        // Best-effort logging; ignore failures.
    }

    auto last_root = join_path(root, "last");
    auto publish_string = [&](std::string const& leaf, std::string const& value) {
        auto inserted = space.insert(join_path(last_root, leaf), value);
        (void)inserted;
    };
    auto publish_bool = [&](std::string const& leaf, bool value) {
        auto inserted = space.insert(join_path(last_root, leaf), value);
        (void)inserted;
    };
    auto publish_uint = [&](std::string const& leaf, std::uint64_t value) {
        auto inserted = space.insert(join_path(last_root, leaf), value);
        (void)inserted;
    };

    publish_uint("timestamp_ms", event.timestamp_ms);
    publish_string("action_id", event.action_id);
    publish_string("label", event.action_label);
    publish_string("path", event.path);
    publish_string("kind", event.kind);
    publish_string("role", event.role);
    publish_string("user", event.user);
    publish_string("client", event.client);
    publish_string("message", event.message);
    publish_string("note", event.note);
    publish_bool("previous_state", event.previous_value);
    publish_bool("new_state", event.new_value);
    publish_string("outcome", event.success ? "success" : "failure");

    auto events_root = join_path(root, "events");
    auto identifier  = sanitize_identifier(event.action_id, "action");
    auto event_path = join_path(events_root,
                                std::to_string(event.timestamp_ms) + "-" + identifier);
    nlohmann::json payload{{"timestamp_ms", event.timestamp_ms},
                           {"action_id", event.action_id},
                           {"label", event.action_label},
                           {"path", event.path},
                           {"kind", event.kind},
                           {"role", event.role},
                           {"user", event.user},
                           {"client", event.client},
                           {"note", event.note},
                           {"message", event.message},
                           {"success", event.success},
                           {"previous_state", event.previous_value},
                           {"new_state", event.new_value}};
    auto inserted = space.insert(event_path, payload.dump());
    (void)inserted;
}

[[nodiscard]] auto find_write_toggle_action(std::vector<InspectorWriteToggleAction> const& actions,
                                            std::string const& id)
    -> InspectorWriteToggleAction const* {
    for (auto const& action : actions) {
        if (action.id == id) {
            return &action;
        }
    }
    return nullptr;
}

[[nodiscard]] auto read_watchlist(PathSpace& space, std::string const& root, std::string const& id)
    -> std::optional<WatchlistRecord> {
    if (auto nested = read_watchlist_from_nested(space, root, id)) {
        return nested;
    }
    auto path = build_watchlist_path(root, id);
    return read_watchlist_legacy(space, path, id);
}

[[nodiscard]] auto list_watchlists(PathSpace& space, std::string const& root) -> std::vector<WatchlistRecord> {
    migrate_watchlists(space, root);
    std::vector<WatchlistRecord> records;
    auto children = space.listChildren(SP::ConcretePathStringView{root});
    for (auto const& child : children) {
        if (child.empty() || child.front() == '.') {
            continue;
        }
        auto record = read_watchlist(space, root, child);
        if (record) {
            records.push_back(std::move(*record));
        }
    }

    std::sort(records.begin(), records.end(), [](WatchlistRecord const& lhs, WatchlistRecord const& rhs) {
        if (lhs.updated_ms == rhs.updated_ms) {
            return lhs.name < rhs.name;
        }
        return lhs.updated_ms > rhs.updated_ms;
    });
    return records;
}

auto persist_watchlist(PathSpace& space, std::string const& path, WatchlistRecord const& record)
    -> Expected<void> {
    auto nested = std::make_unique<PathSpace>();
    auto insert_value = [&](std::string const& target, auto&& value) -> Expected<void> {
        auto inserted = nested->insert(target, std::forward<decltype(value)>(value));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        return {};
    };

    if (auto result = insert_value("/meta/id", record.id); !result) {
        return result;
    }
    if (auto result = insert_value("/meta/name", record.name); !result) {
        return result;
    }
    if (auto result = insert_value("/meta/created_ms", record.created_ms); !result) {
        return result;
    }
    if (auto result = insert_value("/meta/updated_ms", record.updated_ms); !result) {
        return result;
    }
    if (auto result = insert_value("/meta/count", static_cast<std::uint64_t>(record.paths.size())); !result) {
        return result;
    }
    if (auto result = insert_value("/meta/version", kWatchlistSpaceVersion); !result) {
        return result;
    }
    if (auto result = insert_value("/paths", record.paths); !result) {
        return result;
    }

    auto target = watchlist_space_node(path);
    auto inserted = space.insert(target, std::move(nested));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto remove_watchlist(PathSpace& space,
                      std::string const& root,
                      std::string const& trash_root,
                      std::string const& id) -> Expected<bool> {
    auto source_space = build_watchlist_space_path(root, id);
    auto source_base  = build_watchlist_path(root, id);

    auto dest_leaf = id;
    dest_leaf.push_back('-');
    dest_leaf.append(std::to_string(now_ms()));
    auto destination_base  = build_watchlist_path(trash_root, dest_leaf);
    auto destination_space = build_watchlist_space_path(trash_root, dest_leaf);

    auto taken = space.take<std::unique_ptr<PathSpace>>(source_space);
    if (taken) {
        auto owned   = std::move(*taken);
        auto inserted = space.insert(destination_space, std::move(owned));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        clear_legacy_watchlist_payload(space, source_base);
        return true;
    }
    if (taken.error().code != Error::Code::NoSuchPath) {
        return std::unexpected(taken.error());
    }

    auto legacy = move_legacy_watchlist(space, source_base, destination_base, id);
    if (!legacy) {
        return std::unexpected(legacy.error());
    }
    return legacy.value();
}

[[nodiscard]] auto make_watchlist_json(WatchlistRecord const& record) -> nlohmann::json {
    return nlohmann::json{{"id", record.id},
                          {"name", record.name},
                          {"paths", record.paths},
                          {"count", record.paths.size()},
                          {"created_ms", record.created_ms},
                          {"updated_ms", record.updated_ms}};
}

[[nodiscard]] auto make_unique_watchlist_id(std::string base,
                                            std::unordered_set<std::string> const& existing,
                                            std::unordered_set<std::string> const& pending) -> std::string {
    if (base.empty()) {
        base = "watchlist";
    }

    std::size_t suffix   = 2;
    std::string candidate = base;
    while (existing.contains(candidate) || pending.contains(candidate)) {
        std::string next = base;
        next.push_back('-');
        next.append(std::to_string(suffix++));
        if (next.size() > kMaxWatchlistIdLength) {
            next.resize(kMaxWatchlistIdLength);
        }
        candidate = std::move(next);
    }
    return candidate;
}

struct WatchlistInput {
    std::string              name;
    std::string              requested_id;
    bool                     id_provided = false;
    std::vector<std::string> paths;
};

[[nodiscard]] auto parse_watchlist_input(nlohmann::json const& payload,
                                         std::size_t max_paths,
                                         std::string& error) -> std::optional<WatchlistInput> {
    if (!payload.is_object()) {
        error = "watchlist entry must be an object";
        return std::nullopt;
    }
    auto name_it = payload.find("name");
    if (name_it == payload.end() || !name_it->is_string()) {
        error = "watchlist name is required";
        return std::nullopt;
    }
    auto name = trim_copy(name_it->get<std::string>());
    if (name.empty()) {
        error = "watchlist name cannot be empty";
        return std::nullopt;
    }

    WatchlistInput input;
    input.name = std::move(name);

    if (auto id_it = payload.find("id"); id_it != payload.end()) {
        if (!id_it->is_string()) {
            error = "watchlist id must be a string";
            return std::nullopt;
        }
        input.requested_id = trim_copy(id_it->get<std::string>());
        input.id_provided  = !input.requested_id.empty();
    }

    auto paths_it = payload.find("paths");
    if (paths_it == payload.end() || !paths_it->is_array()) {
        error = "paths must be an array";
        return std::nullopt;
    }

    std::vector<std::string> paths;
    for (auto const& entry : *paths_it) {
        if (!entry.is_string()) {
            error = "paths must contain only strings";
            return std::nullopt;
        }
        auto normalized = canonicalize_watch_path(entry.get<std::string>());
        if (!normalized) {
            error = "paths must be absolute (e.g. /app/node)";
            return std::nullopt;
        }
        paths.push_back(*normalized);
    }

    paths = deduplicate_paths(std::move(paths));
    if (paths.size() > max_paths) {
        error = "too many paths in watchlist";
        return std::nullopt;
    }

    input.paths = std::move(paths);
    return input;
}

constexpr std::size_t kMaxSnapshotIdLength = 64;

struct SnapshotRecord {
    std::string                id;
    std::string                label;
    std::string                note;
    std::uint64_t              created_ms = 0;
    InspectorSnapshotOptions   options;
    std::size_t                inspector_bytes = 0;
    std::size_t                export_bytes    = 0;
    std::size_t                diagnostics     = 0;
    std::string                inspector_payload;
    std::string                export_payload;

    [[nodiscard]] auto total_bytes() const -> std::size_t {
        return inspector_bytes + export_bytes;
    }
};

struct SnapshotContext {
    std::string display_user;
    std::string user_id;
    std::string root;
    std::string trash_root;
};

[[nodiscard]] auto sanitize_snapshot_identifier(std::string_view raw) -> std::string {
    return sanitize_identifier(raw, "snapshot");
}

[[nodiscard]] auto snapshot_root_for_user(std::string const& user_id) -> std::string {
    std::string root = "/inspector/user/";
    root.append(user_id.empty() ? "anonymous" : user_id);
    root.append("/snapshots");
    return NormalizeInspectorPath(std::move(root));
}

[[nodiscard]] auto snapshot_trash_root_for_user(std::string const& user_id) -> std::string {
    std::string root = "/inspector/user/";
    root.append(user_id.empty() ? "anonymous" : user_id);
    root.append("/snapshots_trash");
    return NormalizeInspectorPath(std::move(root));
}

[[nodiscard]] auto snapshot_node_root(std::string const& root, std::string const& id) -> std::string {
    return join_path(root, id);
}

[[nodiscard]] auto snapshot_space_root(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_node_root(root, id), "space");
}

[[nodiscard]] auto snapshot_meta_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_space_root(root, id), "meta");
}

[[nodiscard]] auto snapshot_payload_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_space_root(root, id), "inspector");
}

[[nodiscard]] auto snapshot_export_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_space_root(root, id), "export");
}

[[nodiscard]] auto legacy_snapshot_meta_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_node_root(root, id), "meta");
}

[[nodiscard]] auto legacy_snapshot_payload_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_node_root(root, id), "inspector");
}

[[nodiscard]] auto legacy_snapshot_export_path(std::string const& root, std::string const& id) -> std::string {
    return join_path(snapshot_node_root(root, id), "export");
}

auto clear_legacy_snapshot_payload(PathSpace& space, std::string const& root, std::string const& id) -> void {
    auto base = snapshot_node_root(root, id);
    auto remove_value = [&](std::string const& suffix) {
        auto target = join_path(base, suffix);
        while (true) {
            auto removed = space.take<std::string>(target);
            if (!removed) {
                if (removed.error().code == Error::Code::NoSuchPath) {
                    break;
                }
                break;
            }
        }
    };

    remove_value("meta");
    remove_value("inspector");
    remove_value("export");
}

auto persist_snapshot_storage(PathSpace& space,
                              std::string const& root,
                              std::string const& id,
                              std::string meta_payload,
                              std::string inspector_payload,
                              std::string export_payload) -> Expected<void> {
    auto nested = std::make_unique<PathSpace>();
    auto insert_value = [&](std::string const& target, std::string value) -> Expected<void> {
        auto inserted = nested->insert(target, std::move(value));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        return {};
    };

    if (auto result = insert_value("/meta", std::move(meta_payload)); !result) {
        return result;
    }
    if (auto result = insert_value("/inspector", std::move(inspector_payload)); !result) {
        return result;
    }
    if (auto result = insert_value("/export", std::move(export_payload)); !result) {
        return result;
    }

    auto target  = snapshot_space_root(root, id);
    auto inserted = space.insert(target, std::move(nested));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto persist_snapshot_space(PathSpace& space,
                            std::string const& root,
                            SnapshotRecord const& record,
                            std::string meta_payload) -> Expected<void> {
    return persist_snapshot_storage(space,
                                    root,
                                    record.id,
                                    std::move(meta_payload),
                                    record.inspector_payload,
                                    record.export_payload);
}

auto ensure_snapshot_nested(PathSpace& space,
                            std::string const& root,
                            std::string const& id) -> Expected<bool> {
    auto nested_meta = space.read<std::string, std::string>(snapshot_meta_path(root, id));
    if (nested_meta) {
        return true;
    }
    if (nested_meta.error().code != Error::Code::NoSuchPath) {
        return std::unexpected(nested_meta.error());
    }

    auto legacy_meta = space.read<std::string, std::string>(legacy_snapshot_meta_path(root, id));
    if (!legacy_meta) {
        if (legacy_meta.error().code == Error::Code::NoSuchPath) {
            return false;
        }
        return std::unexpected(legacy_meta.error());
    }

    auto inspector_payload = space.read<std::string, std::string>(legacy_snapshot_payload_path(root, id));
    if (!inspector_payload) {
        if (inspector_payload.error().code == Error::Code::NoSuchPath) {
            return false;
        }
        return std::unexpected(inspector_payload.error());
    }

    auto export_payload = space.read<std::string, std::string>(legacy_snapshot_export_path(root, id));
    if (!export_payload) {
        if (export_payload.error().code == Error::Code::NoSuchPath) {
            return false;
        }
        return std::unexpected(export_payload.error());
    }

    if (auto persisted = persist_snapshot_storage(space,
                                                  root,
                                                  id,
                                                  std::move(*legacy_meta),
                                                  std::move(*inspector_payload),
                                                  std::move(*export_payload));
        !persisted) {
        return std::unexpected(persisted.error());
    }
    clear_legacy_snapshot_payload(space, root, id);
    return true;
}

auto migrate_snapshots(PathSpace& space, std::string const& root) -> void {
    auto children = space.listChildren(SP::ConcretePathStringView{root});
    for (auto const& child : children) {
        if (child.empty() || child.front() == '.') {
            continue;
        }
        auto migrated = ensure_snapshot_nested(space, root, child);
        if (!migrated) {
            if (migrated.error().code == Error::Code::NoSuchPath) {
                continue;
            }
        }
    }
}

[[nodiscard]] auto snapshot_options_to_json(InspectorSnapshotOptions const& options)
    -> nlohmann::json {
    return nlohmann::json{{"root", options.root},
                          {"max_depth", options.max_depth},
                          {"max_children", options.max_children},
                          {"include_values", options.include_values}};
}

[[nodiscard]] auto augment_snapshot_export(std::string const& payload,
                                           InspectorSnapshotOptions const& options)
    -> std::optional<std::string> {
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::nullopt;
    }
    json["options"] = snapshot_options_to_json(options);
    return json.dump(2);
}

[[nodiscard]] auto snapshot_options_from_json(nlohmann::json const& json)
    -> InspectorSnapshotOptions {
    InspectorSnapshotOptions options;
    if (auto it = json.find("root"); it != json.end() && it->is_string()) {
        options.root = NormalizeInspectorPath(it->get<std::string>());
    }
    if (auto it = json.find("max_depth"); it != json.end() && it->is_number_unsigned()) {
        options.max_depth = it->get<std::size_t>();
    }
    if (auto it = json.find("max_children"); it != json.end() && it->is_number_unsigned()) {
        options.max_children = it->get<std::size_t>();
    }
    if (auto it = json.find("include_values"); it != json.end() && it->is_boolean()) {
        options.include_values = it->get<bool>();
    }
    return options;
}

[[nodiscard]] auto read_snapshot_blob(PathSpace& space, std::string const& path)
    -> Expected<std::string> {
    return space.read<std::string, std::string>(path);
}

[[nodiscard]] auto read_snapshot_record(PathSpace& space,
                                        std::string const& root,
                                        std::string const& id,
                                        bool load_payloads) -> std::optional<SnapshotRecord> {
    auto meta_blob = read_snapshot_blob(space, snapshot_meta_path(root, id));
    bool use_nested = true;
    std::string meta_payload;
    if (meta_blob) {
        meta_payload = std::move(*meta_blob);
    } else {
        if (meta_blob.error().code != Error::Code::NoSuchPath) {
            return std::nullopt;
        }
        use_nested = false;
        auto legacy_meta = read_snapshot_blob(space, legacy_snapshot_meta_path(root, id));
        if (!legacy_meta) {
            return std::nullopt;
        }
        meta_payload = std::move(*legacy_meta);
    }

    auto meta_json = nlohmann::json::parse(meta_payload, nullptr, false);
    if (meta_json.is_discarded() || !meta_json.is_object()) {
        return std::nullopt;
    }

    SnapshotRecord record;
    record.id    = id;
    record.label = trim_copy(meta_json.value("label", id));
    record.note  = trim_copy(meta_json.value("note", std::string{}));
    record.created_ms = read_uint64(meta_json, "created_ms");
    record.options     = snapshot_options_from_json(meta_json.value("options", nlohmann::json::object()));
    record.inspector_bytes = static_cast<std::size_t>(meta_json.value("inspector_bytes", std::uint64_t{0}));
    record.export_bytes    = static_cast<std::size_t>(meta_json.value("export_bytes", std::uint64_t{0}));
    record.diagnostics     = static_cast<std::size_t>(meta_json.value("diagnostics", std::uint64_t{0}));

    if (load_payloads) {
        auto inspector_blob = read_snapshot_blob(space,
                                                use_nested ? snapshot_payload_path(root, id)
                                                           : legacy_snapshot_payload_path(root, id));
        auto export_blob = read_snapshot_blob(space,
                                              use_nested ? snapshot_export_path(root, id)
                                                         : legacy_snapshot_export_path(root, id));
        if (!inspector_blob || !export_blob) {
            return std::nullopt;
        }
        record.inspector_payload = std::move(*inspector_blob);
        record.export_payload    = std::move(*export_blob);
        record.inspector_bytes   = record.inspector_payload.size();
        record.export_bytes      = record.export_payload.size();
    }

    if (!use_nested) {
        auto migrated = ensure_snapshot_nested(space, root, id);
        (void)migrated;
    }

    return record;
}

[[nodiscard]] auto list_snapshots(PathSpace& space, std::string const& root)
    -> std::vector<SnapshotRecord> {
    migrate_snapshots(space, root);
    std::vector<SnapshotRecord> records;
    auto children = space.listChildren(SP::ConcretePathStringView{root});
    records.reserve(children.size());
    for (auto const& child : children) {
        if (child.empty() || child.front() == '.') {
            continue;
        }
        auto record = read_snapshot_record(space, root, child, false);
        if (record) {
            records.push_back(std::move(*record));
        }
    }
    std::sort(records.begin(), records.end(), [](SnapshotRecord const& lhs, SnapshotRecord const& rhs) {
        if (lhs.created_ms == rhs.created_ms) {
            return lhs.id < rhs.id;
        }
        return lhs.created_ms > rhs.created_ms;
    });
    return records;
}

[[nodiscard]] auto make_snapshot_json(SnapshotRecord const& record) -> nlohmann::json {
    return nlohmann::json{{"id", record.id},
                          {"label", record.label},
                          {"note", record.note},
                          {"created_ms", record.created_ms},
                          {"diagnostics", record.diagnostics},
                          {"inspector_bytes", record.inspector_bytes},
                          {"export_bytes", record.export_bytes},
                          {"total_bytes", record.total_bytes()},
                          {"options", snapshot_options_to_json(record.options)}};
}

auto persist_snapshot_record(PathSpace& space,
                             SnapshotContext const& context,
                             SnapshotRecord const& record) -> Expected<void> {
    if (auto ensured = ensure_placeholder(space, context.root); !ensured) {
        return ensured;
    }
    if (auto ensured = ensure_placeholder(space, context.trash_root); !ensured) {
        return ensured;
    }

    nlohmann::json meta{{"id", record.id},
                        {"label", record.label},
                        {"note", record.note},
                        {"created_ms", record.created_ms},
                        {"inspector_bytes", record.inspector_bytes},
                        {"export_bytes", record.export_bytes},
                        {"diagnostics", record.diagnostics},
                        {"options", snapshot_options_to_json(record.options)},
                        {"version", 1}};
    return persist_snapshot_space(space, context.root, record, meta.dump());
}

auto delete_snapshot_record(PathSpace& space,
                            SnapshotContext const& context,
                            std::string const& id) -> Expected<bool> {
    if (auto ensured = ensure_placeholder(space, context.trash_root); !ensured) {
        return std::unexpected(ensured.error());
    }
    auto source_space = snapshot_space_root(context.root, id);
    auto dest_leaf    = id;
    dest_leaf.push_back('-');
    dest_leaf.append(std::to_string(now_ms()));
    auto destination_space = snapshot_space_root(context.trash_root, dest_leaf);

    auto taken = space.take<std::unique_ptr<PathSpace>>(source_space);
    if (!taken) {
        if (taken.error().code != Error::Code::NoSuchPath) {
            return std::unexpected(taken.error());
        }
        auto migrated = ensure_snapshot_nested(space, context.root, id);
        if (!migrated) {
            if (migrated.error().code == Error::Code::NoSuchPath) {
                return false;
            }
            return std::unexpected(migrated.error());
        }
        if (!*migrated) {
            return false;
        }
        taken = space.take<std::unique_ptr<PathSpace>>(source_space);
        if (!taken) {
            if (taken.error().code == Error::Code::NoSuchPath) {
                return false;
            }
            return std::unexpected(taken.error());
        }
    }

    auto inserted = space.insert(destination_space, std::move(*taken));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return true;
}

auto trim_snapshots(PathSpace& space,
                    SnapshotContext const& context,
                    std::size_t max_snapshots) -> Expected<void> {
    if (max_snapshots == 0) {
        return {};
    }
    auto records = list_snapshots(space, context.root);
    if (records.size() <= max_snapshots) {
        return {};
    }
    for (std::size_t idx = max_snapshots; idx < records.size(); ++idx) {
        auto removed = delete_snapshot_record(space, context, records[idx].id);
        if (!removed) {
            return std::unexpected(removed.error());
        }
    }
    return {};
}

[[nodiscard]] auto make_unique_snapshot_id(std::string base,
                                           std::unordered_set<std::string> const& existing)
    -> std::string {
    if (base.empty()) {
        base = "snapshot";
    }
    std::size_t suffix   = 2;
    std::string candidate = base;
    auto clipped = [](std::string value) {
        if (value.size() > kMaxSnapshotIdLength) {
            value.resize(kMaxSnapshotIdLength);
        }
        return value;
    };
    candidate = clipped(candidate);
    while (existing.contains(candidate)) {
        std::string next = base;
        next.push_back('-');
        next.append(std::to_string(suffix++));
        candidate = clipped(std::move(next));
    }
    return candidate;
}

class RemoteWaiterGuard {
public:
    RemoteWaiterGuard(RemoteMountManager* manager, std::string const& root)
        : manager_(manager) {
        if (manager_ != nullptr) {
            alias_ = manager_->aliasForRoot(root);
            if (alias_) {
                manager_->incrementWaiters(*alias_);
            }
        }
    }

    ~RemoteWaiterGuard() {
        release();
    }

    void release() {
        if (manager_ != nullptr && alias_) {
            manager_->decrementWaiters(*alias_);
            alias_.reset();
        }
    }

    RemoteWaiterGuard(RemoteWaiterGuard const&) = delete;
    auto operator=(RemoteWaiterGuard const&) -> RemoteWaiterGuard& = delete;

private:
    RemoteMountManager*            manager_ = nullptr;
    std::optional<std::string>     alias_;
};

class StreamSession {
public:
    StreamSession(PathSpace& space,
                  InspectorSnapshotOptions options,
                  std::chrono::milliseconds poll_interval,
                  std::chrono::milliseconds keepalive_interval,
                  std::chrono::milliseconds idle_timeout,
                  std::size_t max_pending_events,
                  std::size_t max_events_per_tick,
                  StreamMetricsRecorder& metrics,
                  RemoteMountManager* remote_mounts)
        : space_(space)
        , options_(std::move(options))
        , poll_interval_(poll_interval)
        , keepalive_interval_(keepalive_interval)
        , idle_timeout_(idle_timeout)
        , max_pending_events_(std::max<std::size_t>(1, max_pending_events))
        , max_events_per_tick_(std::max<std::size_t>(1, max_events_per_tick))
        , metrics_(metrics)
        , remote_mounts_(remote_mounts)
        , next_poll_(std::chrono::steady_clock::now()) {
        metrics_.record_session_started();
        acquire_remote_waiter();
    }

    ~StreamSession() {
        release_remote_waiter();
        finalize(StreamDisconnectReason::Server);
    }

    auto cancel(StreamDisconnectReason reason) -> void {
        disconnect_reason_.store(reason, std::memory_order_relaxed);
        cancelled_.store(true, std::memory_order_release);
        release_remote_waiter();
    }

    auto finalize(StreamDisconnectReason fallback_reason) -> void {
        auto reason = disconnect_reason_.load(std::memory_order_relaxed);
        if (reason == StreamDisconnectReason::Client) {
            reason = fallback_reason;
        }
        bool expected = false;
        if (disconnect_recorded_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            metrics_.record_session_ended(reason);
        }
    }

    [[nodiscard]] auto disconnect_reason() const -> StreamDisconnectReason {
        return disconnect_reason_.load(std::memory_order_relaxed);
    }

    auto pump(httplib::DataSink& sink) -> bool {
        if (cancelled_.load(std::memory_order_acquire)) {
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        if (idle_timeout_.count() > 0 && now - last_emit_ >= idle_timeout_) {
            cancel(StreamDisconnectReason::Timeout);
            return false;
        }

        if (drain_pending(sink)) {
            return true;
        }

        if (!snapshot_) {
            auto snapshot = build_snapshot();
            if (!snapshot) {
                emit_error(describeError(snapshot.error()));
                drain_pending(sink);
                return true;
            }
            snapshot_ = std::move(*snapshot);
            version_  = 1;
            enqueue_snapshot();
            drain_pending(sink);
            return true;
        }

        wait_for_next_poll();
        if (cancelled_.load(std::memory_order_acquire)) {
            return false;
        }

        auto snapshot = build_snapshot();
        if (!snapshot) {
            emit_error(describeError(snapshot.error()));
            drain_pending(sink);
            return true;
        }
        last_error_.clear();

        auto delta = BuildInspectorStreamDelta(*snapshot_, *snapshot, version_ + 1);
        if (delta.has_changes()) {
            snapshot_ = std::move(*snapshot);
            ++version_;
            enqueue_event("delta", SerializeInspectorStreamDeltaEvent(delta, -1));
            drain_pending(sink);
            return true;
        }

        now = std::chrono::steady_clock::now();
        if (now - last_emit_ >= keepalive_interval_) {
            enqueue_comment(make_keepalive_block(version_));
            drain_pending(sink);
            return true;
        }
        return true;
    }

private:
    enum class ChunkKind { Event, Comment };

    struct PendingChunk {
        ChunkKind   kind   = ChunkKind::Event;
        std::string name;
        std::string payload;
    };

    void wait_for_next_poll() {
        auto now = std::chrono::steady_clock::now();
        if (now < next_poll_) {
            std::this_thread::sleep_until(next_poll_);
        }
        next_poll_ = std::chrono::steady_clock::now() + poll_interval_;
    }

    void sleep_interval() {
        std::this_thread::sleep_for(poll_interval_);
        next_poll_ = std::chrono::steady_clock::now() + poll_interval_;
    }

    auto drain_pending(httplib::DataSink& sink) -> bool {
        if (pending_.empty()) {
            overflow_resets_ = 0;
            return false;
        }
        std::size_t sent = 0;
        while (!pending_.empty() && sent < max_events_per_tick_) {
            auto chunk = std::move(pending_.front());
            pending_.pop_front();
            if (chunk.kind == ChunkKind::Event) {
                write_event(sink, chunk.name, chunk.payload);
            } else {
                sink.write(chunk.payload.data(), chunk.payload.size());
            }
            ++sent;
            metrics_.record_queue_depth(pending_.size());
            last_emit_ = std::chrono::steady_clock::now();
        }
        if (pending_.empty()) {
            overflow_resets_ = 0;
        }
        return sent > 0;
    }

    void enqueue_event(std::string event_name, std::string payload) {
        pending_.push_back(PendingChunk{ChunkKind::Event, std::move(event_name), std::move(payload)});
        metrics_.record_queue_depth(pending_.size());
        enforce_queue_budget();
    }

    void enqueue_snapshot() {
        if (!snapshot_) {
            return;
        }
        enqueue_event("snapshot", SerializeInspectorStreamSnapshotEvent(*snapshot_, version_, -1));
    }

    void enqueue_comment(std::string payload) {
        pending_.push_back(PendingChunk{ChunkKind::Comment, {}, std::move(payload)});
        metrics_.record_queue_depth(pending_.size());
        enforce_queue_budget();
    }

    [[nodiscard]] auto make_keepalive_block(std::uint64_t version) const -> std::string {
        std::string comment = ": keep-alive ";
        comment.append(std::to_string(version));
        comment.append("\n\n");
        return comment;
    }

    void enforce_queue_budget() {
        if (pending_.size() <= max_pending_events_) {
            return;
        }
        auto dropped = pending_.size();
        pending_.clear();
        metrics_.record_drop(dropped);
        metrics_.record_queue_depth(0);
        if (snapshot_) {
            metrics_.record_snapshot_resent();
            enqueue_snapshot();
        }
        ++overflow_resets_;
        if (overflow_resets_ > kMaxOverflowResets) {
            cancel(StreamDisconnectReason::Backpressure);
        }
    }

    void emit_error(std::string const& message) {
        auto payload = make_stream_error_payload(message);
        if (payload != last_error_) {
            last_error_ = payload;
            enqueue_event("error", payload);
        }
        sleep_interval();
    }

    void acquire_remote_waiter() {
        if (remote_mounts_ == nullptr || remote_alias_) {
            return;
        }
        if (auto alias = remote_mounts_->aliasForRoot(options_.root)) {
            remote_alias_ = *alias;
            remote_mounts_->incrementWaiters(*remote_alias_);
        }
    }

    void release_remote_waiter() {
        if (remote_mounts_ != nullptr && remote_alias_) {
            remote_mounts_->decrementWaiters(*remote_alias_);
            remote_alias_.reset();
        }
    }

    static void write_event(httplib::DataSink& sink,
                            std::string_view event_name,
                            std::string const& payload) {
        std::string block;
        block.reserve(payload.size() + 32);
        block.append("event: ");
        block.append(event_name);
        block.append("\n");
        std::size_t start = 0U;
        while (start < payload.size()) {
            auto end = payload.find('\n', start);
            auto len = (end == std::string::npos ? payload.size() : end) - start;
            block.append("data: ");
            block.append(payload.data() + start, len);
            block.append("\n");
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        block.append("\n");
        sink.write(block.data(), block.size());
    }

    auto build_snapshot() -> Expected<InspectorSnapshot> {
        if (remote_mounts_) {
            if (auto remote = remote_mounts_->buildRemoteSnapshot(options_)) {
                return *remote;
            }
        }
        auto snapshot = BuildInspectorSnapshot(space_, options_);
        if (!snapshot) {
            return snapshot;
        }
        if (remote_mounts_) {
            remote_mounts_->augmentLocalSnapshot(*snapshot);
        }
        return snapshot;
    }

    PathSpace&                             space_;
    InspectorSnapshotOptions               options_;
    std::chrono::milliseconds              poll_interval_;
    std::chrono::milliseconds              keepalive_interval_;
    std::chrono::milliseconds              idle_timeout_;
    std::size_t                            max_pending_events_;
    std::size_t                            max_events_per_tick_;
    StreamMetricsRecorder&                 metrics_;
    RemoteMountManager*                    remote_mounts_;
    std::deque<PendingChunk>               pending_;
    std::size_t                            overflow_resets_ = 0;
    static constexpr std::size_t           kMaxOverflowResets = 2;
    std::atomic<bool>                      cancelled_{false};
    std::atomic<bool>                      disconnect_recorded_{false};
    std::atomic<StreamDisconnectReason>    disconnect_reason_{StreamDisconnectReason::Client};
    std::optional<InspectorSnapshot>       snapshot_;
    std::uint64_t                          version_ = 0;
    std::chrono::steady_clock::time_point  next_poll_;
    std::chrono::steady_clock::time_point  last_emit_{std::chrono::steady_clock::now()};
    std::string                            last_error_;
    std::optional<std::string>             remote_alias_;
};

} // namespace

InspectorHttpServer::InspectorHttpServer(PathSpace& space)
    : InspectorHttpServer(space, Options{}) {}

InspectorHttpServer::InspectorHttpServer(PathSpace& space, Options options)
    : space_(space)
    , options_(std::move(options))
    , stream_metrics_(space_)
    , search_metrics_(space_)
    , usage_metrics_(space_)
    , remote_mounts_(options_.remote_mounts, &space_)
    , acl_(space_, options_.acl) {
    if (options_.write_toggles.enabled) {
        if (options_.write_toggles.allowed_roles.empty()) {
            options_.write_toggles.allowed_roles.emplace_back("root");
        }
        if (options_.write_toggles.audit_root.empty()) {
            options_.write_toggles.audit_root = "/diagnostics/web/inspector/audit_log";
        }
        options_.write_toggles.audit_root = NormalizeInspectorPath(options_.write_toggles.audit_root);
        if (options_.write_toggles.confirmation_header.empty()) {
            options_.write_toggles.confirmation_header = "x-pathspace-inspector-write-confirmed";
        }
        if (options_.write_toggles.confirmation_token.empty()) {
            options_.write_toggles.confirmation_token = "true";
        }
        for (auto& action : options_.write_toggles.actions) {
            action.path = NormalizeInspectorPath(action.path);
        }
    }
}

InspectorHttpServer::~InspectorHttpServer() {
    this->stop();
    this->join();
}

auto InspectorHttpServer::start() -> Expected<void> {
    std::unique_lock lock(mutex_);
    if (server_) {
        return std::unexpected(
            Error{Error::Code::InvalidError, "Inspector server already running"});
    }

    server_ = std::make_unique<httplib::Server>();
    this->configure_routes(*server_);

    auto       requested_port = options_.port;
    if (requested_port < 0) {
        requested_port = 0;
    }

    int bound_port = requested_port;
    if (requested_port == 0) {
        bound_port = server_->bind_to_any_port(options_.host);
        if (bound_port < 0) {
            server_.reset();
            return std::unexpected(
                Error{Error::Code::UnknownError, "Failed to bind inspector HTTP server"});
        }
    } else {
        if (!server_->bind_to_port(options_.host, requested_port)) {
            server_.reset();
            return std::unexpected(
                Error{Error::Code::UnknownError, "Failed to bind inspector HTTP server"});
        }
    }

    bound_port_ = static_cast<std::uint16_t>(bound_port);
    running_.store(true);

    server_thread_ = std::thread([this]() {
        if (server_) {
            server_->listen_after_bind();
        }
        running_.store(false);
    });

    lock.unlock();
    server_->wait_until_ready();
    lock.lock();
    if (!server_ || !server_->is_running()) {
        server_->stop();
        lock.unlock();
        this->join();
        lock.lock();
        server_.reset();
        bound_port_ = 0;
        running_.store(false);
        return std::unexpected(
            Error{Error::Code::UnknownError, "Inspector server failed to start listening"});
    }

    if (remote_mounts_.hasMounts()) {
        remote_mounts_.start();
    }

    return {};
}

auto InspectorHttpServer::stop() -> void {
    std::unique_lock lock(mutex_);
    if (!server_) {
        return;
    }
    server_->stop();
    lock.unlock();
    this->join();
    lock.lock();
    server_.reset();
    bound_port_ = 0;
    running_.store(false);
    remote_mounts_.stop();
}

auto InspectorHttpServer::join() -> void {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

auto InspectorHttpServer::is_running() const -> bool {
    return running_.load();
}

auto InspectorHttpServer::port() const -> std::uint16_t {
    return bound_port_;
}

auto InspectorHttpServer::configure_routes(httplib::Server& server) -> void {
    if (options_.enable_ui) {
        server.Get("/", [this](httplib::Request const&, httplib::Response& res) {
            this->handle_ui_request(res, "index.html");
        });
        server.Get("/index.html", [this](httplib::Request const&, httplib::Response& res) {
            this->handle_ui_request(res, "index.html");
        });
    }

    auto build_snapshot = [this](InspectorSnapshotOptions opts) -> Expected<InspectorSnapshot> {
        opts.root = NormalizeInspectorPath(opts.root);
        RemoteWaiterGuard remote_guard(remote_mounts_.hasMounts() ? &remote_mounts_ : nullptr,
                                       opts.root);
        if (auto remote = remote_mounts_.buildRemoteSnapshot(opts)) {
            return *remote;
        }
        auto snapshot = BuildInspectorSnapshot(space_, opts);
        if (!snapshot) {
            return snapshot;
        }
        remote_mounts_.augmentLocalSnapshot(*snapshot);
        return snapshot;
    };

    if (options_.enable_test_controls) {
        server.Post("/inspector/test/mutate", [this](httplib::Request const& req, httplib::Response& res) {
            if (req.body.empty()) {
                auto [status, payload] = make_error("missing test mutation payload", 400);
                res.status             = status;
                res.set_content(payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return;
            }

            auto payload = nlohmann::json::parse(req.body, nullptr, false);
            if (payload.is_discarded()) {
                auto [status, json_payload] = make_error("invalid JSON payload", 400);
                res.status                  = status;
                res.set_content(json_payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return;
            }

            bool clear_requested = false;
            if (auto clear_it = payload.find("clear"); clear_it != payload.end() && clear_it->is_boolean()) {
                clear_requested = clear_it->get<bool>();
            }
            auto applied         = apply_test_mutations(space_, payload);
            if (!applied) {
                auto const& error = applied.error();
                int         code  = (error.code == Error::Code::MalformedInput
                                     || error.code == Error::Code::InvalidPath
                                     || error.code == Error::Code::InvalidPermissions)
                                      ? 400
                                      : 500;
                auto [status, json_payload] = make_error(describeError(error), code);
                res.status                  = status;
                res.set_content(json_payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return;
            }

            nlohmann::json response{{"status", "ok"},
                                    {"set", *applied},
                                    {"cleared", clear_requested}};
            res.status = 200;
            res.set_content(response.dump(2), "application/json");
            res.set_header("Cache-Control", "no-store");
        });
    }

    if (options_.write_toggles.enabled) {
        auto authorize_write_request = [this](httplib::Request const& req,
                                             httplib::Response& res,
                                             bool require_confirmation)
            -> std::optional<WriteToggleRequestContext> {
            if (!options_.write_toggles.enabled) {
                auto [status, payload] = make_error("write toggles disabled", 404);
                res.status             = status;
                res.set_content(payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return std::nullopt;
            }
            auto role = extract_role(req);
            if (!is_role_allowed(options_.write_toggles.allowed_roles, role)) {
                auto [status, payload] =
                    make_error("admin role required for inspector write toggles", 403);
                res.status = status;
                res.set_content(payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return std::nullopt;
            }
            if (require_confirmation
                && !write_confirmation_present(req, options_.write_toggles)) {
                auto [status, payload] = make_error(
                    "write confirmation header required", 428);
                res.status = status;
                res.set_content(payload, "application/json");
                res.set_header("Cache-Control", "no-store");
                return std::nullopt;
            }
            WriteToggleRequestContext context;
            context.role   = role;
            context.user   = extract_user(req);
            context.client = req.remote_addr;
            return context;
        };

        server.Get("/inspector/actions/toggles",
                   [this, authorize_write_request](httplib::Request const& req,
                                                   httplib::Response& res) {
                       auto context = authorize_write_request(req, res, false);
                       if (!context) {
                           return;
                       }
                       nlohmann::json response{{"enabled", true},
                                               {"allowed_roles",
                                                options_.write_toggles.allowed_roles},
                                               {"requires_confirmation",
                                                !options_.write_toggles.confirmation_header.empty()},
                                               {"confirmation_header",
                                                options_.write_toggles.confirmation_header},
                                               {"confirmation_token",
                                                options_.write_toggles.confirmation_token},
                                               {"actions", nlohmann::json::array()}};
                       for (auto const& action : options_.write_toggles.actions) {
                           auto state = read_bool_state(space_, action.path, action.default_state);
                           if (!state) {
                               auto [status, payload] = make_error(
                                   describeError(state.error()), 500);
                               res.status = status;
                               res.set_content(payload, "application/json");
                               res.set_header("Cache-Control", "no-store");
                               return;
                           }
                           nlohmann::json action_json{{"id", action.id},
                                                      {"label", action.label},
                                                      {"description", action.description},
                                                      {"kind", inspector_write_kind_string(action.kind)},
                                                      {"path", action.path},
                                                      {"current_state", state->value},
                                                      {"default_state", action.default_state}};
                           if (action.kind == InspectorWriteToggleKind::SetBool) {
                               action_json["target_state"] = action.default_state;
                           }
                           response["actions"].push_back(std::move(action_json));
                       }
                       res.status = 200;
                       res.set_content(response.dump(2), "application/json");
                       res.set_header("Cache-Control", "no-store");
                   });

        server.Post("/inspector/actions/toggles",
                    [this, authorize_write_request](httplib::Request const& req,
                                                    httplib::Response& res) {
                        auto context = authorize_write_request(req, res, true);
                        if (!context) {
                            return;
                        }
                        if (req.body.empty()) {
                            auto [status, payload] = make_error("missing toggle payload", 400);
                            res.status             = status;
                            res.set_content(payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        auto payload = nlohmann::json::parse(req.body, nullptr, false);
                        if (payload.is_discarded() || !payload.is_object()) {
                            auto [status, json_payload] = make_error("invalid JSON payload", 400);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        auto id_it = payload.find("id");
                        if (id_it == payload.end() || !id_it->is_string()) {
                            auto [status, json_payload] = make_error("toggle id is required", 400);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        auto id = trim_copy(id_it->get<std::string>());
                        if (id.empty()) {
                            auto [status, json_payload] = make_error("toggle id is invalid", 400);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        auto* action = find_write_toggle_action(options_.write_toggles.actions, id);
                        if (!action) {
                            auto [status, json_payload] = make_error("toggle not found", 404);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        std::string operation;
                        if (auto op_it = payload.find("operation"); op_it != payload.end()
                            && op_it->is_string()) {
                            operation = lowercase_copy(op_it->get<std::string>());
                        }
                        if (operation.empty()) {
                            operation = action->kind == InspectorWriteToggleKind::ToggleBool ? "toggle"
                                                                                            : "set";
                        }
                        bool operation_allowed = false;
                        if (action->kind == InspectorWriteToggleKind::ToggleBool) {
                            operation_allowed = (operation == "toggle");
                        } else {
                            operation_allowed = (operation == "set" || operation == "apply"
                                                 || operation == "reset");
                        }
                        if (!operation_allowed) {
                            auto [status, json_payload] = make_error("unsupported toggle operation", 400);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        std::string note;
                        if (auto note_it = payload.find("note"); note_it != payload.end()
                            && note_it->is_string()) {
                            note = trim_copy(note_it->get<std::string>());
                        }

                        auto apply_result = apply_write_toggle_action(space_, *action);
                        WriteToggleAuditEvent audit_event;
                        audit_event.action_id    = action->id;
                        audit_event.action_label = action->label;
                        audit_event.path         = action->path;
                        audit_event.kind         = std::string(inspector_write_kind_string(action->kind));
                        audit_event.role         = context->role;
                        audit_event.user         = context->user;
                        audit_event.client       = context->client;
                        audit_event.note         = note;
                        audit_event.timestamp_ms = now_ms();

                        if (!apply_result) {
                            audit_event.success = false;
                            audit_event.message = describeError(apply_result.error());
                            record_write_audit_event(space_, options_.write_toggles, audit_event);
                            auto [status, json_payload] = make_error(audit_event.message, 500);
                            res.status                  = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }

                        audit_event.success        = true;
                        audit_event.previous_value = apply_result->previous;
                        audit_event.new_value      = apply_result->current;
                        audit_event.message        = (action->kind == InspectorWriteToggleKind::ToggleBool)
                                                         ? "toggle"
                                                         : "set";
                        record_write_audit_event(space_, options_.write_toggles, audit_event);

                        nlohmann::json response{{"status", "updated"},
                                                {"action_id", action->id},
                                                {"kind", inspector_write_kind_string(action->kind)},
                                                {"previous_state", apply_result->previous},
                                                {"current_state", apply_result->current}};
                        res.status = 200;
                        res.set_content(response.dump(2), "application/json");
                        res.set_header("Cache-Control", "no-store");
                    });
    }

    server.Get("/inspector/tree", [this, build_snapshot](httplib::Request const& req, httplib::Response& res) {
        auto options = options_.snapshot;
        if (auto root = req.get_param_value("root"); !root.empty()) {
            options.root = root;
        }
        if (auto depth = req.get_param_value("depth"); !depth.empty()) {
            options.max_depth = parse_unsigned(depth, options.max_depth);
        }
        if (auto max_children = req.get_param_value("max_children"); !max_children.empty()) {
            options.max_children = parse_unsigned(max_children, options.max_children);
        }
        if (auto include_values = req.get_param_value("include_values"); !include_values.empty()) {
            options.include_values = parse_bool(include_values, options.include_values);
        }

        options.root = NormalizeInspectorPath(options.root);
        if (enforce_acl(req, res, options.root, "/inspector/tree")) {
            return;
        }

        auto snapshot = build_snapshot(options);
        if (!snapshot) {
            auto [status, payload] = make_error(
                describeError(snapshot.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializeInspectorSnapshot(*snapshot), "application/json");
    });

    server.Get("/inspector/node", [this, build_snapshot](httplib::Request const& req, httplib::Response& res) {
        auto path = req.get_param_value("path");
        if (path.empty()) {
            auto [status, payload] = make_error("path parameter required", 400);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        InspectorSnapshotOptions options = options_.snapshot;
        options.root                     = NormalizeInspectorPath(path);
        options.max_depth                = parse_unsigned(req.get_param_value("depth"), 0);
        options.max_children             = parse_unsigned(req.get_param_value("max_children"), options.max_children);
        if (auto include_values = req.get_param_value("include_values"); !include_values.empty()) {
            options.include_values = parse_bool(include_values, options.include_values);
        }

        if (enforce_acl(req, res, options.root, "/inspector/node")) {
            return;
        }

        auto snapshot = build_snapshot(options);
        if (!snapshot) {
            auto [status, payload] = make_error(
                describeError(snapshot.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializeInspectorSnapshot(*snapshot), "application/json");
    });

    server.Get("/inspector/remotes", [this](httplib::Request const&, httplib::Response& res) {
        nlohmann::json mounts = nlohmann::json::array();
        if (remote_mounts_.hasMounts()) {
            for (auto const& status : remote_mounts_.statuses()) {
                auto health = status.health.empty()
                                   ? (status.connected ? std::string{"connected"}
                                                      : std::string{"unavailable"})
                                   : status.health;
                nlohmann::json entry{{"alias", status.alias},
                                     {"path", status.path},
                                     {"connected", status.connected},
                                     {"message", status.message},
                                     {"access_hint", status.access_hint},
                                     {"health", std::move(health)},
                                     {"latency",
                                      {{"last_ms", status.last_latency.count()},
                                       {"average_ms", status.average_latency.count()},
                                       {"max_ms", status.max_latency.count()}}},
                                     {"requests",
                                      {{"success_total", status.success_count},
                                       {"error_total", status.error_count},
                                       {"consecutive_errors", status.consecutive_errors}}},
                                     {"waiters",
                                      {{"current", status.waiter_depth},
                                       {"max", status.max_waiter_depth}}}};
                auto updated = to_millis_since_epoch(status.last_update);
                if (updated > 0) {
                    entry["last_update_ms"] = updated;
                }
                auto last_error_ms = to_millis_since_epoch(status.last_error_time);
                if (last_error_ms > 0) {
                    entry["last_error_ms"] = last_error_ms;
                }
                mounts.push_back(std::move(entry));
            }
        }

        nlohmann::json payload{{"default_root", options_.snapshot.root},
                               {"remote_root", "/remote"},
                               {"defaults",
                                {{"max_depth", options_.snapshot.max_depth},
                                 {"max_children", options_.snapshot.max_children},
                                 {"include_values", options_.snapshot.include_values}}},
                               {"mounts", std::move(mounts)}};
        res.status = 200;
        res.set_content(payload.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Get("/inspector/stream", [this](httplib::Request const& req, httplib::Response& res) {
        auto options = options_.snapshot;
        if (auto root = req.get_param_value("root"); !root.empty()) {
            options.root = root;
        }
        if (auto depth = req.get_param_value("depth"); !depth.empty()) {
            options.max_depth = parse_unsigned(depth, options.max_depth);
        }
        if (auto max_children = req.get_param_value("max_children"); !max_children.empty()) {
            options.max_children = parse_unsigned(max_children, options.max_children);
        }
        if (auto include_values = req.get_param_value("include_values"); !include_values.empty()) {
            options.include_values = parse_bool(include_values, options.include_values);
        }

        options.root = NormalizeInspectorPath(options.root);
        if (enforce_acl(req, res, options.root, "/inspector/stream")) {
            return;
        }

        auto poll_ms = parse_unsigned(req.get_param_value("poll_ms"),
                                      static_cast<std::size_t>(options_.stream.poll_interval.count()));
        auto keepalive_ms = parse_unsigned(req.get_param_value("keepalive_ms"),
                                           static_cast<std::size_t>(options_.stream.keepalive_interval.count()));

        auto poll_interval      = clamp_interval(poll_ms, options_.stream.poll_interval, std::chrono::milliseconds(100));
        auto keepalive_interval = clamp_interval(keepalive_ms, options_.stream.keepalive_interval, std::chrono::milliseconds(1000));
        auto idle_timeout       = options_.stream.idle_timeout;
        auto max_pending_events = options_.stream.max_pending_events == 0 ? std::size_t{1} : options_.stream.max_pending_events;
        auto max_events_per_tick = options_.stream.max_events_per_tick == 0 ? std::size_t{1} : options_.stream.max_events_per_tick;

        RemoteMountManager* remote_ptr = remote_mounts_.hasMounts() ? &remote_mounts_ : nullptr;
        auto session = std::make_shared<StreamSession>(space_,
                                                       options,
                                                       poll_interval,
                                                       keepalive_interval,
                                                       idle_timeout,
                                                       max_pending_events,
                                                       max_events_per_tick,
                                                       stream_metrics_,
                                                       remote_ptr);

        res.set_header("Cache-Control", "no-store");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream",
            [session](size_t, httplib::DataSink& sink) {
                return session->pump(sink);
            },
            [session](bool done) {
                if (!done) {
                    session->cancel(StreamDisconnectReason::Client);
                }
                session->finalize(done ? StreamDisconnectReason::Server : StreamDisconnectReason::Client);
            });
    });

    server.Get("/inspector/metrics/stream", [this](httplib::Request const&, httplib::Response& res) {
        auto snapshot = stream_metrics_.snapshot();
        nlohmann::json json{
            {"active_sessions", snapshot.active_sessions},
            {"total_sessions", snapshot.total_sessions},
            {"queue_depth", snapshot.queue_depth},
            {"max_queue_depth", snapshot.max_queue_depth},
            {"dropped", snapshot.dropped_events},
            {"resent", snapshot.resent_snapshots},
            {"disconnect",
             {
                 {"client", snapshot.disconnect_client},
                 {"server", snapshot.disconnect_server},
                 {"backpressure", snapshot.disconnect_backpressure},
                 {"timeout", snapshot.disconnect_timeout},
             }},
            {"limits",
             {
                 {"max_pending_events", options_.stream.max_pending_events},
                 {"max_events_per_tick", options_.stream.max_events_per_tick},
                 {"idle_timeout_ms", options_.stream.idle_timeout.count()},
             }},
        };
        res.status = 200;
        res.set_content(json.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Post("/inspector/metrics/search", [this](httplib::Request const& req, httplib::Response& res) {
        if (req.body.empty()) {
            auto [status, payload] = make_error("missing search metrics payload", 400);
            res.status             = status;
            res.set_content(payload, "application/json");
            return;
        }

        auto payload = nlohmann::json::parse(req.body, nullptr, false);
        if (payload.is_discarded()) {
            auto [status, json_payload] = make_error("invalid JSON payload", 400);
            res.status                 = status;
            res.set_content(json_payload, "application/json");
            return;
        }

        bool recorded = false;
        if (auto query = payload.find("query"); query != payload.end() && query->is_object()) {
            SearchQueryEvent event;
            event.latency_ms     = read_uint64(*query, "latency_ms");
            event.match_count    = read_uint64(*query, "match_count");
            event.returned_count = read_uint64(*query, "returned_count");
            search_metrics_.record_query(event);
            recorded = true;
        }

        if (auto watch = payload.find("watch"); watch != payload.end() && watch->is_object()) {
            SearchWatchlistEvent event;
            event.live         = read_uint64(*watch, "live");
            event.missing      = read_uint64(*watch, "missing");
            event.truncated    = read_uint64(*watch, "truncated");
            event.out_of_scope = read_uint64(*watch, "out_of_scope");
            event.unknown      = read_uint64(*watch, "unknown");
            search_metrics_.record_watchlist(event);
            recorded = true;
        }

        if (!recorded) {
            auto [status, json_payload] =
                make_error("search metrics payload is missing query/watch", 400);
            res.status = status;
            res.set_content(json_payload, "application/json");
            return;
        }

        nlohmann::json ack{{"status", "recorded"}};
        res.status = 202;
        res.set_content(ack.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Get("/inspector/metrics/search", [this](httplib::Request const&, httplib::Response& res) {
        auto snapshot = search_metrics_.snapshot();
        nlohmann::json json{
            {"queries",
             {
                 {"total", snapshot.queries.total_queries},
                 {"truncated_queries", snapshot.queries.truncated_queries},
                 {"truncated_results_total", snapshot.queries.truncated_results_total},
                 {"last_latency_ms", snapshot.queries.last_latency_ms},
                 {"average_latency_ms", snapshot.queries.average_latency_ms},
                 {"last_match_count", snapshot.queries.last_match_count},
                 {"last_returned_count", snapshot.queries.last_returned_count},
                 {"last_truncated_count", snapshot.queries.last_truncated_count},
                 {"last_updated_ms", snapshot.queries.last_updated_ms},
             }},
            {"watch",
             {
                 {"live", snapshot.watch.live},
                 {"missing", snapshot.watch.missing},
                 {"truncated", snapshot.watch.truncated},
                 {"out_of_scope", snapshot.watch.out_of_scope},
                 {"unknown", snapshot.watch.unknown},
                 {"total", snapshot.watch.total},
                 {"last_updated_ms", snapshot.watch.last_updated_ms},
             }},
        };
        res.status = 200;
        res.set_content(json.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Post("/inspector/metrics/usage", [this](httplib::Request const& req,
                                                    httplib::Response& res) {
        if (req.body.empty()) {
            auto [status, payload] = make_error("missing usage metrics payload", 400);
            res.status             = status;
            res.set_content(payload, "application/json");
            return;
        }

        auto payload = nlohmann::json::parse(req.body, nullptr, false);
        if (payload.is_discarded()) {
            auto [status, json_payload] = make_error("invalid JSON payload", 400);
            res.status                 = status;
            res.set_content(json_payload, "application/json");
            return;
        }

        auto panels_it = payload.find("panels");
        if (panels_it == payload.end() || !panels_it->is_array()) {
            auto [status, json_payload] =
                make_error("usage metrics payload requires panels[]", 400);
            res.status = status;
            res.set_content(json_payload, "application/json");
            return;
        }

        auto const default_timestamp = read_uint64(payload, "timestamp_ms");

        std::vector<PanelUsageEvent> events;
        events.reserve(panels_it->size());

        for (auto const& entry : *panels_it) {
            if (!entry.is_object()) {
                continue;
            }
            auto id_it = entry.find("id");
            if (id_it == entry.end() || !id_it->is_string()) {
                continue;
            }
            auto sanitized = sanitize_panel_identifier(id_it->get<std::string>());
            if (sanitized.empty()) {
                continue;
            }

            PanelUsageEvent event{};
            event.panel_id    = std::move(sanitized);
            event.dwell_ms    = read_uint64(entry, "dwell_ms");
            event.entries     = read_uint64(entry, "entries");
            event.timestamp_ms = read_uint64(entry, "timestamp_ms");
            if (event.timestamp_ms == 0) {
                event.timestamp_ms = default_timestamp;
            }
            if (event.timestamp_ms == 0) {
                event.timestamp_ms = now_ms();
            }
            if (event.dwell_ms == 0 && event.entries == 0) {
                continue;
            }
            events.emplace_back(std::move(event));
        }

        if (events.empty()) {
            auto [status, json_payload] = make_error("no valid usage metrics entries", 400);
            res.status                 = status;
            res.set_content(json_payload, "application/json");
            return;
        }

        usage_metrics_.record(events);

        nlohmann::json ack{{"status", "recorded"}, {"panels", events.size()}};
        res.status = 202;
        res.set_content(ack.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Get("/inspector/metrics/usage", [this](httplib::Request const&, httplib::Response& res) {
        auto snapshot = usage_metrics_.snapshot();
        nlohmann::json panels = nlohmann::json::array();
        auto& panel_array = panels.get_ref<nlohmann::json::array_t&>();
        panel_array.reserve(snapshot.panels.size());
        for (auto const& [id, metrics] : snapshot.panels) {
            panel_array.push_back({{"id", id},
                                   {"dwell_ms", metrics.dwell_ms_total},
                                   {"entries", metrics.entries_total},
                                   {"last_dwell_ms", metrics.last_dwell_ms},
                                   {"last_updated_ms", metrics.last_updated_ms}});
        }
        nlohmann::json json{{"total",
                             {
                                 {"dwell_ms", snapshot.total_dwell_ms},
                                 {"entries", snapshot.total_entries},
                                 {"last_updated_ms", snapshot.last_updated_ms},
                             }},
                            {"panels", std::move(panels)}};
        res.status = 200;
        res.set_content(json.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    auto make_watchlist_context = [this](httplib::Request const& req) -> WatchlistContext {
        WatchlistContext context;
        context.display_user = trim_copy(extract_user(req));
        if (context.display_user.empty()) {
            context.display_user = "anonymous";
        }
        context.user_id   = sanitize_user_identifier(context.display_user);
        context.root      = watchlist_root_for_user(context.user_id);
        context.trash_root = watchlist_trash_root_for_user(context.user_id);
        return context;
    };

    server.Get("/inspector/watchlists", [this, make_watchlist_context](httplib::Request const& req,
                                                                      httplib::Response& res) {
        auto context    = make_watchlist_context(req);
        auto watchlists = list_watchlists(space_, context.root);
        nlohmann::json response{{"user", context.display_user},
                                {"user_id", context.user_id},
                                {"count", watchlists.size()},
                                {"limits",
                                 {{"max_watchlists", options_.watchlists.max_saved_sets},
                                  {"max_paths_per_watchlist", options_.watchlists.max_paths_per_set}}},
                                {"watchlists", nlohmann::json::array()}};
        for (auto const& record : watchlists) {
            response["watchlists"].push_back(make_watchlist_json(record));
        }
        res.status = 200;
        res.set_content(response.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    auto make_snapshot_context = [this](httplib::Request const& req) -> SnapshotContext {
        SnapshotContext context;
        context.display_user = trim_copy(extract_user(req));
        if (context.display_user.empty()) {
            context.display_user = "anonymous";
        }
        context.user_id   = sanitize_user_identifier(context.display_user);
        context.root       = snapshot_root_for_user(context.user_id);
        context.trash_root = snapshot_trash_root_for_user(context.user_id);
        return context;
    };

    server.Get("/inspector/watchlists/export",
               [this, make_watchlist_context](httplib::Request const& req, httplib::Response& res) {
                   auto context    = make_watchlist_context(req);
                   auto watchlists = list_watchlists(space_, context.root);
                   nlohmann::json response{{"user", context.display_user},
                                           {"user_id", context.user_id},
                                           {"exported_ms", now_ms()},
                                           {"watchlists", nlohmann::json::array()}};
                   for (auto const& record : watchlists) {
                       response["watchlists"].push_back(make_watchlist_json(record));
                   }
                   res.status = 200;
                   res.set_content(response.dump(2), "application/json");
                   res.set_header("Cache-Control", "no-store");
               });

    server.Post("/inspector/watchlists",
                [this, make_watchlist_context](httplib::Request const& req, httplib::Response& res) {
                    if (req.body.empty()) {
                        auto [status, payload] = make_error("missing watchlist payload", 400);
                        res.status             = status;
                        res.set_content(payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto payload = nlohmann::json::parse(req.body, nullptr, false);
                    if (payload.is_discarded()) {
                        auto [status, json_payload] = make_error("invalid JSON payload", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto context = make_watchlist_context(req);
                    auto existing_records = list_watchlists(space_, context.root);
                    std::unordered_map<std::string, WatchlistRecord> existing_map;
                    existing_map.reserve(existing_records.size());
                    std::unordered_set<std::string> existing_ids;
                    existing_ids.reserve(existing_records.size());
                    for (auto const& record : existing_records) {
                        existing_map.emplace(record.id, record);
                        existing_ids.insert(record.id);
                    }
                    std::size_t current_total = existing_records.size();

                    std::string parse_error;
                    auto        parsed = parse_watchlist_input(payload,
                                                               options_.watchlists.max_paths_per_set,
                                                               parse_error);
                    if (!parsed) {
                        auto [status, json_payload] = make_error(parse_error, 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    bool overwrite = false;
                    if (auto it = payload.find("overwrite"); it != payload.end()) {
                        if (it->is_boolean()) {
                            overwrite = it->get<bool>();
                        } else if (it->is_number_integer()) {
                            overwrite = it->get<std::int64_t>() != 0;
                        } else if (it->is_string()) {
                            auto lowered = lowercase_copy(it->get<std::string>());
                            overwrite    = (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on");
                        }
                    }

                    auto base_id = parsed->id_provided && !parsed->requested_id.empty()
                                       ? sanitize_watchlist_identifier(parsed->requested_id)
                                       : sanitize_watchlist_identifier(parsed->name);
                    if (base_id.empty()) {
                        base_id = make_unique_watchlist_id("watchlist", existing_ids, {});
                    }

                    auto candidate_id = base_id;
                    bool target_exists = existing_map.contains(candidate_id);
                    if (target_exists && !overwrite) {
                        if (parsed->id_provided) {
                            auto [status, payload_json] = make_error("watchlist id already exists", 409);
                            res.status                  = status;
                            res.set_content(payload_json, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                        candidate_id = make_unique_watchlist_id(candidate_id, existing_ids, {});
                        target_exists = existing_map.contains(candidate_id);
                    }

                    if (!target_exists && current_total >= options_.watchlists.max_saved_sets) {
                        auto [status, payload_json] = make_error("watchlist limit reached", 400);
                        res.status                  = status;
                        res.set_content(payload_json, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    if (!target_exists) {
                        ++current_total;
                    }

                    auto timestamp = now_ms();

                    WatchlistRecord record;
                    record.id        = candidate_id;
                    record.name      = parsed->name;
                    record.paths     = parsed->paths;
                    record.created_ms = target_exists ? existing_map[candidate_id].created_ms : timestamp;
                    record.updated_ms = timestamp;

                    auto persisted =
                        persist_watchlist(space_, build_watchlist_path(context.root, candidate_id), record);
                    if (!persisted) {
                        auto [status, payload_json] = make_error(describeError(persisted.error()), 500);
                        res.status                  = status;
                        res.set_content(payload_json, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    nlohmann::json response{{"status", target_exists ? "updated" : "created"},
                                            {"user", context.display_user},
                                            {"user_id", context.user_id},
                                            {"watchlist", make_watchlist_json(record)}};
                    res.status = target_exists ? 200 : 201;
                    res.set_content(response.dump(2), "application/json");
                    res.set_header("Cache-Control", "no-store");
                });

    server.Delete("/inspector/watchlists",
                  [this, make_watchlist_context](httplib::Request const& req, httplib::Response& res) {
                      auto context = make_watchlist_context(req);
                      auto id      = req.get_param_value("id");
                      if (id.empty()) {
                          auto [status, payload] = make_error("watchlist id is required", 400);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }
                      auto sanitized_id = sanitize_watchlist_identifier(id);
                      if (sanitized_id.empty()) {
                          auto [status, payload] = make_error("watchlist id is invalid", 400);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }

                      auto ensure_trash = ensure_placeholder(space_, context.trash_root);
                      if (!ensure_trash) {
                          auto [status, payload_json] = make_error(describeError(ensure_trash.error()), 500);
                          res.status                  = status;
                          res.set_content(payload_json, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }

                      auto removed = remove_watchlist(space_, context.root, context.trash_root, sanitized_id);
                      if (!removed) {
                          auto [status, payload_json] = make_error(describeError(removed.error()), 500);
                          res.status                  = status;
                          res.set_content(payload_json, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }
                      if (!*removed) {
                          auto [status, payload_json] = make_error("watchlist not found", 404);
                          res.status                  = status;
                          res.set_content(payload_json, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }

                      res.status = 204;
                      res.set_header("Cache-Control", "no-store");
                  });

    server.Post("/inspector/watchlists/import",
                [this, make_watchlist_context](httplib::Request const& req, httplib::Response& res) {
                    if (req.body.empty()) {
                        auto [status, payload] = make_error("missing watchlist payload", 400);
                        res.status             = status;
                        res.set_content(payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto payload = nlohmann::json::parse(req.body, nullptr, false);
                    if (payload.is_discarded()) {
                        auto [status, json_payload] = make_error("invalid JSON payload", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto watchlists_json = payload.find("watchlists");
                    if (watchlists_json == payload.end() || !watchlists_json->is_array()) {
                        auto [status, json_payload] = make_error("watchlists array is required", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto mode  = payload.value("mode", std::string{"merge"});
                    auto lower = lowercase_copy(mode);
                    bool replace = (lower == "replace");

                    auto context = make_watchlist_context(req);
                    auto existing_records = list_watchlists(space_, context.root);
                    std::unordered_map<std::string, WatchlistRecord> existing_map;
                    std::unordered_set<std::string> existing_ids;
                    existing_map.reserve(existing_records.size());
                    existing_ids.reserve(existing_records.size());
                    for (auto const& record : existing_records) {
                        existing_map.emplace(record.id, record);
                        existing_ids.insert(record.id);
                    }

                    std::size_t removed = 0;
                    if (replace && !existing_records.empty()) {
                        auto ensure_trash = ensure_placeholder(space_, context.trash_root);
                        if (!ensure_trash) {
                            auto [status, payload_json] = make_error(describeError(ensure_trash.error()), 500);
                            res.status                  = status;
                            res.set_content(payload_json, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }

                        for (auto const& record : existing_records) {
                            auto removed_result =
                                remove_watchlist(space_, context.root, context.trash_root, record.id);
                            if (!removed_result) {
                                auto [status, payload_json] = make_error(describeError(removed_result.error()), 500);
                                res.status                  = status;
                                res.set_content(payload_json, "application/json");
                                res.set_header("Cache-Control", "no-store");
                                return;
                            }
                            if (*removed_result) {
                                ++removed;
                            }
                        }
                        existing_map.clear();
                        existing_ids.clear();
                    }

                    std::vector<WatchlistRecord> staged;
                    staged.reserve(watchlists_json->size());
                    std::unordered_set<std::string> import_ids;
                    import_ids.reserve(watchlists_json->size());

                    std::size_t new_count = 0;
                    auto        timestamp = now_ms();

                    for (auto const& entry : *watchlists_json) {
                        std::string parse_error;
                        auto        parsed = parse_watchlist_input(entry,
                                                                   options_.watchlists.max_paths_per_set,
                                                                   parse_error);
                        if (!parsed) {
                            auto [status, json_payload] = make_error(parse_error, 400);
                            res.status                 = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }

                        auto base_id = parsed->id_provided && !parsed->requested_id.empty()
                                           ? sanitize_watchlist_identifier(parsed->requested_id)
                                           : sanitize_watchlist_identifier(parsed->name);
                        if (base_id.empty()) {
                            base_id = "watchlist";
                        }

                        auto candidate_id = base_id;
                        bool id_provided  = parsed->id_provided && !parsed->requested_id.empty();
                        bool target_exists = existing_map.contains(candidate_id);

                        if (target_exists && !id_provided) {
                            candidate_id = make_unique_watchlist_id(candidate_id, existing_ids, import_ids);
                            target_exists = existing_map.contains(candidate_id);
                        }

                        if (import_ids.contains(candidate_id)) {
                            if (id_provided) {
                                auto [status, payload_json] =
                                    make_error("duplicate watchlist id in import payload", 409);
                                res.status = status;
                                res.set_content(payload_json, "application/json");
                                res.set_header("Cache-Control", "no-store");
                                return;
                            }
                            candidate_id = make_unique_watchlist_id(candidate_id, existing_ids, import_ids);
                        }

                        if (!target_exists) {
                            ++new_count;
                            if (!replace && existing_map.size() + new_count > options_.watchlists.max_saved_sets) {
                                auto [status, payload_json] = make_error("watchlist limit reached", 400);
                                res.status                  = status;
                                res.set_content(payload_json, "application/json");
                                res.set_header("Cache-Control", "no-store");
                                return;
                            }
                        }

                        import_ids.insert(candidate_id);
                        existing_ids.insert(candidate_id);

                        WatchlistRecord record;
                        record.id        = candidate_id;
                        record.name      = parsed->name;
                        record.paths     = parsed->paths;
                        record.created_ms = target_exists ? existing_map[candidate_id].created_ms : timestamp;
                        record.updated_ms = timestamp;
                        staged.push_back(std::move(record));
                    }

                    for (auto const& record : staged) {
                        auto persisted =
                            persist_watchlist(space_, build_watchlist_path(context.root, record.id), record);
                        if (!persisted) {
                            auto [status, payload_json] = make_error(describeError(persisted.error()), 500);
                            res.status                  = status;
                            res.set_content(payload_json, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                    }

                    nlohmann::json response{{"status", "imported"},
                                            {"mode", replace ? "replace" : "merge"},
                                            {"deleted", replace ? removed : 0},
                                            {"imported", staged.size()},
                                            {"user", context.display_user},
                                            {"user_id", context.user_id},
                                            {"watchlists", nlohmann::json::array()}};
                    for (auto const& record : staged) {
                        response["watchlists"].push_back(make_watchlist_json(record));
                    }

                    res.status = 202;
                    res.set_content(response.dump(2), "application/json");
                    res.set_header("Cache-Control", "no-store");
                });

    auto parse_snapshot_options = [this](nlohmann::json const& payload) -> InspectorSnapshotOptions {
        auto apply_overrides = [](InspectorSnapshotOptions& target, nlohmann::json const& source) {
            if (!source.is_object()) {
                return;
            }
            if (auto it = source.find("root"); it != source.end() && it->is_string()) {
                target.root = NormalizeInspectorPath(it->get<std::string>());
            }
            if (auto it = source.find("max_depth"); it != source.end() && it->is_number_unsigned()) {
                target.max_depth = it->get<std::size_t>();
            }
            if (auto it = source.find("max_children"); it != source.end() && it->is_number_unsigned()) {
                target.max_children = it->get<std::size_t>();
            }
            if (auto it = source.find("include_values"); it != source.end() && it->is_boolean()) {
                target.include_values = it->get<bool>();
            }
        };

        InspectorSnapshotOptions options = options_.snapshot;
        apply_overrides(options, payload);
        if (auto it = payload.find("options"); it != payload.end() && it->is_object()) {
            apply_overrides(options, *it);
        }
        if (options.root.empty()) {
            options.root = options_.snapshot.root;
        }
        return options;
    };

    auto parse_overwrite_flag = [](nlohmann::json const& payload) -> bool {
        auto it = payload.find("overwrite");
        if (it == payload.end()) {
            return false;
        }
        if (it->is_boolean()) {
            return it->get<bool>();
        }
        if (it->is_number_integer()) {
            return it->get<std::int64_t>() != 0;
        }
        if (it->is_string()) {
            auto lowered = lowercase_copy(it->get<std::string>());
            return lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on";
        }
        return false;
    };

    server.Get("/inspector/snapshots", [this, make_snapshot_context](httplib::Request const& req,
                                                                      httplib::Response& res) {
        auto context  = make_snapshot_context(req);
        auto snapshots = list_snapshots(space_, context.root);
        nlohmann::json response{{"user", context.display_user},
                                {"user_id", context.user_id},
                                {"count", snapshots.size()},
                                {"limit", options_.snapshots.max_saved_snapshots},
                                {"max_snapshot_bytes", options_.snapshots.max_snapshot_bytes},
                                {"snapshots", nlohmann::json::array()}};
        for (auto const& record : snapshots) {
            response["snapshots"].push_back(make_snapshot_json(record));
        }
        res.status = 200;
        res.set_content(response.dump(2), "application/json");
        res.set_header("Cache-Control", "no-store");
    });

    server.Post("/inspector/snapshots",
                [this,
                 make_snapshot_context,
                 parse_snapshot_options,
                 parse_overwrite_flag,
                 build_snapshot](httplib::Request const& req, httplib::Response& res) {
                    if (req.body.empty()) {
                        auto [status, payload] = make_error("missing snapshot payload", 400);
                        res.status             = status;
                        res.set_content(payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto payload = nlohmann::json::parse(req.body, nullptr, false);
                    if (payload.is_discarded()) {
                        auto [status, json_payload] = make_error("invalid JSON payload", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto label_it = payload.find("label");
                    if (label_it == payload.end() || !label_it->is_string()) {
                        auto [status, json_payload] = make_error("snapshot label is required", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }
                    auto label = trim_copy(label_it->get<std::string>());
                    if (label.empty()) {
                        auto [status, json_payload] = make_error("snapshot label cannot be empty", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    SnapshotContext context = make_snapshot_context(req);
                    auto snapshot_options = parse_snapshot_options(payload);
                    if (this->enforce_acl(req, res, snapshot_options.root, "/inspector/snapshots")) {
                        return;
                    }

                    auto snapshot = build_snapshot(snapshot_options);
                    if (!snapshot) {
                        auto [status, json_payload] = make_error(describeError(snapshot.error()), 500);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    PathSpaceJsonOptions exporter_options;
                    exporter_options.visit.root          = snapshot_options.root;
                    exporter_options.visit.maxDepth      = snapshot_options.max_depth;
                    exporter_options.visit.maxChildren   = snapshot_options.max_children == 0
                                                               ? VisitOptions::kUnlimitedChildren
                                                               : snapshot_options.max_children;
                    exporter_options.visit.includeValues = snapshot_options.include_values;
                    auto export_payload = PathSpaceJsonExporter::Export(space_, exporter_options);
                    if (!export_payload) {
                        auto [status, json_payload] = make_error(describeError(export_payload.error()), 500);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    SnapshotRecord record;
                    record.label             = label;
                    record.note              = trim_copy(payload.value("note", std::string{}));
                    record.created_ms        = now_ms();
                    record.options           = snapshot_options;
                    record.diagnostics       = snapshot->diagnostics.size();
                    record.inspector_payload = SerializeInspectorSnapshot(*snapshot, 2);
                    record.export_payload    = *export_payload;
                    if (auto augmented = augment_snapshot_export(record.export_payload, snapshot_options)) {
                        record.export_payload = std::move(*augmented);
                    }
                    record.inspector_bytes   = record.inspector_payload.size();
                    record.export_bytes      = record.export_payload.size();

                    auto byte_limit = options_.snapshots.max_snapshot_bytes;
                    if (byte_limit > 0 &&
                        (record.inspector_bytes > byte_limit || record.export_bytes > byte_limit)) {
                        auto [status, json_payload] = make_error("snapshot exceeds configured byte limit", 413);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto existing_records = list_snapshots(space_, context.root);
                    std::unordered_set<std::string> existing_ids;
                    existing_ids.reserve(existing_records.size());
                    for (auto const& entry : existing_records) {
                        existing_ids.insert(entry.id);
                    }

                    std::string requested_id;
                    if (auto it = payload.find("id"); it != payload.end() && it->is_string()) {
                        requested_id = trim_copy(it->get<std::string>());
                    }
                    auto candidate_id = !requested_id.empty() ? sanitize_snapshot_identifier(requested_id)
                                                              : sanitize_snapshot_identifier(label);
                    if (candidate_id.empty()) {
                        candidate_id = make_unique_snapshot_id("snapshot", existing_ids);
                    }
                    if (candidate_id.size() > kMaxSnapshotIdLength) {
                        candidate_id.resize(kMaxSnapshotIdLength);
                    }

                    bool overwrite     = parse_overwrite_flag(payload);
                    bool target_exists = existing_ids.contains(candidate_id);
                    if (target_exists && !overwrite) {
                        candidate_id  = make_unique_snapshot_id(candidate_id, existing_ids);
                        target_exists = false;
                    }
                    record.id = candidate_id;

                    if (target_exists && overwrite) {
                        auto removed = delete_snapshot_record(space_, context, candidate_id);
                        if (!removed) {
                            auto [status, json_payload] = make_error(describeError(removed.error()), 500);
                            res.status                 = status;
                            res.set_content(json_payload, "application/json");
                            res.set_header("Cache-Control", "no-store");
                            return;
                        }
                    }

                    auto persisted = persist_snapshot_record(space_, context, record);
                    if (!persisted) {
                        auto [status, json_payload] = make_error(describeError(persisted.error()), 500);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto trimmed = trim_snapshots(space_, context, options_.snapshots.max_saved_snapshots);
                    if (!trimmed) {
                        auto [status, json_payload] = make_error(describeError(trimmed.error()), 500);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    nlohmann::json response{{"status", target_exists && overwrite ? "updated" : "created"},
                                            {"snapshot", make_snapshot_json(record)}};
                    res.status = target_exists && overwrite ? 200 : 201;
                    res.set_content(response.dump(2), "application/json");
                    res.set_header("Cache-Control", "no-store");
                });

    server.Delete("/inspector/snapshots",
                  [this, make_snapshot_context](httplib::Request const& req, httplib::Response& res) {
                      auto id = trim_copy(req.get_param_value("id"));
                      if (id.empty()) {
                          auto [status, payload] = make_error("snapshot id is required", 400);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }
                      auto sanitized = sanitize_snapshot_identifier(id);
                      if (sanitized.empty()) {
                          auto [status, payload] = make_error("snapshot id is invalid", 400);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }

                      auto context = make_snapshot_context(req);
                      auto removed = delete_snapshot_record(space_, context, sanitized);
                      if (!removed) {
                          auto [status, payload] = make_error(describeError(removed.error()), 500);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }
                      if (!*removed) {
                          auto [status, payload] = make_error("snapshot not found", 404);
                          res.status             = status;
                          res.set_content(payload, "application/json");
                          res.set_header("Cache-Control", "no-store");
                          return;
                      }

                      res.status = 204;
                      res.set_header("Cache-Control", "no-store");
                  });

    server.Get("/inspector/snapshots/export",
               [this, make_snapshot_context](httplib::Request const& req, httplib::Response& res) {
                   auto id = trim_copy(req.get_param_value("id"));
                   if (id.empty()) {
                       auto [status, payload] = make_error("snapshot id is required", 400);
                       res.status             = status;
                       res.set_content(payload, "application/json");
                       res.set_header("Cache-Control", "no-store");
                       return;
                   }
                   auto sanitized = sanitize_snapshot_identifier(id);
                   if (sanitized.empty()) {
                       auto [status, payload] = make_error("snapshot id is invalid", 400);
                       res.status             = status;
                       res.set_content(payload, "application/json");
                       res.set_header("Cache-Control", "no-store");
                       return;
                   }

                   auto context = make_snapshot_context(req);
                   auto record  = read_snapshot_record(space_, context.root, sanitized, true);
                   if (!record) {
                       auto [status, payload] = make_error("snapshot not found", 404);
                       res.status             = status;
                       res.set_content(payload, "application/json");
                       res.set_header("Cache-Control", "no-store");
                       return;
                   }

                   std::string filename = record->id;
                   filename.push_back('-');
                   filename.append(std::to_string(record->created_ms));
                   filename.append(".json");

                   res.status = 200;
                   if (auto augmented = augment_snapshot_export(record->export_payload, record->options)) {
                       res.set_content(*augmented, "application/json");
                   } else {
                       res.set_content(record->export_payload, "application/json");
                   }
                   res.set_header("Cache-Control", "no-store");
                   res.set_header("Content-Disposition",
                                  std::string{"attachment; filename=\""}.append(filename).append("\""));
               });

    server.Post("/inspector/snapshots/diff",
                [this, make_snapshot_context](httplib::Request const& req, httplib::Response& res) {
                    if (req.body.empty()) {
                        auto [status, payload] = make_error("missing diff payload", 400);
                        res.status             = status;
                        res.set_content(payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto payload = nlohmann::json::parse(req.body, nullptr, false);
                    if (payload.is_discarded()) {
                        auto [status, json_payload] = make_error("invalid JSON payload", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto before_id = sanitize_snapshot_identifier(trim_copy(payload.value("before", std::string{})));
                    auto after_id  = sanitize_snapshot_identifier(trim_copy(payload.value("after", std::string{})));
                    if (before_id.empty() || after_id.empty()) {
                        auto [status, json_payload] = make_error("before/after snapshot ids are required", 400);
                        res.status                 = status;
                        res.set_content(json_payload, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto context = make_snapshot_context(req);
                    auto before_record = read_snapshot_record(space_, context.root, before_id, true);
                    auto after_record  = read_snapshot_record(space_, context.root, after_id, true);
                    if (!before_record || !after_record) {
                        auto [status, payload_json] = make_error("snapshot not found", 404);
                        res.status                  = status;
                        res.set_content(payload_json, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto before_snapshot = ParseInspectorSnapshot(before_record->inspector_payload);
                    if (!before_snapshot) {
                        auto [status, payload_json] = make_error(describeError(before_snapshot.error()), 500);
                        res.status                  = status;
                        res.set_content(payload_json, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }
                    auto after_snapshot = ParseInspectorSnapshot(after_record->inspector_payload);
                    if (!after_snapshot) {
                        auto [status, payload_json] = make_error(describeError(after_snapshot.error()), 500);
                        res.status                  = status;
                        res.set_content(payload_json, "application/json");
                        res.set_header("Cache-Control", "no-store");
                        return;
                    }

                    auto delta      = BuildInspectorStreamDelta(*before_snapshot, *after_snapshot, 1);
                    auto delta_json = nlohmann::json::parse(SerializeInspectorStreamDeltaEvent(delta, 2));

                    nlohmann::json response{{"before", make_snapshot_json(*before_record)},
                                            {"after", make_snapshot_json(*after_record)},
                                            {"summary",
                                             {
                                                 {"added", delta.added.size()},
                                                 {"updated", delta.updated.size()},
                                                 {"removed", delta.removed.size()},
                                             }},
                                            {"changes", delta_json.value("changes", nlohmann::json::object())},
                                            {"diagnostics", delta_json.value("diagnostics", nlohmann::json::array())}};
                    res.status = 200;
                    res.set_content(response.dump(2), "application/json");
                    res.set_header("Cache-Control", "no-store");
                });

    server.Get("/inspector/cards/paint-example", [this](httplib::Request const& req, httplib::Response& res) {
        auto options = options_.paint_card;
        if (auto override_path = req.get_param_value("diagnostics_root"); !override_path.empty()) {
            options.diagnostics_root = override_path;
        }

        auto card = BuildPaintScreenshotCard(space_, options);
        if (!card) {
            auto [status, payload] = make_error(
                describeError(card.error()), 500);
            res.status = status;
            res.set_content(payload, "application/json");
            return;
        }

        res.status = 200;
        res.set_content(SerializePaintScreenshotCard(*card), "application/json");
    });
}

auto InspectorHttpServer::handle_ui_request(httplib::Response& res, std::string_view asset) -> void {
    auto bundle = LoadInspectorUiAsset(options_.ui_root, asset);
    res.status  = 200;
    res.set_content(bundle.content, bundle.content_type);
    res.set_header("Cache-Control", "no-store");
}

auto InspectorHttpServer::enforce_acl(httplib::Request const& req,
                                      httplib::Response& res,
                                      std::string const& requested_path,
                                      std::string_view endpoint) -> bool {
    if (!acl_.enabled()) {
        return false;
    }

    auto role     = extract_role(req);
    auto decision = acl_.evaluate(role, requested_path);
    if (decision.allowed) {
        return false;
    }

    auto payload = make_acl_error_payload(decision, endpoint);
    res.status   = 403;
    res.set_content(payload, "application/json");
    res.set_header("Cache-Control", "no-store");

    InspectorAclDecision log_decision = decision;
    if (log_decision.reason.empty()) {
        log_decision.reason = "access denied";
    }
    acl_.record_violation(log_decision, extract_user(req), req.remote_addr, endpoint);
    return true;
}

auto InspectorHttpServer::extract_role(httplib::Request const& req) const -> std::string {
    auto role = options_.acl.default_role;
    if (!options_.acl.role_header.empty()) {
        auto header = req.get_header_value(options_.acl.role_header);
        if (!header.empty()) {
            role = header;
        }
    }
    if (role.empty()) {
        role = "root";
    }
    return role;
}

auto InspectorHttpServer::extract_user(httplib::Request const& req) const -> std::string {
    if (options_.acl.user_header.empty()) {
        return {};
    }
    return req.get_header_value(options_.acl.user_header);
}

} // namespace SP::Inspector
