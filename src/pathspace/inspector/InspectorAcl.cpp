#include "inspector/InspectorAcl.hpp"

#include "PathSpace.hpp"
#include "inspector/InspectorMetricUtils.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace {

auto normalize_impl(std::string path) -> std::string {
    if (path.empty()) {
        path = "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }

    std::string normalized;
    normalized.reserve(path.size());
    bool last_was_slash = false;
    for (char ch : path) {
        if (ch == '/') {
            if (!last_was_slash) {
                normalized.push_back('/');
                last_was_slash = true;
            }
            continue;
        }
        normalized.push_back(ch);
        last_was_slash = false;
    }
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        normalized = "/";
    }
    return normalized;
}

bool is_subpath(std::string const& path, std::string const& root) {
    if (root == "/") {
        return true;
    }
    if (path == root) {
        return true;
    }
    if (path.size() < root.size()) {
        return false;
    }
    if (path.rfind(root, 0) != 0) {
        return false;
    }
    if (path.size() == root.size()) {
        return true;
    }
    return path[root.size()] == '/';
}

} // namespace

namespace SP::Inspector {

InspectorAcl::InspectorAcl(PathSpace& space, InspectorAclOptions options)
    : space_(space)
    , options_(std::move(options)) {
    if (options_.role_header.empty()) {
        options_.role_header = "x-pathspace-role";
    }
    if (options_.user_header.empty()) {
        options_.user_header = "x-pathspace-user";
    }
    if (options_.default_role.empty()) {
        options_.default_role = "root";
    }

    diagnostics_root_ = normalize_impl(options_.diagnostics_root);
    if (diagnostics_root_.empty()) {
        diagnostics_root_ = "/diagnostics/web/inspector/acl";
    }

    rules_.reserve(options_.rules.size());
    for (auto const& rule : options_.rules) {
        RuleEntry entry;
        entry.role      = rule.role;
        entry.allow_all = rule.allow_all;
        for (auto const& root : rule.roots) {
            auto normalized = normalize_impl(root);
            if (!normalized.empty()) {
                entry.roots.push_back(std::move(normalized));
            }
        }
        if (entry.role.empty()) {
            continue;
        }
        rules_.push_back(std::move(entry));
    }
    enabled_ = !rules_.empty();
}

bool InspectorAcl::enabled() const {
    return enabled_;
}

auto InspectorAcl::normalize_path(std::string path) -> std::string {
    return normalize_impl(std::move(path));
}

bool InspectorAcl::is_within(std::string const& path, std::string const& root) {
    return is_subpath(path, root);
}

auto InspectorAcl::find_rule(std::string const& role) const -> RuleEntry const* {
    auto it = std::find_if(rules_.begin(), rules_.end(), [&](RuleEntry const& entry) {
        return entry.role == role;
    });
    if (it == rules_.end()) {
        return nullptr;
    }
    return &*it;
}

auto InspectorAcl::evaluate(std::string const& role, std::string const& requested_path) const
    -> InspectorAclDecision {
    InspectorAclDecision decision;
    decision.role           = role;
    decision.requested_path = normalize_impl(requested_path);
    decision.allowed        = true;

    if (!enabled_) {
        return decision;
    }

    auto const* rule = find_rule(role);
    if (rule == nullptr) {
        return decision;
    }

    decision.allowed_roots = rule->roots;
    if (rule->allow_all || rule->roots.empty()) {
        return decision;
    }

    for (auto const& root : rule->roots) {
        if (is_subpath(decision.requested_path, root)) {
            return decision;
        }
    }

    decision.allowed = false;
    decision.reason  = std::string{"path '"}.append(decision.requested_path).append("' is outside allowed roots");
    return decision;
}

auto InspectorAcl::current_time_ms() const -> std::uint64_t {
    auto const now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

auto InspectorAcl::build_event_path(std::uint64_t timestamp_ms) const -> std::string {
    std::ostringstream oss;
    oss << diagnostics_root_ << "/violations/events/" << std::setw(20) << std::setfill('0')
        << timestamp_ms;
    return oss.str();
}

auto InspectorAcl::record_violation(InspectorAclDecision const& decision,
                                    std::string const& user,
                                    std::string const& client,
                                    std::string_view endpoint) -> void {
    if (!enabled_ || decision.allowed) {
        return;
    }

    auto const timestamp = current_time_ms();

    std::uint64_t total = 0;
    {
        std::lock_guard lock(mutex_);
        violation_count_ = violation_count_ + 1;
        total            = violation_count_;
    }

    auto build_value = [&](std::string const& suffix) { return diagnostics_root_ + suffix; };
    auto publish_metric = [&](std::string const& suffix, auto const& value) {
        auto path = build_value(suffix);
        if (auto replaced = Detail::ReplaceMetricValue(space_, path, value); !replaced) {
            (void)replaced;
        }
    };

    publish_metric("/violations/total", total);
    publish_metric("/violations/last/timestamp_ms", timestamp);
    publish_metric("/violations/last/role", decision.role);
    publish_metric("/violations/last/requested_path", decision.requested_path);
    publish_metric("/violations/last/endpoint", std::string{endpoint});
    publish_metric("/violations/last/client", client.empty() ? std::string{"unknown"} : client);
    publish_metric("/violations/last/user", user.empty() ? std::string{"anonymous"} : user);
    publish_metric("/violations/last/reason",
                   decision.reason.empty() ? std::string{"access denied"} : decision.reason);

    nlohmann::json event{{"timestamp_ms", timestamp},
                         {"role", decision.role},
                         {"requested_path", decision.requested_path},
                         {"endpoint", endpoint},
                         {"reason", decision.reason},
                         {"allowed_roots", decision.allowed_roots},
                         {"user", user},
                         {"client", client}};
    space_.insert(build_event_path(timestamp), event.dump());
}

auto NormalizeInspectorPath(std::string path) -> std::string {
    return normalize_impl(std::move(path));
}

} // namespace SP::Inspector
