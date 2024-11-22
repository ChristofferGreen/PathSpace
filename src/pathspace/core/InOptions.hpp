#pragma once
#include "ExecutionOptions.hpp"
#include "path/validation.hpp"

#include <optional>

namespace SP {

struct InOptions {
    std::optional<ExecutionOptions> execution;
    ValidationLevel                 validationLevel = ValidationLevel::Basic;
};

} // namespace SP