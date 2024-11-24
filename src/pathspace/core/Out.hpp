#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace SP {

struct Out {
    static auto Block(std::chrono::milliseconds const& timeout = 876600h) -> Out {
        return Out{.block_ = true, .timeout = timeout};
    }

    static auto FullValidation() -> Out {
        return Out{.validationLevel = ValidationLevel::Full};
    }

    static auto NoValidation() -> Out {
        return Out{.validationLevel = ValidationLevel::None};
    }

    auto block(std::chrono::milliseconds const& timeout = 876600h) -> Out& {
        this->block_  = true;
        this->timeout = timeout;
        return *this;
    }

    auto validateFull() -> Out& {
        this->validationLevel = ValidationLevel::Full;
        return *this;
    }

    auto validateNone() -> Out& {
        this->validationLevel = ValidationLevel::None;
        return *this;
    }

    friend auto operator&(Out const& lhs, Out const& rhs) -> Out {
        Out result;
        result.block_          = lhs.block_ || rhs.block_;
        result.timeout         = result.block_ ? std::min(lhs.timeout, rhs.timeout) : 876600h;
        result.validationLevel = std::max(lhs.validationLevel, rhs.validationLevel);
        return result;
    }

    // private:
    ValidationLevel           validationLevel = ValidationLevel::Basic;
    bool                      block_          = false;
    std::chrono::milliseconds timeout         = 876600h; // 100 years
};

struct Block : Out {
    Block(std::chrono::milliseconds const& timeout = 876600h)
        : Out{.block_ = true, .timeout = timeout} {}
};

struct NoValidation : Out {
    NoValidation() {
        this->validationLevel = ValidationLevel::None;
    }
};

struct FullValidation : Out {
    FullValidation() {
        this->validationLevel = ValidationLevel::Full;
    }
};

} // namespace SP