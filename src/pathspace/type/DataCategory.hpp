#pragma once
#include <typeinfo>

namespace SP {

enum struct DataCategory {
    None = 0,
    SerializedData,
    Execution,
    FunctionPointer,
    Fundamental,
    SerializationLibraryCompatible
};

} // namespace SP