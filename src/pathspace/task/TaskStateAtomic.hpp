#pragma once
#include "TaskState.hpp"

#include <atomic>
#include <string_view>

namespace SP {

// Thread-safe wrapper for managing task state transitions
struct TaskStateAtomic {
    TaskStateAtomic() = default;                              // Default constructor initializes to NotStarted
    TaskStateAtomic(const TaskStateAtomic& other);            // Copy constructor takes a snapshot of the other state
    TaskStateAtomic& operator=(const TaskStateAtomic& other); // Copy assignment takes a snapshot of the other state

    // Move operations are deleted because std::atomic is non-movable
    TaskStateAtomic(TaskStateAtomic&& other)            = delete;
    TaskStateAtomic& operator=(TaskStateAtomic&& other) = delete;

    bool tryStart();            // Attempts to transition from NotStarted to Starting. Returns true if successful, false if already started
    bool transitionToRunning(); // Attempts to transition from Starting to Running. Returns true if successful, false if not in Starting state
    bool markCompleted();       // Attempts to transition from Running to Completed. Returns true if successful, false if not in Running state
    bool markFailed();          // Marks task as failed unless it's already completed. Returns true if state was changed to Failed, false if already Completed
    bool isTerminal() const;    // Check if task is in a terminal state (Completed or Failed)
    bool hasStarted() const;    // Check if task has started (any state except NotStarted)
    bool isCompleted() const;   // Check if task completed successfully
    bool isFailed() const;      // Check if task failed
    bool isRunning() const;     // Check if task is currently running

    std::string_view toString() const; // Get string representation of current state

private:
    TaskState get() const; // Get current state with acquire semantics

    std::atomic<TaskState> state{TaskState::NotStarted}; // The underlying atomic state storage
};

} // namespace SP