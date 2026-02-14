#include "type/FunctionCategory.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("type.function_category") {
TEST_CASE("FunctionCategory enumerators preserve expected ordering") {
    CHECK(static_cast<int>(FunctionCategory::None) == 0);
    CHECK(static_cast<int>(FunctionCategory::FunctionPointer) == 1);
    CHECK(static_cast<int>(FunctionCategory::StdFunction) == 2);
}
}
