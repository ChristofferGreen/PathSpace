#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
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

struct InspectorStreamDelta {
    InspectorSnapshotOptions       options;
    std::string                    root_path;
    std::uint64_t                  version = 0;
    std::vector<InspectorNodeSummary> added;
    std::vector<InspectorNodeSummary> updated;
    std::vector<std::string>       removed;
    std::vector<std::string>       diagnostics;

    [[nodiscard]] bool has_changes() const {
        return !added.empty() || !updated.empty() || !removed.empty();
    }
};

auto BuildInspectorSnapshot(PathSpace& space,
                            InspectorSnapshotOptions const& options)
    -> Expected<InspectorSnapshot>;

auto SerializeInspectorSnapshot(InspectorSnapshot const& snapshot,
                                int indent = 2) -> std::string;

auto ParseInspectorSnapshot(std::string const& payload)
    -> Expected<InspectorSnapshot>;

auto BuildInspectorStreamDelta(InspectorSnapshot const& previous,
                               InspectorSnapshot const& current,
                               std::uint64_t version) -> InspectorStreamDelta;

auto SerializeInspectorStreamSnapshotEvent(InspectorSnapshot const& snapshot,
                                          std::uint64_t version,
                                          int indent = -1) -> std::string;

auto SerializeInspectorStreamDeltaEvent(InspectorStreamDelta const& delta,
                                       int indent = -1) -> std::string;

} // namespace Inspector
} // namespace SP
