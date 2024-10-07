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

    void (*taskExecutor)(Task const& task) = nullptr;
};
/*
template <typename T>
auto task_executor(Task const& task) -> void {
    assert(task.space);
    if (task.userSuppliedFunctionPointer != nullptr) {
        task.space->insert(task.pathToInsertReturnValueTo, reinterpret_cast<T (*)()>(task.userSuppliedFunctionPointer)());
    }
}
*/
} // namespace SP