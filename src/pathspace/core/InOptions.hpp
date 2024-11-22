#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

namespace SP {

struct InOptions {
    ExecutionCategory executionCategory = ExecutionCategory::Unknown;
    ValidationLevel   validationLevel   = ValidationLevel::Basic;
};

} // namespace SP