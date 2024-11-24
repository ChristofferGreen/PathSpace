#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

namespace SP {

struct In {
    friend auto operator&(In const& lhs, In const& rhs) -> In {
        In result;
        result.executionCategory = (rhs.executionCategory != ExecutionCategory::Unknown) ? rhs.executionCategory : lhs.executionCategory;
        result.validationLevel   = std::max(lhs.validationLevel, rhs.validationLevel);
        return result;
    }

    ExecutionCategory executionCategory = ExecutionCategory::Unknown;
    ValidationLevel   validationLevel   = ValidationLevel::Basic;
};

struct Immediate : In {
    Immediate() {
        executionCategory = ExecutionCategory::Immediate;
    }
};

struct Lazy : In {
    Lazy() {
        executionCategory = ExecutionCategory::Lazy;
    }
};

struct InNoValidation : In {
    InNoValidation() {
        validationLevel = ValidationLevel::None;
    }
};

struct InFullValidation : In {
    InFullValidation() {
        validationLevel = ValidationLevel::Full;
    }
};

} // namespace SP