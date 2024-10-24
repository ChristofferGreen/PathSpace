#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <cassert>
#include <functional>

namespace SP {
struct PathSpace;
struct TaskToken;

struct Task {
    PathSpace* space = nullptr;                  // Returned values from the execution will be inserted here
    TaskToken* token = nullptr;                  // Token for validation
    ConstructiblePath pathToInsertReturnValueTo; // On this path, the return value will be inserted.
    ExecutionOptions executionOptions;

    std::function<void(Task const& task, void* obj, bool isOut)> function;
};

} // namespace SP