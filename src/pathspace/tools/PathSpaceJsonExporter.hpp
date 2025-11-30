#pragma once

#include "PathSpaceBase.hpp"

#include <string>

namespace SP {

class PathSpaceJsonExporter {
public:
    static auto Export(PathSpaceBase& space, PathSpaceJsonOptions const& options = PathSpaceJsonOptions{})
        -> Expected<std::string>;

    static auto WriteToFile(PathSpaceBase& space,
                            std::string const& filePath,
                            PathSpaceJsonOptions const& options = PathSpaceJsonOptions{}) -> Expected<void>;
};

} // namespace SP

