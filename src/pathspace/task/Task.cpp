#include "task/Task.hpp"

namespace SP {

bool Task::isCompleted() const {
    return this->state.isCompleted();
}

bool Task::hasStarted() const {
    return this->state.hasStarted();
}

bool Task::tryStart() {
    return this->state.tryStart();
}

bool Task::transitionToRunning() {
    return this->state.transitionToRunning();
}

void Task::markCompleted() {
    this->state.markCompleted();
}

void Task::markFailed() {
    this->state.markFailed();
}

auto Task::category() const -> std::optional<ExecutionOptions::Category> {
    if (!this->executionOptions.has_value())
        return std::nullopt;
    return this->executionOptions.value().category;
}

} // namespace SP