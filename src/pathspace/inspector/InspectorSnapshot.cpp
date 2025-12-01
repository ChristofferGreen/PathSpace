#include "inspector/InspectorSnapshot.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "path/ConcretePath.hpp"

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

} // namespace

auto BuildInspectorSnapshot(PathSpace& space,
                            InspectorSnapshotOptions const& options)
    -> Expected<InspectorSnapshot> {
    SnapshotBuilder builder(space, options);
    return builder.build();
}

auto SerializeInspectorSnapshot(InspectorSnapshot const& snapshot, int indent) -> std::string {
    nlohmann::json json{
        {"options",
         {
             {"root", snapshot.options.root},
             {"max_depth", snapshot.options.max_depth},
             {"max_children", snapshot.options.max_children},
             {"include_values", snapshot.options.include_values},
         }},
        {"root", to_json(snapshot.root)},
        {"diagnostics", snapshot.diagnostics},
    };
    return json.dump(indent);
}

} // namespace SP::Inspector

