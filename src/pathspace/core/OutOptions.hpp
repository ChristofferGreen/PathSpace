#pragma once
#include "BlockOptions.hpp"
#include "ExecutionOptions.hpp"

#include <optional>

namespace SP {

struct OutOptions {
    std::optional<ExecutionOptions> execution;
    std::optional<BlockOptions> block;
    std::optional<int> maxReadsForBlob;
    bool bypassCache{false}; // New option to bypass cache for specific operations
};

} // namespace SP