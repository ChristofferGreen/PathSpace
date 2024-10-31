#pragma once
#include "TaskState.hpp"
#include "core/ExecutionOptions.hpp"
#include "path/ConcretePath.hpp"
#include "path/ConstructiblePath.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <future>

namespace SP {
struct PathSpace;

// Represents a task to be executed within PathSpace, possibly via TaskPool
struct Task {
    TaskStateAtomic state;      // Atomic state of the task
    PathSpace* space = nullptr; // Pointer to a PathSpace where the return values from lazy executions will be inserted
    std::function<void(Task& task, bool const objIsData)> function; // Function to be executed by the task
    ConcretePathString notificationPath;
    std::shared_ptr<std::future<void>> executionFuture;                   // Future for the asynchronous execution
    std::function<void(std::any const& from, void* const to)> resultCopy; // Function to copy the result
    std::any result;                                                      // Result of the task execution

    std::optional<ExecutionOptions> executionOptions; // Optional execution options for the task
};

} // namespace SP