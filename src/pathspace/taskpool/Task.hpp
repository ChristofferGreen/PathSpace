#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include <cassert>
#include <functional>
#include <variant>

namespace SP {
struct PathSpace;

struct Task {
    void* userSuppliedFunctionPointer = nullptr; // Function pointer inserted by the user (it has no arguments).
    PathSpace* space = nullptr;                  // Returned values from the execution will be inserted here
    ConstructiblePath pathToInsertReturnValueTo; // On this path, the return value will be inserted.
    ExecutionOptions executionOptions;

    void (*taskExecutorFunctionPointer)(Task const& task) = nullptr;
    std::function<void(Task const& task, void* obj)> taskExecutorStdFunction;
};

} // namespace SP