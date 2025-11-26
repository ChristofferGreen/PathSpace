#include "inspector/InspectorSnapshot.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>

#include "nlohmann/json.hpp"

namespace SP::Inspector {
namespace {

[[nodiscard]] auto join_paths(std::string const& base, std::string const& child) -> std::string {
    if (base.empty() || base == "/") {
        return std::string{"/"}.append(child);
    }
    std::string full = base;
    if (!full.empty() && full.back() != '/') {
        full.push_back('/');
    }
    full.append(child);
    return full;
}

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

struct ValueSample {
    std::string type;
    std::string summary;
};

template <typename T, typename Formatter>
auto try_sample(PathSpace& space,
                std::string const& path,
                std::string_view   type_name,
                Formatter&&        formatter,
                std::vector<std::string>& diagnostics) -> std::optional<ValueSample> {
    auto read = space.read<T, std::string>(path);
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
        default: {
            diagnostics.push_back(
                std::string{"read failed for "}
                .append(path)
                .append(": ")
                .append(describeError(read.error())));
            return std::nullopt;
        }
        }
    }
    return ValueSample{
        std::string{type_name},
        formatter(*read),
    };
}

[[nodiscard]] auto list_children(PathSpace& space, std::string const& root) -> std::vector<std::string> {
    if (root.empty() || root == "/") {
        return space.listChildren();
    }
    ConcretePathStringView view{std::string_view{root}};
    return space.listChildren(view);
}

struct SnapshotBuilder {
    PathSpace&                   space;
    InspectorSnapshotOptions     options;
    std::vector<std::string>     diagnostics;

    [[nodiscard]] auto build(std::string const& path, std::size_t depth) -> InspectorNodeSummary {
        InspectorNodeSummary node;
        node.path = path.empty() ? "/" : path;

        auto children = list_children(space, node.path);
        std::sort(children.begin(), children.end());

        node.child_count        = children.size();
        node.children_truncated = false;

        if (children.empty() && options.include_values) {
            node.value_type = "opaque";
            if (auto sample = sample_value(node.path)) {
                node.value_type    = std::move(sample->type);
                node.value_summary = std::move(sample->summary);
            }
        } else {
            node.value_type = children.empty() ? "value" : "object";
        }

        if (depth >= options.max_depth) {
            node.children_truncated = node.child_count > 0;
            return node;
        }

        auto const limit = std::min<std::size_t>(children.size(), options.max_children);
        for (std::size_t i = 0; i < limit; ++i) {
            auto const child_path = join_paths(node.path, children[i]);
            node.children.push_back(build(child_path, depth + 1));
        }
        node.children_truncated = children.size() > limit;
        return node;
    }

    [[nodiscard]] auto sample_value(std::string const& path) -> std::optional<ValueSample> {
        if (!options.include_values) {
            return std::nullopt;
        }
        if (auto sample = try_sample<bool>(
                space,
                path,
                "bool",
                [](bool value) { return value ? "true" : "false"; },
                diagnostics)) {
            return sample;
        }
        if (auto sample = try_sample<std::int64_t>(
                space,
                path,
                "int64",
                [](std::int64_t value) { return std::to_string(value); },
                diagnostics)) {
            return sample;
        }
        if (auto sample = try_sample<std::uint64_t>(
                space,
                path,
                "uint64",
                [](std::uint64_t value) { return std::to_string(value); },
                diagnostics)) {
            return sample;
        }
        if (auto sample = try_sample<double>(
                space,
                path,
                "double",
                [](double value) {
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(3);
                    oss << value;
                    return oss.str();
                },
                diagnostics)) {
            return sample;
        }
        if (auto sample = try_sample<std::string>(
                space,
                path,
                "string",
                [](std::string const& value) { return truncate_summary(value); },
                diagnostics)) {
            return sample;
        }
        return std::nullopt;
    }
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
    SnapshotBuilder builder{
        .space      = space,
        .options    = options,
        .diagnostics = {},
    };

    auto normalized_root = options.root.empty() ? std::string{"/"} : options.root;
    if (normalized_root.front() != '/') {
        normalized_root.insert(normalized_root.begin(), '/');
    }

    auto root_summary = builder.build(normalized_root, 0);
    InspectorSnapshot snapshot{
        .options      = options,
        .root         = std::move(root_summary),
        .diagnostics  = std::move(builder.diagnostics),
    };
    snapshot.options.root = normalized_root;
    return snapshot;
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
