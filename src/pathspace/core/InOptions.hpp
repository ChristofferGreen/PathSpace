#pragma once
#include "BlockOptions.hpp"
#include "ExecutionOptions.hpp"
#include "TimeToLive.hpp"

#include <optional>

namespace SP {

struct InOptions {
    std::optional<TimeToLive> ttl;
    std::optional<ExecutionOptions> execution;
    std::optional<BlockOptions> block;
    std::optional<int> maxInsertionsForBlob;
    bool createDirectoriesAlongPath = true;
};

} // namespace SP