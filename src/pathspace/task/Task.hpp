#pragma once
#include "TaskStateAtomic.hpp"
#include "core/ExecutionCategory.hpp"
#include "log/TaggedLogger.hpp"
#include "type/InputData.hpp"
#include "core/NotificationSink.hpp"
#include "task/Executor.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace SP {
struct PathSpaceBase;

struct Task {
    template <typename FunctionType>
    static auto Create(FunctionType&& fun) -> std::shared_ptr<Task> {
        auto task      = std::shared_ptr<Task>(new Task{});
        task->function = fun;
        return task;
    }
    template <typename DataType>
    static auto Create(PathSpaceBase* space, std::string_view const& notificationPath, DataType const& userFunction, ExecutionCategory const& inExecutionCategory) -> std::shared_ptr<Task> {
        sp_log("Task::Create", "Function Called");

        // For any callable type (lambda, function pointer, etc)
        if constexpr (requires { typename std::invoke_result_t<DataType>; }) {
            using ResultType = std::invoke_result_t<DataType>;

            auto task               = std::shared_ptr<Task>(new Task{});
            task->space             = space;
            task->notificationPath  = notificationPath;
            task->executionCategory = (inExecutionCategory == ExecutionCategory::Unknown) ? ExecutionCategory::Immediate : inExecutionCategory;
            task->function          = [fun = userFunction](Task& task, bool const) {
                sp_log("Task lambda execution", "Task");
                task.result = fun();
                sp_log("Task lambda completed", "Task");
            };
            task->resultCopyFn = [](std::any const& from, void* const to) {
                sp_log("Task copying result", "Task");
                *static_cast<ResultType*>(to) = std::any_cast<ResultType>(from);
            };

            return task;
        } else {
            sp_log("Task::Create - Invalid callable type", "ERROR");
            return nullptr;
        }
    }

    template <typename DataType>
    static auto Create(std::weak_ptr<NotificationSink> notifier, std::string_view const& notificationPath, DataType const& userFunction, ExecutionCategory const& inExecutionCategory) -> std::shared_ptr<Task> {
        sp_log("Task::Create", "Function Called");

        // For any callable type (lambda, function pointer, etc)
        if constexpr (requires { typename std::invoke_result_t<DataType>; }) {
            using ResultType = std::invoke_result_t<DataType>;

            auto task               = std::shared_ptr<Task>(new Task{});
            task->notifier          = std::move(notifier);
            task->notificationPath  = notificationPath;
            task->executionCategory = (inExecutionCategory == ExecutionCategory::Unknown) ? ExecutionCategory::Immediate : inExecutionCategory;
            task->function          = [fun = userFunction](Task& task, bool const) {
                sp_log("Task lambda execution", "Task");
                task.result = fun();
                sp_log("Task lambda completed", "Task");
            };
            task->resultCopyFn = [](std::any const& from, void* const to) {
                sp_log("Task copying result", "Task");
                *static_cast<ResultType*>(to) = std::any_cast<ResultType>(from);
            };

            return task;
        } else {
            sp_log("Task::Create - Invalid callable type", "ERROR");
            return nullptr;
        }
    }

    auto isCompleted() const -> bool;
    auto hasStarted() const -> bool;
    auto tryStart() -> bool;
    auto transitionToRunning() -> bool;
    auto markCompleted() -> void;
    auto markFailed() -> void;
    auto category() const -> ExecutionCategory;
    auto resultCopy(void* dest) -> void {
        // Wait for result to be ready before copying
        while (!this->state.isCompleted())
            std::this_thread::yield();
        resultCopyFn(result, dest);
    }

    // Set preferred executor for (re)submission
    auto setExecutor(Executor* exec) -> void {
        this->executor = exec;
    }

    auto setLabel(std::string label) -> void {
        this->label = std::move(label);
    }

    auto getLabel() const -> std::string const& {
        return this->label;
    }

private:
    friend class TaskPool;
    friend struct NodeData; // Allow NodeData to access Task internals for scheduling

    Task()                       = default; // Private constructor - use Create()
    Task(const Task&)            = delete;  // Non-copyable
    Task& operator=(const Task&) = delete;  // Non-copyable
    Task(Task&&)                 = delete;  // Non-movable (since we use shared_ptr)
    Task& operator=(Task&&)      = delete;  // Non-movable (since we use shared_ptr)

    PathSpaceBase*                                            space = nullptr;   // Pointer to a PathSpace where the return values from lazy executions will be inserted
    TaskStateAtomic                                           state;             // Atomic state of the task
    std::function<void(Task& task, bool const objIsData)>     function;          // Function to be executed by the task
    std::function<void(std::any const& from, void* const to)> resultCopyFn;       // Function to copy the result
    std::any                                                  result;            // Result of the task execution
    ExecutionCategory                                         executionCategory; // Optional execution options for the task
    std::weak_ptr<NotificationSink>                           notifier;          // Weak reference to notification sink for lifetime-safe notifications
    std::string                                               notificationPath;
    Executor*                                                 executor = nullptr; // Optional preferred executor for (re)submission
    std::string                                               label;
};

} // namespace SP
