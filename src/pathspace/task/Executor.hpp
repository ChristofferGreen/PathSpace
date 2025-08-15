#pragma once

#include "core/Error.hpp"

#include <memory>
#include <optional>

namespace SP {

struct Task;

/**
 * Executor — interface for scheduling and executing Tasks
 *
 * Rationale
 * ---------
 * This interface decouples task orchestration from storage and I/O layers.
 * Implementations (e.g., a thread pool) are responsible for:
 *  - accepting tasks for execution
 *  - running the task's callable
 *  - updating task state (started/running/completed/failed)
 *  - performing any post-completion notification (e.g., via Task internals)
 *
 * Contract
 * --------
 * - submit(...) returns std::nullopt on success, or an Error on refusal
 *   (e.g., executor shutting down, backpressure limits exceeded).
 * - shutdown() initiates a graceful shutdown of the executor; it should:
 *     - stop accepting new tasks
 *     - wake workers
 *     - allow in-flight tasks to finish if possible
 * - size() returns an implementation-defined measure of capacity
 *   (e.g., number of worker threads).
 *
 * Thread-safety
 * -------------
 * Implementations must be thread-safe for concurrent submit() calls and
 * for shutdown() to be called while tasks may still be in flight.
 */
struct Executor {
    virtual ~Executor() = default;

    // Primary submission API — accepts a weak reference to decouple lifetime.
    // Implementations should attempt to lock the weak_ptr before scheduling.
    virtual auto submit(std::weak_ptr<Task>&&) -> std::optional<Error> = 0;

    // Convenience overload — accepts a shared_ptr and forwards to the primary API.
    auto submit(std::shared_ptr<Task> const& task) -> std::optional<Error> {
        return submit(std::weak_ptr<Task>(task));
    }

    // Initiate shutdown (graceful if possible).
    virtual auto shutdown() -> void = 0;

    // Implementation-defined capacity/size (e.g., number of workers).
    virtual auto size() const -> size_t = 0;
};

} // namespace SP