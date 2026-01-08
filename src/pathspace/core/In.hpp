#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <concepts>

namespace SP {

struct In {
    ExecutionCategory executionCategory = ExecutionCategory::Unknown;
    ValidationLevel   validationLevel   = ValidationLevel::Basic;

    template <typename T>
        requires requires(T const& t, In& i) { t.modify(i); }
    friend auto operator&(In const& lhs, T const& rhs) -> In {
        In i = lhs;
        rhs.modify(i);
        return i;
    }
};

struct Immediate : In {
    Immediate() {
        this->executionCategory = ExecutionCategory::Immediate;
    }

    void modify(In& i) const {
        i.executionCategory = ExecutionCategory::Immediate;
    }
};

struct Lazy : In {
    Lazy() {
        this->executionCategory = ExecutionCategory::Lazy;
    }

    void modify(In& i) const {
        i.executionCategory = ExecutionCategory::Lazy;
    }
};

struct InBasicValidation : In {
    InBasicValidation() {
        this->validationLevel = ValidationLevel::Basic;
    }

    void modify(In& i) const {
        i.validationLevel = ValidationLevel::Basic;
    }
};

struct InNoValidation : In {
    InNoValidation() {
        this->validationLevel = ValidationLevel::None;
    }

    void modify(In& i) const {
        i.validationLevel = ValidationLevel::None;
    }
};

struct InFullValidation : In {
    InFullValidation() {
        this->validationLevel = ValidationLevel::Full;
    }

    void modify(In& i) const {
        i.validationLevel = ValidationLevel::Full;
    }
};

} // namespace SP
