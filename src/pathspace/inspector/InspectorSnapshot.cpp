#include "inspector/InspectorSnapshot.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace SP::Inspector {
namespace {

struct ValueSample {
    std::string type;
    std::string summary;
};

[[nodiscard]] auto truncate_summary(std::string_view value, std::size_t limit = 96) -> std::string {
    if (value.size() <= limit) {
        return std::string{value};
    }
    std::string truncated;
    truncated.reserve(limit + 3);
    truncated.append(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(limit));
    truncated.append("...");
    return truncated;
}

class ValueSampler {
public:
    ValueSampler(bool enabled, std::vector<std::string>& diagnostics)
        : enabled_(enabled)
        , diagnostics_(diagnostics) {}

    auto sample(ValueHandle& handle, std::string const& path) -> std::optional<ValueSample> {
        if (!enabled_ || !handle.valid() || !handle.hasValues()) {
            return std::nullopt;
        }
        if (auto sample = trySample<bool>(handle, path, "bool", [](bool value) {
                return value ? "true" : "false";
            })) {
            return sample;
        }
        if (auto sample = trySample<std::int64_t>(handle, path, "int64", [](std::int64_t value) {
                return std::to_string(value);
            })) {
            return sample;
        }
        if (auto sample = trySample<std::uint64_t>(handle, path, "uint64", [](std::uint64_t value) {
                return std::to_string(value);
            })) {
            return sample;
        }
        if (auto sample = trySample<double>(handle, path, "double", [](double value) {
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(3);
                oss << value;
                return oss.str();
            })) {
            return sample;
        }
        if (auto sample = trySample<std::string>(handle, path, "string", [](std::string const& value) {
                return truncate_summary(value);
            })) {
            return sample;
        }
        return std::nullopt;
    }

private:
    template <typename T, typename Formatter>
    auto trySample(ValueHandle& handle,
                   std::string const& path,
                   std::string_view typeName,
                   Formatter&& formatter) -> std::optional<ValueSample> {
        auto read = handle.read<T>();
        if (!read) {
            switch (read.error().code) {
            case Error::Code::TypeMismatch:
            case Error::Code::SerializationFunctionMissing:
            case Error::Code::UnserializableType:
            case Error::Code::InvalidType:
            case Error::Code::NoObjectFound:
            case Error::Code::NoSuchPath:
            case Error::Code::NotFound:
                return std::nullopt;
            default:
                diagnostics_.push_back(std::string{"read failed for "}.append(path).append(": ")
                                           .append(describeError(read.error())));
                return std::nullopt;
            }
        }
        return ValueSample{std::string{typeName}, formatter(*read)};
    }

    bool                        enabled_;
    std::vector<std::string>&   diagnostics_;
};

[[nodiscard]] auto normalize_root(std::string root) -> std::string {
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

[[nodiscard]] auto parent_path(std::string const& path) -> std::string {
    if (path.empty() || path == "/") {
        return {};
    }
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }
    if (pos == 0) {
        return std::string{"/"};
    }
    return path.substr(0, pos);
}

struct NodeRecord {
    InspectorNodeSummary      summary;
    std::vector<std::string> children;
};

[[nodiscard]] auto is_descendant_path(std::string const& ancestor,
                                      std::string const& path) -> bool {
    if (ancestor.empty() || ancestor == path) {
        return false;
    }
    if (ancestor == "/") {
        return path != "/";
    }
    if (path.size() <= ancestor.size()) {
        return false;
    }
    if (path.compare(0, ancestor.size(), ancestor) != 0) {
        return false;
    }
    auto const next = path[ancestor.size()];
    return next == '/';
}

[[nodiscard]] auto collapse_removed_paths(std::vector<std::string> removed)
    -> std::vector<std::string> {
    std::sort(removed.begin(),
              removed.end(),
              [](std::string const& lhs, std::string const& rhs) {
                  if (lhs.size() != rhs.size()) {
                      return lhs.size() < rhs.size();
                  }
                  return lhs < rhs;
              });
    std::vector<std::string> filtered;
    filtered.reserve(removed.size());
    for (auto const& path : removed) {
        bool hasAncestor = false;
        for (auto const& kept : filtered) {
            if (is_descendant_path(kept, path)) {
                hasAncestor = true;
                break;
            }
        }
        if (!hasAncestor) {
            filtered.push_back(path);
        }
    }
    std::sort(filtered.begin(), filtered.end());
    return filtered;
}

class SnapshotBuilder {
public:
    SnapshotBuilder(PathSpace& space, InspectorSnapshotOptions const& options)
        : space_(space)
        , options_(options) {}

    auto build() -> Expected<InspectorSnapshot> {
        rootPath_ = normalize_root(options_.root);

        VisitOptions visit{};
        visit.root                = rootPath_;
        visit.maxDepth            = options_.max_depth;
        visit.maxChildren         = options_.max_children == 0
                                    ? std::numeric_limits<std::size_t>::max()
                                    : options_.max_children;
        visit.includeNestedSpaces = true;
        visit.includeValues       = options_.include_values;

        ValueSampler sampler(options_.include_values, diagnostics_);

        auto visitor = [&](PathEntry const& entry, ValueHandle& handle) -> VisitControl {
            std::string path = entry.path.empty() ? std::string{"/"} : entry.path;
            if (shouldSkipNode(path)) {
                return VisitControl::SkipChildren;
            }

            auto parent = parent_path(path);
            auto& node  = ensureNode(path);
            node.summary.path               = path;
            node.summary.child_count        = entry.approxChildCount;
            node.summary.children_truncated = childrenTruncated(entry);
            node.summary.value_summary.clear();

            if (!entry.hasChildren && options_.include_values) {
                node.summary.value_type = "opaque";
                if (auto sample = sampler.sample(handle, path)) {
                    node.summary.value_type    = std::move(sample->type);
                    node.summary.value_summary = std::move(sample->summary);
                }
            } else {
                node.summary.value_type = entry.hasChildren ? "object" : "value";
            }

            if (!parent.empty()) {
                auto& parentNode = ensureNode(parent);
                parentNode.children.push_back(path);
            }

            return VisitControl::Continue;
        };

        auto visitResult = space_.visit(visitor, visit);
        if (!visitResult) {
            return std::unexpected(visitResult.error());
        }

        auto rootSummary = buildTree(rootPath_);
        InspectorSnapshot snapshot{
            .options     = options_,
            .root        = std::move(rootSummary),
            .diagnostics = std::move(diagnostics_),
        };
        snapshot.options.root = rootPath_;
        return snapshot;
    }

private:
    auto ensureNode(std::string const& path) -> NodeRecord& {
        auto [it, inserted] = nodes_.try_emplace(path);
        if (inserted) {
            it->second.summary.path = path;
        }
        return it->second;
    }

    auto shouldSkipNode(std::string const& path) const -> bool {
        if (options_.max_children != 0) {
            return false;
        }
        return path != rootPath_ && !parent_path(path).empty();
    }

    auto childrenTruncated(PathEntry const& entry) const -> bool {
        if (!entry.hasChildren) {
            return false;
        }
        if (options_.max_children == 0) {
            return entry.approxChildCount > 0;
        }
        return entry.approxChildCount > options_.max_children;
    }

    auto buildTree(std::string const& path) -> InspectorNodeSummary {
        auto it = nodes_.find(path);
        if (it == nodes_.end()) {
            InspectorNodeSummary missing;
            missing.path       = path;
            missing.value_type = "value";
            return missing;
        }

        InspectorNodeSummary summary = it->second.summary;
        summary.children.clear();
        summary.children.reserve(it->second.children.size());
        for (auto const& childPath : it->second.children) {
            summary.children.push_back(buildTree(childPath));
        }
        return summary;
    }

    PathSpace&                            space_;
    InspectorSnapshotOptions              options_;
    std::string                           rootPath_;
    std::vector<std::string>              diagnostics_;
    std::unordered_map<std::string, NodeRecord> nodes_;
};

[[nodiscard]] auto snapshot_options_json(InspectorSnapshotOptions const& options)
    -> nlohmann::json {
    return nlohmann::json{
        {"root", options.root},
        {"max_depth", options.max_depth},
        {"max_children", options.max_children},
        {"include_values", options.include_values},
    };
}

[[nodiscard]] auto to_json(InspectorNodeSummary const& node) -> nlohmann::json {
    nlohmann::json result{
        {"path", node.path},
        {"value_type", node.value_type},
        {"value_summary", node.value_summary},
        {"child_count", node.child_count},
        {"children_truncated", node.children_truncated},
    };

    if (!node.children.empty()) {
        nlohmann::json children_json = nlohmann::json::array();
        for (auto const& child : node.children) {
            children_json.push_back(to_json(child));
        }
        result["children"] = std::move(children_json);
    }
    return result;
}

struct FlatNodeRecord {
    InspectorNodeSummary const* summary;
    std::size_t                 fingerprint;
};

[[nodiscard]] auto hash_combine(std::size_t seed, std::size_t value) -> std::size_t {
    constexpr std::size_t kMagic = 0x9e3779b97f4a7c15ULL;
    seed ^= value + kMagic + (seed << 6U) + (seed >> 2U);
    return seed;
}

[[nodiscard]] auto node_fingerprint(InspectorNodeSummary const& node) -> std::size_t {
    std::size_t fingerprint = std::hash<std::string>{}(node.path);
    fingerprint             = hash_combine(fingerprint, std::hash<std::string>{}(node.value_type));
    fingerprint             = hash_combine(fingerprint, std::hash<std::string>{}(node.value_summary));
    fingerprint             = hash_combine(fingerprint, std::hash<std::size_t>{}(node.child_count));
    fingerprint             = hash_combine(fingerprint, node.children_truncated ? 0x1ULL : 0x0ULL);
    return fingerprint;
}

auto collect_flat_nodes(InspectorNodeSummary const& node,
                        std::unordered_map<std::string, FlatNodeRecord>& out) -> void {
    auto fingerprint = node_fingerprint(node);
    out.insert_or_assign(node.path, FlatNodeRecord{&node, fingerprint});
    for (auto const& child : node.children) {
        collect_flat_nodes(child, out);
    }
}

[[nodiscard]] auto snapshot_to_map(InspectorSnapshot const& snapshot)
    -> std::unordered_map<std::string, FlatNodeRecord> {
    std::unordered_map<std::string, FlatNodeRecord> flat;
    flat.reserve(64);
    collect_flat_nodes(snapshot.root, flat);
    return flat;
}

[[nodiscard]] auto node_from_json(nlohmann::json const& json)
    -> Expected<InspectorNodeSummary> {
    if (!json.is_object()) {
        return std::unexpected(
            Error{Error::Code::MalformedInput, "inspector node must be an object"});
    }

    InspectorNodeSummary node;
    node.path               = json.value("path", std::string{"/"});
    node.value_type         = json.value("value_type", std::string{});
    node.value_summary      = json.value("value_summary", std::string{});
    node.child_count        = json.value("child_count", std::size_t{0});
    node.children_truncated = json.value("children_truncated", false);

    if (auto it = json.find("children"); it != json.end()) {
        if (!it->is_array()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                         "inspector node children must be an array"});
        }
        node.children.reserve(it->size());
        for (auto const& child : *it) {
            auto parsed = node_from_json(child);
            if (!parsed) {
                return parsed;
            }
            node.children.push_back(std::move(*parsed));
        }
    }
    return node;
}

[[nodiscard]] auto parse_snapshot_json(nlohmann::json const& json)
    -> Expected<InspectorSnapshot> {
    if (!json.is_object()) {
        return std::unexpected(
            Error{Error::Code::MalformedInput, "inspector snapshot must be an object"});
    }

    auto options_it = json.find("options");
    if (options_it == json.end() || !options_it->is_object()) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "inspector snapshot missing options"});
    }

    InspectorSnapshotOptions options;
    options.root           = options_it->value("root", std::string{"/"});
    options.max_depth      = options_it->value("max_depth", std::size_t{2});
    options.max_children   = options_it->value("max_children", std::size_t{32});
    options.include_values = options_it->value("include_values", true);

    auto root_it = json.find("root");
    if (root_it == json.end()) {
        return std::unexpected(Error{Error::Code::MalformedInput,
                                     "inspector snapshot missing root"});
    }
    auto root_summary = node_from_json(*root_it);
    if (!root_summary) {
        return std::unexpected(root_summary.error());
    }

    std::vector<std::string> diagnostics;
    if (auto diag_it = json.find("diagnostics"); diag_it != json.end()) {
        if (!diag_it->is_array()) {
            return std::unexpected(Error{Error::Code::MalformedInput,
                                         "inspector snapshot diagnostics must be an array"});
        }
        diagnostics.reserve(diag_it->size());
        for (auto const& entry : *diag_it) {
            if (!entry.is_string()) {
                return std::unexpected(Error{Error::Code::MalformedInput,
                                             "inspector diagnostics entries must be strings"});
            }
            diagnostics.push_back(entry.get<std::string>());
        }
    }

    InspectorSnapshot snapshot;
    snapshot.options     = std::move(options);
    snapshot.root        = std::move(*root_summary);
    snapshot.diagnostics = std::move(diagnostics);
    return snapshot;
}

} // namespace

auto BuildInspectorSnapshot(PathSpace& space,
                            InspectorSnapshotOptions const& options)
    -> Expected<InspectorSnapshot> {
    SnapshotBuilder builder(space, options);
    return builder.build();
}

auto SerializeInspectorSnapshot(InspectorSnapshot const& snapshot, int indent) -> std::string {
    nlohmann::json json{
        {"options", snapshot_options_json(snapshot.options)},
        {"root", to_json(snapshot.root)},
        {"diagnostics", snapshot.diagnostics},
    };
    return json.dump(indent);
}

auto BuildInspectorStreamDelta(InspectorSnapshot const& previous,
                               InspectorSnapshot const& current,
                               std::uint64_t version) -> InspectorStreamDelta {
    InspectorStreamDelta delta;
    delta.options     = current.options;
    delta.root_path   = current.root.path;
    delta.version     = version;
    delta.diagnostics = current.diagnostics;

    auto previous_map = snapshot_to_map(previous);
    auto current_map  = snapshot_to_map(current);

    for (auto const& [path, record] : current_map) {
        auto it = previous_map.find(path);
        if (it == previous_map.end()) {
            delta.added.push_back(*record.summary);
            continue;
        }
        if (it->second.fingerprint != record.fingerprint) {
            delta.updated.push_back(*record.summary);
        }
    }

    for (auto const& [path, record] : previous_map) {
        if (current_map.find(path) == current_map.end()) {
            (void)record;
            delta.removed.push_back(path);
        }
    }

    auto sort_by_path = [](InspectorNodeSummary const& lhs,
                           InspectorNodeSummary const& rhs) {
        return lhs.path < rhs.path;
    };

    std::sort(delta.added.begin(), delta.added.end(), sort_by_path);
    std::sort(delta.updated.begin(), delta.updated.end(), sort_by_path);
    delta.removed = collapse_removed_paths(std::move(delta.removed));

    return delta;
}

auto SerializeInspectorStreamSnapshotEvent(InspectorSnapshot const& snapshot,
                                          std::uint64_t version,
                                          int indent) -> std::string {
    nlohmann::json json{
        {"event", "snapshot"},
        {"version", version},
        {"options", snapshot_options_json(snapshot.options)},
        {"root", to_json(snapshot.root)},
        {"diagnostics", snapshot.diagnostics},
    };
    return json.dump(indent);
}

auto SerializeInspectorStreamDeltaEvent(InspectorStreamDelta const& delta, int indent) -> std::string {
    nlohmann::json changes{
        {"added", nlohmann::json::array()},
        {"updated", nlohmann::json::array()},
        {"removed", delta.removed},
    };

    for (auto const& node : delta.added) {
        changes["added"].push_back(to_json(node));
    }
    for (auto const& node : delta.updated) {
        changes["updated"].push_back(to_json(node));
    }

    nlohmann::json json{
        {"event", "delta"},
        {"version", delta.version},
        {"root", delta.root_path},
        {"options", snapshot_options_json(delta.options)},
        {"diagnostics", delta.diagnostics},
        {"changes", std::move(changes)},
    };
    return json.dump(indent);
}

auto ParseInspectorSnapshot(std::string const& payload) -> Expected<InspectorSnapshot> {
    auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        return std::unexpected(
            Error{Error::Code::MalformedInput, "invalid inspector snapshot JSON"});
    }
    return parse_snapshot_json(json);
}

} // namespace SP::Inspector
