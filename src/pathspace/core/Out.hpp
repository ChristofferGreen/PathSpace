#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace SP {

struct Out {
    friend auto operator&(Out const& lhs, Out const& rhs) -> Out {
        Out result;
        result.doPop           = lhs.doPop || rhs.doPop;
        result.doBlock         = lhs.doBlock || rhs.doBlock;
        result.timeout         = result.doBlock ? std::min(lhs.timeout, rhs.timeout) : 876600h;
        result.validationLevel = std::max(lhs.validationLevel, rhs.validationLevel);
        return result;
    }

    ValidationLevel           validationLevel = ValidationLevel::Basic;
    bool                      doBlock         = false;
    std::chrono::milliseconds timeout         = 876600h; // 100 years
    bool                      doPop           = false;
};

struct Block : Out {
    Block(std::chrono::milliseconds const& timeout = 876600h)
        : Out{.doBlock = true, .timeout = timeout} {}
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

struct Pop : Out {
    Pop() {
        this->doPop = true;
    }
};

} // namespace SP