#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct InspectorSnapshotOptions {
    std::string root          = "/";
    std::size_t max_depth     = 2;
    std::size_t max_children  = 32;
    bool        include_values = true;
};

struct InspectorNodeSummary {
    std::string                  path;
    std::string                  value_type;
    std::string                  value_summary;
    std::size_t                  child_count       = 0;
    bool                         children_truncated = false;
    std::vector<InspectorNodeSummary> children;
};

struct InspectorSnapshot {
    InspectorSnapshotOptions      options;
    InspectorNodeSummary          root;
    std::vector<std::string>      diagnostics;
};

auto BuildInspectorSnapshot(PathSpace& space,
                            InspectorSnapshotOptions const& options)
    -> Expected<InspectorSnapshot>;

auto SerializeInspectorSnapshot(InspectorSnapshot const& snapshot,
                                int indent = 2) -> std::string;

} // namespace Inspector
} // namespace SP

