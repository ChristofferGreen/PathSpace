#pragma once
#include <typeinfo>

namespace SP {

enum struct DataCategory {
    None = 0,
    SerializedData,
    ExecutionFunctionPointer,
    FunctionPointer,
    ExecutionStdFunction,
    Fundamental,
    SerializationLibraryCompatible
};

} // namespace SP