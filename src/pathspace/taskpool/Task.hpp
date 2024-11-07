#pragma once
#include "TaskState.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/InOptions.hpp"
#include "path/ConcretePath.hpp"
#include "type/InputData.hpp"
#include "utils/TaggedLogger.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <future>

namespace SP {
struct PathSpace;

// Represents a task to be executed within PathSpace, possibly via TaskPool
struct Task {
    template <typename DataType>
    static auto Create(PathSpace* space,
                       ConcretePathString const& notificationPath,
                       DataType const& data,
                       InputData const& inputData,
                       InOptions const& options) -> std::shared_ptr<Task> {
        sp_log("Task::Create", "Function Called");

        // For any callable type (lambda, function pointer, etc)
        if constexpr (requires { typename std::invoke_result_t<DataType>; }) {
            using ResultType = std::invoke_result_t<DataType>;

            auto task = std::shared_ptr<Task>(new Task{});
            task->space = space;
            task->notificationPath = notificationPath;
            task->executionOptions = options.execution;
            task->function = [userFunction = data](Task& task, bool const) {
                sp_log("Task lambda execution", "DEBUG");
                task.result = userFunction();
                sp_log("Task lambda completed", "DEBUG");
            };
            task->resultCopy = [](std::any const& from, void* const to) {
                sp_log("Task copying result", "DEBUG");
                *static_cast<ResultType*>(to) = std::any_cast<ResultType>(from);
            };

            return task;
        } else {
            sp_log("Task::Create - Invalid callable type", "ERROR");
            return nullptr;
        }
    }

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