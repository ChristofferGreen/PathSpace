#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include <functional>
#include <variant>

namespace SP {
using FunctionPointerTask = void (*)(void* const functionPointer);

struct Task {
    std::variant<std::function<void()>, FunctionPointerTask> callable; // FunctionPointerTask is a wrapper
    void* functionPointer = nullptr;                                   // User function
    ConstructiblePath path;
    ExecutionOptions executionOptions;
};

} // namespace SP