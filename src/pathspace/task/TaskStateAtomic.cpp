#include "TaskStateAtomic.hpp"

#include <string_view>

namespace SP {

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

TaskStateAtomic::TaskStateAtomic(const TaskStateAtomic& other) : state(other.state.load(std::memory_order_acquire)) {}

TaskStateAtomic& TaskStateAtomic::operator=(const TaskStateAtomic& other) {
    state.store(other.state.load(std::memory_order_acquire), std::memory_order_release);
    return *this;
}

bool TaskStateAtomic::tryStart() {
    TaskState expected = TaskState::NotStarted;
    return state.compare_exchange_strong(expected, TaskState::Starting);
}

bool TaskStateAtomic::transitionToRunning() {
    TaskState expected = TaskState::Starting;
    return state.compare_exchange_strong(expected, TaskState::Running);
}

bool TaskStateAtomic::markCompleted() {
    TaskState expected = TaskState::Running;
    return state.compare_exchange_strong(expected, TaskState::Completed);
}

bool TaskStateAtomic::markFailed() {
    TaskState current = state.load();
    if (current != TaskState::Completed) {
        state.store(TaskState::Failed);
        return true;
    }
    return false;
}

TaskState TaskStateAtomic::get() const {
    return state.load(std::memory_order_acquire);
}

bool TaskStateAtomic::isTerminal() const {
    TaskState current = get();
    return current == TaskState::Completed || current == TaskState::Failed;
}

bool TaskStateAtomic::hasStarted() const {
    return get() != TaskState::NotStarted;
}

bool TaskStateAtomic::isCompleted() const {
    return get() == TaskState::Completed;
}

bool TaskStateAtomic::isFailed() const {
    return get() == TaskState::Failed;
}

bool TaskStateAtomic::isRunning() const {
    return get() == TaskState::Running;
}

std::string_view TaskStateAtomic::toString() const {
    return taskStateToString(get());
}

} // namespace SP