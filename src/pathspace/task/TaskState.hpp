#pragma once
#include <atomic>
#include <string_view>

namespace SP {

// Represents the possible states of a task
enum class TaskState {
    NotStarted, // Initial state when task is created
    Starting,   // Task is being prepared for execution
    Running,    // Task is actively executing
    Completed,  // Task finished successfully
    Failed      // Task encountered an error during execution
};

// Convert TaskState to string for debugging/logging
constexpr std::string_view taskStateToString(TaskState state);

// Thread-safe wrapper for managing task state transitions
struct TaskStateAtomic {
    TaskStateAtomic() = default;                              // Default constructor initializes to NotStarted
    TaskStateAtomic(const TaskStateAtomic& other);            // Copy constructor takes a snapshot of the other state
    TaskStateAtomic& operator=(const TaskStateAtomic& other); // Copy assignment takes a snapshot of the other state

    // Allow move operations
    TaskStateAtomic(TaskStateAtomic&& other)            = default;
    TaskStateAtomic& operator=(TaskStateAtomic&& other) = default;

    bool tryStart();            // Attempts to transition from NotStarted to Starting. Returns true if successful, false if already started
    bool transitionToRunning(); // Attempts to transition from Starting to Running. Returns true if successful, false if not in Starting state
    bool markCompleted();       // Attempts to transition from Running to Completed. Returns true if successful, false if not in Running state
    bool markFailed();          // Marks task as failed unless it's already completed. Returns true if state was changed to Failed, false if already Completed
    bool isTerminal() const;    // Check if task is in a terminal state (Completed or Failed)
    bool hasStarted() const;    // Check if task has started (any state except NotStarted)
    bool isCompleted() const;   // Check if task completed successfully
    bool isFailed() const;      // Check if task failed
    bool isRunning() const;     // Check if task is currently running

    TaskState get() const; // Get current state with acquire semantics

    std::string_view toString() const; // Get string representation of current state

private:
    std::atomic<TaskState> state{TaskState::NotStarted}; // The underlying atomic state storage
};

} // namespace SP