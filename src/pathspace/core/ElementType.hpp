#pragma once

#include <typeinfo>

namespace SP {

struct ElementType {
    enum class DataCategory {
        SerializedData,
        FunctionPointer,
        StdFunction
    };
    std::type_info const* const typeInfo = nullptr;
    uint32_t elements = 0;
    DataCategory category = DataCategory::SerializedData;
};

} // namespace SP