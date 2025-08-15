#pragma once

#include "core/ExecutionCategory.hpp"
#include "core/NotificationSink.hpp"
#include "core/Error.hpp"
#include "task/Executor.hpp"
#include "task/Task.hpp"
#include "task/IFutureAny.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace SP {

/**
 * TaskT<T> — typed task scaffolding that fulfills a PromiseT<T> when executed.
 *
 * Design
 * ------
 * - Wraps the existing untyped Task to leverage current TaskPool/Executor plumbing
 *   and lifetime-safe NotificationSink integration.
 * - Internally creates a PromiseT<T>/FutureT<T> pair; the wrapped callable sets
 *   the promise value before returning. The untyped Task stores the same value in
 *   its std::any result for backward compatibility.
 * - Does not assume a global singleton executor; provide an Executor* at creation
 *   or call schedule(exec) explicitly.
 *
 * Typical usage
 * -------------
 *   auto task = TaskT<int>::Create(notifier, "/path", []{ return 42; },
 *                                  ExecutionCategory::Immediate, exec);
 *   auto fut  = task->future();
 *   auto err  = task->schedule(exec); // or rely on immediate category calling code
 *   // ... later ...
 *   int v = 0;
 *   fut.get(v); // v == 42
 */
template <typename T>
class TaskT : public std::enable_shared_from_this<TaskT<T>> {
public:
    using value_type = T;

    TaskT(TaskT const&)            = delete;
    TaskT& operator=(TaskT const&) = delete;

    TaskT(TaskT&&)            = delete;
    TaskT& operator=(TaskT&&) = delete;

    ~TaskT() = default;

    // Factory: create a typed task with notification support.
    template <typename F>
    static std::shared_ptr<TaskT<T>> Create(std::weak_ptr<NotificationSink> notifier,
                                            std::string notificationPath,
                                            F&&         func,
                                            ExecutionCategory category = ExecutionCategory::Immediate,
                                            Executor*   exec = nullptr) {
        auto self = std::shared_ptr<TaskT<T>>(new TaskT<T>());

        // Capture the shared state so we can fulfill the promise inside the task.
        auto shared_state = self->promise_.shared_state();

        // Wrap the user function to both compute the result and fulfill the promise.
        auto wrapped = [st = std::move(shared_state), fn = std::forward<F>(func)]() -> T {
            T value = fn();
            // Best-effort fulfill; if already set, ignore (first set wins).
            st->set_value(value);
            return value;
        };

        // Create underlying untyped Task that will execute the wrapped callable and notify.
        self->legacy_ = Task::Create(std::move(notifier), notificationPath, std::move(wrapped), category);

        // Prefer the provided executor for (re)submission.
        if (exec) {
            self->legacy_->setExecutor(exec);
        }

        return self;
    }

    // Factory: create a typed task without notifications (no NotificationSink).
    template <typename F>
    static std::shared_ptr<TaskT<T>> Create(F&& func,
                                            ExecutionCategory category = ExecutionCategory::Immediate,
                                            Executor*   exec = nullptr) {
        auto self = std::shared_ptr<TaskT<T>>(new TaskT<T>());
        auto shared_state = self->promise_.shared_state();

        auto wrapped = [st = std::move(shared_state), fn = std::forward<F>(func)]() -> T {
            T value = fn();
            st->set_value(value);
            return value;
        };

        // Create an untyped Task that only executes the wrapped callable; no notifications.
        self->legacy_ = Task::Create(std::move(wrapped));
        if (exec) {
            self->legacy_->setExecutor(exec);
        }
        // Note: ExecutionCategory is recorded in the untyped Task created via Task::Create(notifier, ...).
        // For the minimal factory we rely on explicit scheduling by the caller.

        return self;
    }

    // Schedule this task on the provided executor.
    // Returns std::nullopt on success, or an Error if executor refused submission.
    std::optional<Error> schedule(Executor* exec) {
        if (!legacy_) {
            return Error{Error::Code::UnknownError, "No underlying task present"};
        }
        if (!exec) {
            return Error{Error::Code::UnknownError, "No executor provided for scheduling"};
        }
        return exec->submit(legacy_);
    }

    // Access the typed Future<T> for this task.
    FutureT<T> future() const {
        return promise_.get_future();
    }

    // Access a type-erased future view (useful for uniform storage).
    FutureAny any_future() const {
        return future().to_any();
    }

    // Expose the underlying untyped Task for integration with existing systems.
    std::shared_ptr<Task> legacy_task() const {
        return legacy_;
    }

private:
    TaskT()
        : promise_()
        , legacy_(nullptr) {}

    PromiseT<T>              promise_;
    std::shared_ptr<Task>    legacy_;
};

} // namespace SP