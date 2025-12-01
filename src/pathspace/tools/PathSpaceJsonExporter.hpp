#pragma once

#include "PathSpaceBase.hpp"

#include <string>

namespace SP {

class PathSpaceJsonExporter {
public:
    static auto Export(PathSpaceBase& space, PathSpaceJsonOptions const& options = PathSpaceJsonOptions{})
        -> Expected<std::string>;
};

} // namespace SP
