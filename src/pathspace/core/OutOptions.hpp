#pragma once
#include "BlockOptions.hpp"
#include "Capabilities.hpp"
#include "ExecutionOptions.hpp"

#include <optional>

namespace SP {

struct OutOptions {
    std::optional<Capabilities> capabilities;
    std::optional<ExecutionOptions> execution;
    std::optional<BlockOptions> block;
    std::optional<int> maxReadsForBlob;
    bool doPop = true;
};

} // namespace SP