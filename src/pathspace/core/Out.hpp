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

    // private:
    ValidationLevel           validationLevel = ValidationLevel::Basic;
    bool                      block_          = false;
    std::chrono::milliseconds timeout         = 876600h; // 100 years
};

} // namespace SP