#include <pathspace/ui/LegacyBuildersDeprecation.hpp>

#include "BuildersDetail.hpp"

#include <pathspace/log/TaggedLogger.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

namespace SP::UI::LegacyBuilders {

namespace {

using SP::UI::Builders::Detail::make_error;
using SP::UI::Builders::Detail::read_optional;
using SP::UI::Builders::Detail::replace_single;

constexpr std::string_view kDiagnosticsRoot = "/_system/diagnostics/legacy_widget_builders";
constexpr std::string_view kStatusPhasePath = "/_system/diagnostics/legacy_widget_builders/status/phase";
constexpr std::string_view kStatusDeadlinePath =
    "/_system/diagnostics/legacy_widget_builders/status/support_window_expires";
constexpr std::string_view kStatusDocPath =
    "/_system/diagnostics/legacy_widget_builders/status/plan";
constexpr std::string_view kSupportWindowDeadline = "2026-02-01T00:00:00Z";

enum class EnforcementMode {
    Warn,
    Allow,
    Error,
};

auto parse_mode(std::string_view value) -> EnforcementMode {
    auto equals = [](std::string_view lhs, std::string_view rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(lhs[i]))
                != std::tolower(static_cast<unsigned char>(rhs[i]))) {
                return false;
            }
        }
        return true;
    };

    if (equals(value, "allow")) {
        return EnforcementMode::Allow;
    }
    if (equals(value, "error")) {
        return EnforcementMode::Error;
    }
    return EnforcementMode::Warn;
}

auto mode_from_env() -> EnforcementMode {
    static EnforcementMode mode = [] {
        if (const char* env = std::getenv("PATHSPACE_LEGACY_WIDGET_BUILDERS")) {
            if (*env) {
                return parse_mode(env);
            }
        }
        return EnforcementMode::Warn;
    }();
    return mode;
}

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto sanitize_component(std::string_view entry_point) -> std::string {
    std::string sanitized;
    sanitized.reserve(entry_point.size());
    for (char ch : entry_point) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            sanitized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            continue;
        }
        switch (ch) {
            case ':':
            case ' ':
            case '.':
            case '#':
                sanitized.push_back('_');
                break;
            case '(':
            case ')':
            case ',':
            case '&':
            case '*':
            case '<':
            case '>':
            case '\'':
            case '"':
            case '/':
                sanitized.push_back('_');
                break;
            default:
                sanitized.push_back('_');
                break;
        }
    }
    if (sanitized.empty()) {
        sanitized = "entry";
    }
    return sanitized;
}

struct ProcessState {
    std::mutex mutex;
    bool banner_emitted = false;
    std::unordered_set<std::string> warned_entries;
};

auto state() -> ProcessState& {
    static ProcessState s;
    return s;
}

auto emit_log_once(std::string_view entry_point, std::optional<SP::ConcretePathStringView> path_hint) -> void {
#ifdef SP_LOG_DEBUG
    auto& shared = state();
    auto lock = std::unique_lock<std::mutex>{shared.mutex};
    if (!shared.banner_emitted) {
        sp_log("Legacy widget builders are deprecated; migrate to SP::UI::Declarative::* APIs. "
               "See docs/Plan_WidgetDeclarativeAPI.md for the support window.",
               "LegacyBuilders",
               "UI");
        shared.banner_emitted = true;
    }
    if (shared.warned_entries.insert(std::string(entry_point)).second) {
        if (path_hint.has_value()) {
            sp_log(std::string("Legacy builder entry '")
                       + std::string(entry_point)
                       + "' invoked for "
                       + std::string(path_hint->getPath()),
                   "LegacyBuilders",
                   "UI");
        } else {
            sp_log(std::string("Legacy builder entry '") + std::string(entry_point) + "' invoked",
                   "LegacyBuilders",
                   "UI");
        }
    }
#else
    (void)entry_point;
    (void)path_hint;
#endif
}

auto ensure_status_paths(PathSpace& space) -> void {
    auto mode = mode_from_env();
    auto phase = std::string_view{"warning"};
    if (mode == EnforcementMode::Error) {
        phase = "blocked";
    } else if (mode == EnforcementMode::Allow) {
        phase = "allow";
    }
    (void)replace_single<std::string>(space, std::string{kStatusPhasePath}, std::string{phase});
    (void)replace_single<std::string>(space, std::string{kStatusDeadlinePath}, std::string{kSupportWindowDeadline});
    (void)replace_single<std::string>(space, std::string{kStatusDocPath}, "docs/Plan_WidgetDeclarativeAPI.md");
}

} // namespace

auto SupportWindowDeadline() -> std::string_view {
    return kSupportWindowDeadline;
}

auto NoteUsage(PathSpace& space,
               std::string_view entry_point,
               std::optional<SP::ConcretePathStringView> path_hint) -> SP::Expected<void> {
    ensure_status_paths(space);
    emit_log_once(entry_point, path_hint);

    auto sanitized = sanitize_component(entry_point);
    auto base = std::string{kDiagnosticsRoot} + "/" + sanitized;
    auto usage_path = base + "/usage_total";
    auto last_entry_path = base + "/last_entry";
    auto last_path_path = base + "/last_path";
    auto last_timestamp_path = base + "/last_timestamp_ns";

    auto current = read_optional<std::uint64_t>(space, usage_path);
    if (!current) {
        return std::unexpected(current.error());
    }
    auto next_total = current->value_or(0) + 1;

    if (auto status = replace_single<std::uint64_t>(space, usage_path, next_total); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, last_entry_path, std::string(entry_point)); !status) {
        return status;
    }
    if (path_hint.has_value()) {
        (void)replace_single<std::string>(space, last_path_path, std::string(path_hint->getPath()));
    }
    if (auto status = replace_single<std::uint64_t>(space, last_timestamp_path, now_ns()); !status) {
        return status;
    }

    switch (mode_from_env()) {
        case EnforcementMode::Allow:
            return {};
        case EnforcementMode::Warn:
            return {};
        case EnforcementMode::Error:
            return std::unexpected(make_error(
                "legacy widget builders are disabled (set PATHSPACE_LEGACY_WIDGET_BUILDERS=allow to bypass locally)",
                SP::Error::Code::NotSupported));
    }
    return {};
}

auto NoteUsage(PathSpace const& space,
               std::string_view entry_point,
               std::optional<SP::ConcretePathStringView> path_hint) -> SP::Expected<void> {
    return NoteUsage(const_cast<PathSpace&>(space), entry_point, path_hint);
}

} // namespace SP::UI::LegacyBuilders
