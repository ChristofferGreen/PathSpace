#include "type/DataCategory.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("type.data_category") {
TEST_CASE("DataCategory enumerators preserve expected ordering") {
    CHECK(static_cast<int>(DataCategory::None) == 0);
    CHECK(static_cast<int>(DataCategory::SerializedData) == 1);
    CHECK(static_cast<int>(DataCategory::Execution) == 2);
    CHECK(static_cast<int>(DataCategory::FunctionPointer) == 3);
    CHECK(static_cast<int>(DataCategory::Fundamental) == 4);
    CHECK(static_cast<int>(DataCategory::SerializationLibraryCompatible) == 5);
    CHECK(static_cast<int>(DataCategory::UniquePtr) == 6);
}
}
