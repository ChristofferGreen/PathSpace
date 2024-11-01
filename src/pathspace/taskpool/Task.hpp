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
        sp_log("PathSpace::createTask", "Function Called");
        if constexpr (ExecutionFunctionPointer<DataType> || ExecutionStdFunction<DataType>) {
            return std::make_shared<Task>(Task{
                    .space = space,
                    .function
                    = [userFunctionOrData = std::move(data)](Task& task, bool const objIsData) { task.result = userFunctionOrData(); },
                    .notificationPath = notificationPath,
                    .resultCopy =
                            [](std::any const& from, void* const to) {
                                *static_cast<std::invoke_result_t<DataType>*>(to) = *std::any_cast<std::invoke_result_t<DataType>>(&from);
                            },
                    .executionOptions = options.execution});
        }
        return {};
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