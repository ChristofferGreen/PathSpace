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
constexpr std::string_view taskStateToString(TaskState state) {
    switch (state) {
        case TaskState::NotStarted:
            return "NotStarted";
        case TaskState::Starting:
            return "Starting";
        case TaskState::Running:
            return "Running";
        case TaskState::Completed:
            return "Completed";
        case TaskState::Failed:
            return "Failed";
        default:
            return "Unknown";
    }
}

// Thread-safe wrapper for managing task state transitions
struct TaskStateAtomic {
    // The underlying atomic state storage
    std::atomic<TaskState> state{TaskState::NotStarted};

    // Default constructor initializes to NotStarted
    TaskStateAtomic() = default;

    // Copy constructor takes a snapshot of the other state
    TaskStateAtomic(const TaskStateAtomic& other) : state(other.state.load(std::memory_order_acquire)) {
    }

    // Copy assignment takes a snapshot of the other state
    TaskStateAtomic& operator=(const TaskStateAtomic& other) {
        state.store(other.state.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }

    // Allow move operations
    TaskStateAtomic(TaskStateAtomic&& other) = default;
    TaskStateAtomic& operator=(TaskStateAtomic&& other) = default;

    // Attempts to transition from NotStarted to Starting
    // Returns true if successful, false if already started
    bool tryStart() {
        TaskState expected = TaskState::NotStarted;
        return state.compare_exchange_strong(expected, TaskState::Starting);
    }

    // Attempts to transition from Starting to Running
    // Returns true if successful, false if not in Starting state
    bool transitionToRunning() {
        TaskState expected = TaskState::Starting;
        return state.compare_exchange_strong(expected, TaskState::Running);
    }

    // Attempts to transition from Running to Completed
    // Returns true if successful, false if not in Running state
    bool markCompleted() {
        TaskState expected = TaskState::Running;
        return state.compare_exchange_strong(expected, TaskState::Completed);
    }

    // Marks task as failed unless it's already completed
    // Returns true if state was changed to Failed, false if already Completed
    bool markFailed() {
        TaskState current = state.load();
        if (current != TaskState::Completed) {
            state.store(TaskState::Failed);
            return true;
        }
        return false;
    }

    // Get current state with acquire semantics
    TaskState get() const {
        return state.load(std::memory_order_acquire);
    }

    // Check if task is in a terminal state (Completed or Failed)
    bool isTerminal() const {
        TaskState current = get();
        return current == TaskState::Completed || current == TaskState::Failed;
    }

    // Check if task has started (any state except NotStarted)
    bool hasStarted() const {
        return get() != TaskState::NotStarted;
    }

    // Check if task completed successfully
    bool isCompleted() const {
        return get() == TaskState::Completed;
    }

    // Check if task failed
    bool isFailed() const {
        return get() == TaskState::Failed;
    }

    // Check if task is currently running
    bool isRunning() const {
        return get() == TaskState::Running;
    }

    // Get string representation of current state
    std::string_view toString() const {
        return taskStateToString(get());
    }
};

} // namespace SP