#pragma once
#include "BlockOptions.hpp"
#include "ExecutionOptions.hpp"
#include "path/validation.hpp"

#include <optional>

namespace SP {

struct OutOptions {
    std::optional<ExecutionOptions> execution;
    std::optional<BlockOptions>     block;
    ValidationLevel                 validationLevel = ValidationLevel::Basic;
};

} // namespace SP