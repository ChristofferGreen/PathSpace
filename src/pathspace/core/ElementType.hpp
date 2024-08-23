#pragma once

#include "type/DataCategory.hpp"

#include <typeinfo>

namespace SP {

struct ElementType {
    std::type_info const* const typeInfo = nullptr;
    uint32_t elements = 0;
    DataCategory category = DataCategory::SerializedData;
};

} // namespace SP