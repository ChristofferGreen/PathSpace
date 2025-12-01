#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct InspectorAclRuleConfig {
    std::string              role;
    std::vector<std::string> roots;
    bool                     allow_all = false;
};

struct InspectorAclOptions {
    std::string                     default_role    = "root";
    std::string                     role_header     = "x-pathspace-role";
    std::string                     user_header     = "x-pathspace-user";
    std::string                     diagnostics_root = "/diagnostics/web/inspector/acl";
    std::vector<InspectorAclRuleConfig> rules;
};

struct InspectorAclDecision {
    bool                     allowed        = true;
    std::string              role;
    std::string              requested_path;
    std::string              reason;
    std::vector<std::string> allowed_roots;
};

class InspectorAcl {
public:
    InspectorAcl(PathSpace& space, InspectorAclOptions options = {});

    [[nodiscard]] bool enabled() const;

    [[nodiscard]] auto evaluate(std::string const& role,
                                std::string const& requested_path) const -> InspectorAclDecision;

    auto record_violation(InspectorAclDecision const& decision,
                          std::string const& user,
                          std::string const& client,
                          std::string_view endpoint) -> void;

private:
    struct RuleEntry {
        std::string              role;
        std::vector<std::string> roots;
        bool                     allow_all = false;
    };

    static auto normalize_path(std::string path) -> std::string;
    static bool is_within(std::string const& path, std::string const& root);

    [[nodiscard]] auto find_rule(std::string const& role) const -> RuleEntry const*;
    [[nodiscard]] auto current_time_ms() const -> std::uint64_t;
    [[nodiscard]] auto build_event_path(std::uint64_t timestamp_ms) const -> std::string;

    PathSpace&              space_;
    InspectorAclOptions     options_;
    std::vector<RuleEntry>  rules_;
    bool                    enabled_ = false;
    std::string             diagnostics_root_;

    mutable std::mutex      mutex_;
    std::uint64_t           violation_count_ = 0;
};

[[nodiscard]] auto NormalizeInspectorPath(std::string path) -> std::string;

} // namespace Inspector
} // namespace SP
