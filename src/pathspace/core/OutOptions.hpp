#pragma once
#include "BlockOptions.hpp"
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <optional>

namespace SP {

struct OutOptions {
    ExecutionCategory           executionCategory = ExecutionCategory::Unknown;
    std::optional<BlockOptions> block;
    ValidationLevel             validationLevel = ValidationLevel::Basic;
};

} // namespace SP