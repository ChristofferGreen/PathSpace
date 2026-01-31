#include "core/In.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("core.in") {

TEST_CASE("default In uses expected flags") {
    In defaults{};
    CHECK(defaults.executionCategory == ExecutionCategory::Unknown);
    CHECK(defaults.validationLevel == ValidationLevel::Basic);
    CHECK(defaults.replaceExistingPayload == false);
}

TEST_CASE("operator& composes modifiers without mutating the left operand") {
    In base{};
    base.executionCategory    = ExecutionCategory::Lazy;
    base.validationLevel      = ValidationLevel::Full;
    base.replaceExistingPayload = false;

    struct ToggleReplace {
        void modify(In& in) const {
            in.replaceExistingPayload = true;
            in.validationLevel        = ValidationLevel::None;
        }
    };

    auto combined = base & ToggleReplace{};

    CHECK(base.replaceExistingPayload == false); // base remains unchanged
    CHECK(base.validationLevel == ValidationLevel::Full);

    CHECK(combined.executionCategory == ExecutionCategory::Lazy); // preserved
    CHECK(combined.validationLevel == ValidationLevel::None);     // overridden
    CHECK(combined.replaceExistingPayload == true);
}

TEST_CASE("modifiers set the advertised execution and validation semantics") {
    auto immediate = In{} & Immediate{};
    CHECK(immediate.executionCategory == ExecutionCategory::Immediate);
    CHECK(immediate.validationLevel == ValidationLevel::Basic);
    CHECK_FALSE(immediate.replaceExistingPayload);

    auto lazyFullReplace = In{} & Lazy{} & InFullValidation{} & ReplaceExisting{};
    CHECK(lazyFullReplace.executionCategory == ExecutionCategory::Lazy);
    CHECK(lazyFullReplace.validationLevel == ValidationLevel::Full);
    CHECK(lazyFullReplace.replaceExistingPayload);

    auto noneValidation = In{} & InFullValidation{} & InNoValidation{};
    CHECK(noneValidation.validationLevel == ValidationLevel::None);

    auto basicValidation = In{} & InNoValidation{} & InBasicValidation{};
    CHECK(basicValidation.validationLevel == ValidationLevel::Basic);
}

}
