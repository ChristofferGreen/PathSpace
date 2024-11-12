#pragma once
#include "BlockOptions.hpp"
#include "ExecutionOptions.hpp"

#include <optional>

namespace SP {

struct InOptions {
    std::optional<ExecutionOptions> execution;
    std::optional<BlockOptions>     block;
};

} // namespace SP