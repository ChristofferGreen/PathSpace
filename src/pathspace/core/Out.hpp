#pragma once
#include "ExecutionCategory.hpp"
#include "path/validation.hpp"

#include <chrono>
#include <concepts>

namespace SP {
using namespace std::chrono_literals;

static constexpr auto DEFAULT_TIMEOUT = 876600h; // 100 years

struct Out {
    bool                      doBlock         = false;
    bool                      doPop           = false;
    std::chrono::milliseconds timeout         = DEFAULT_TIMEOUT;
    ValidationLevel           validationLevel = ValidationLevel::Basic;

    template <typename T>
        requires requires(T const& t, Out& o) { t.modify(o); }
    friend auto operator&(Out const& lhs, T const& rhs) -> Out {
        Out o = lhs;
        rhs.modify(o);
        return o;
    }
};

struct Block : Out {
    explicit Block(std::chrono::milliseconds const& timeout = DEFAULT_TIMEOUT) {
        this->doBlock = true;
        this->timeout = timeout;
    }

    void modify(Out& o) const {
        o.doBlock = true;
        o.timeout = timeout;
    }
};

struct Pop : Out {
    Pop() {
        this->doPop = true;
    }

    void modify(Out& o) const {
        o.doPop = true;
    }
};

struct OutNoValidation : Out {
    OutNoValidation() {
        this->validationLevel = ValidationLevel::None;
    }

    void modify(Out& o) const {
        o.validationLevel = ValidationLevel::None;
    }
};

struct OutFullValidation : Out {
    OutFullValidation() {
        this->validationLevel = ValidationLevel::Full;
    }

    void modify(Out& o) const {
        o.validationLevel = ValidationLevel::Full;
    }
};

} // namespace SP