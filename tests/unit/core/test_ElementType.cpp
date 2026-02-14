#include "core/ElementType.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("core.element_type") {
TEST_CASE("ElementType defaults to serialized data with zero elements") {
    ElementType element{};
    CHECK(element.typeInfo == nullptr);
    CHECK(element.elements == 0u);
    CHECK(element.category == DataCategory::SerializedData);
}

TEST_CASE("ElementType captures type info and element count") {
    ElementType element{};
    element.typeInfo = &typeid(int);
    element.elements = 4;
    element.category = DataCategory::Fundamental;

    CHECK(element.typeInfo == &typeid(int));
    CHECK(element.elements == 4u);
    CHECK(element.category == DataCategory::Fundamental);
}
}
