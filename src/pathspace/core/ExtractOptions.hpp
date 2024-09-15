#pragma once
#include "BlockOptions.hpp"
#include "Capabilities.hpp"
#include "ExecutionOptions.hpp"

#include <optional>

namespace SP {

struct ExtractOptions {
    std::optional<Capabilities> capabilities;
    std::optional<BlockOptions> block;
    std::optional<int> maxReadsForBlob;
    bool doPop = true;
};

} // namespace SP