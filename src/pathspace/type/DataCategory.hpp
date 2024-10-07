#pragma once
#include <typeinfo>

namespace SP {

enum struct DataCategory {
    None = 0,
    SerializedData,
    ExecutionFunctionPointer,
    FunctionPointer,
    ExecutionStdFunction
};

} // namespace SP