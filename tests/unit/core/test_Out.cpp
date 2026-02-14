#include "core/Out.hpp"
#include "third_party/doctest.h"

#include <chrono>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("core.out") {

TEST_CASE("operator& applies modifiers without mutating the left operand") {
    Out base{};
    base.doBlock   = true;
    base.timeout   = 250ms;
    base.isMinimal = false;

    struct TogglePop {
        void modify(Out& out) const { out.doPop = true; }
    };

    auto combined = base & TogglePop{};

    CHECK(base.doPop == false);          // original untouched
    CHECK(combined.doBlock == true);     // preserved from base
    CHECK(combined.doPop == true);       // applied by modifier
    CHECK(combined.timeout == 250ms);    // copied from base
}

TEST_CASE("Out modifiers compose via operator&") {
    Out start{};

    auto chained = start & Block{5ms} & Minimal{} & OutFullValidation{};

    CHECK(chained.doBlock);
    CHECK(chained.timeout == 5ms);
    CHECK(chained.isMinimal);
    CHECK(chained.validationLevel == ValidationLevel::Full);
}

TEST_CASE("Out defaults and remaining modifiers are exercised") {
    Out base{};
    CHECK_FALSE(base.doBlock);
    CHECK_FALSE(base.doPop);
    CHECK_FALSE(base.isMinimal);
    CHECK(base.timeout == DEFAULT_TIMEOUT);
    CHECK(base.validationLevel == ValidationLevel::Basic);

    auto withPop = base & Pop{};
    CHECK(withPop.doPop);
    CHECK(withPop.timeout == DEFAULT_TIMEOUT);

    auto noValidation = base & OutNoValidation{};
    CHECK(noValidation.validationLevel == ValidationLevel::None);

    auto fullValidation = base & OutFullValidation{};
    CHECK(fullValidation.validationLevel == ValidationLevel::Full);

    Block defaultBlock{};
    CHECK(defaultBlock.doBlock);
    CHECK(defaultBlock.timeout == DEFAULT_TIMEOUT);
}

} // TEST_SUITE
