#pragma once
#include "TaskState.hpp"
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <future>

namespace SP {
struct PathSpace;

// Options for asynchronous task execution
struct TaskAsyncOptions {
    std::any resultStorage;                             // Storage for the result when using a timeout on a ReadExtract execution
    std::shared_ptr<std::future<void>> executionFuture; // Future for the asynchronous execution
    void* resultPtr = nullptr;                          // Pointer to the result
    std::function<void(void const* const from, void* const to)> resultCopy; // Function to copy the result
};

// Represents a task to be executed within PathSpace, possibly via TaskPool
struct Task {
    TaskStateAtomic state;                       // Atomic state of the task
    PathSpace* space = nullptr;                  // Pointer to a PathSpace where the return values from lazy executions will be inserted
    ConstructiblePath pathToInsertReturnValueTo; // Path where the return value will be inserted
    std::function<void(Task const& task, void* obj, bool const objIsData)> function; // Function to be executed by the task

    std::optional<ExecutionOptions> executionOptions; // Optional execution options for the task
    std::optional<TaskAsyncOptions> asyncTask;        // Optional asynchronous task options (Lazy+timeout)
};

} // namespace SP