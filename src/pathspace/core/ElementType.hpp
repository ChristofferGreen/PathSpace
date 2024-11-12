#pragma once
#include "type/DataCategory.hpp"

#include <stdint.h>
#include <typeinfo>

namespace SP {

struct ElementType {
    std::type_info const* typeInfo = nullptr;
    uint32_t              elements = 0;
    DataCategory          category = DataCategory::SerializedData;
};

} // namespace SP