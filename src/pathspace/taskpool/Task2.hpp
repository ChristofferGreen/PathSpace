#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>

namespace SP {
struct PathSpace;
struct TaskToken;

struct Task2 {
    PathSpace* space = nullptr;                  // Returned values from the execution will be inserted here
    ConstructiblePath pathToInsertReturnValueTo; // On this path, the return value will be inserted.
    ExecutionOptions executionOptions;

    std::function<void(Task2 const& task, void* obj, bool isOut)> function;
};

} // namespace SP