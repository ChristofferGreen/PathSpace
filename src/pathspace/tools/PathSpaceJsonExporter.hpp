#pragma once

#include "PathSpaceBase.hpp"

#include <string>

namespace SP {

class PathSpaceJsonExporter {
public:
    static auto Export(PathSpaceBase& space, PathSpaceJsonOptions const& options = PathSpaceJsonOptions{})
        -> Expected<std::string>;
};

// Lightweight namespaced helper so callers can write `SP::JSON::Export(space, opts)`
// without adding another member to PathSpaceBase. Lives in the tools layer to
// keep the core API minimal and JSON concerns opt-in.
namespace JSON {
inline auto Export(PathSpaceBase& space, PathSpaceJsonOptions const& options = PathSpaceJsonOptions{})
    -> Expected<std::string> {
    return PathSpaceJsonExporter::Export(space, options);
}
} // namespace JSON

} // namespace SP
