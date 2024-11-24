#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

namespace SP {

struct In {
    // Static creation methods for execution categories
    static auto Immediate() -> In {
        return In{.executionCategory = ExecutionCategory::Immediate};
    }

    static auto Lazy() -> In {
        return In{.executionCategory = ExecutionCategory::Lazy};
    }

    // Static creation methods for validation levels
    static auto FullValidation() -> In {
        return In{.validationLevel = ValidationLevel::Full};
    }

    static auto NoValidation() -> In {
        return In{.validationLevel = ValidationLevel::None};
    }

    // Chainable methods for execution category
    auto immediate() -> In& {
        executionCategory = ExecutionCategory::Immediate;
        return *this;
    }

    auto lazy() -> In& {
        executionCategory = ExecutionCategory::Lazy;
        return *this;
    }

    // Chainable methods for validation
    auto validateFull() -> In& {
        validationLevel = ValidationLevel::Full;
        return *this;
    }

    auto validateNone() -> In& {
        validationLevel = ValidationLevel::None;
        return *this;
    }

    // Combine configurations
    friend auto operator&(In const& lhs, In const& rhs) -> In {
        In result;
        result.executionCategory = (rhs.executionCategory != ExecutionCategory::Unknown) ? rhs.executionCategory : lhs.executionCategory;
        result.validationLevel   = std::max(lhs.validationLevel, rhs.validationLevel);
        return result;
    }

    ExecutionCategory executionCategory = ExecutionCategory::Unknown;
    ValidationLevel   validationLevel   = ValidationLevel::Basic;
};

// Helper types for static-style configuration
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