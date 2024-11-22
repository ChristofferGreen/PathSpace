#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

namespace SP {

struct In {
    ExecutionCategory executionCategory = ExecutionCategory::Unknown;
    ValidationLevel   validationLevel   = ValidationLevel::Basic;
};

} // namespace SP