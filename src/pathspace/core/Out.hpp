#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace SP {

struct Out {
    friend auto operator&(Out const& lhs, Out const& rhs) -> Out {
        Out result;
        result.block_          = lhs.block_ || rhs.block_;
        result.timeout         = result.block_ ? std::min(lhs.timeout, rhs.timeout) : 876600h;
        result.validationLevel = std::max(lhs.validationLevel, rhs.validationLevel);
        result.doExtract       = lhs.doExtract || rhs.doExtract;
        return result;
    }

    ValidationLevel           validationLevel = ValidationLevel::Basic;
    bool                      block_          = false;
    std::chrono::milliseconds timeout         = 876600h; // 100 years
    bool                      doExtract       = false;
};

struct Block : Out {
    Block(std::chrono::milliseconds const& timeout = 876600h)
        : Out{.block_ = true, .timeout = timeout} {}
};

struct OutNoValidation : Out {
    OutNoValidation() {
        this->validationLevel = ValidationLevel::None;
    }
};

struct OutFullValidation : Out {
    OutFullValidation() {
        this->validationLevel = ValidationLevel::Full;
    }
};

struct Extract : Out {
    Extract() {
        this->doExtract = true;
    }
};

} // namespace SP