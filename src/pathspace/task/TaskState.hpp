#pragma once

namespace SP {

// Represents the possible states of a task
enum class TaskState {
    NotStarted, // Initial state when task is created
    Starting,   // Task is being prepared for execution
    Running,    // Task is actively executing
    Completed,  // Task finished successfully
    Failed      // Task encountered an error during execution
};

} // namespace SP