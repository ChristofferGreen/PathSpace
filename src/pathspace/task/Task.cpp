#include "task/Task.hpp"

namespace SP {

auto Task::isCompleted() const -> bool {
    return this->state.isCompleted();
}

auto Task::hasStarted() const -> bool {
    return this->state.hasStarted();
}

auto Task::tryStart() -> bool {
    return this->state.tryStart();
}

auto Task::transitionToRunning() -> bool {
    return this->state.transitionToRunning();
}

auto Task::markCompleted() -> void {
    this->state.markCompleted();
}

auto Task::markFailed() -> void {
    this->state.markFailed();
}

auto Task::category() const -> ExecutionCategory {
    return this->executionCategory;
}

} // namespace SP