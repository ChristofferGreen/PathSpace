#pragma once
#include <typeinfo>

namespace SP {

enum struct ExecutionCategory {
    None = 0,
    FunctionPointer,
    StdFunction
};

} // namespace SP