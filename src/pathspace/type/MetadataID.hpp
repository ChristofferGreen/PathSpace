#pragma once
#include <typeinfo>

namespace SP {

enum struct MetadataID {
    None = 0,
    FunctionPointer,
    ExecutionFunctionPointer,
    PathLambda,
};

inline std::type_info const* to_type_info(MetadataID const &id) {
    return reinterpret_cast<std::type_info const*>(static_cast<std::uintptr_t>(static_cast<int>(id)));
}

} // namespace SP