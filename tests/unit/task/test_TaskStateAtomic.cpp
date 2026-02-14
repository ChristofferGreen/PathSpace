#include "task/TaskStateAtomic.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("task.taskstate.atomic") {
TEST_CASE("TaskStateAtomic transitions cover all branches") {
    TaskStateAtomic state;
    CHECK_FALSE(state.hasStarted());
    CHECK_FALSE(state.isTerminal());
    CHECK(state.toString() == "NotStarted");

    // First start succeeds, second fails.
    CHECK(state.tryStart());
    CHECK_FALSE(state.tryStart());
    CHECK(state.hasStarted());
    CHECK(state.toString() == "Starting");

    // Only transitions from Starting -> Running succeed.
    CHECK(state.transitionToRunning());
    CHECK_FALSE(state.transitionToRunning());
    CHECK(state.isRunning());
    CHECK_FALSE(state.isCompleted());

    // Mark completed once; subsequent calls no-op.
    CHECK(state.markCompleted());
    CHECK(state.isCompleted());
    CHECK(state.isTerminal());
    CHECK_FALSE(state.markCompleted());
    CHECK(state.toString() == "Completed");

    // markFailed should return false once already completed.
    CHECK_FALSE(state.markFailed());
}

TEST_CASE("TaskStateAtomic markFailed before completion") {
    TaskStateAtomic state;
    CHECK(state.markFailed());
    CHECK(state.isFailed());
    CHECK(state.toString() == "Failed");
    CHECK(state.isTerminal());
}

TEST_CASE("TaskStateAtomic copy and assignment snapshot current state") {
    TaskStateAtomic original;
    CHECK(original.tryStart());
    CHECK(original.transitionToRunning());

    TaskStateAtomic copied{original};
    CHECK(copied.isRunning());

    TaskStateAtomic assigned;
    assigned = original;
    CHECK(assigned.isRunning());
}

TEST_CASE("TaskStateAtomic toString falls back to Unknown for invalid states") {
    TaskStateAtomic state;
    // Force an invalid enumeration value to reach the default branch.
    auto* raw = reinterpret_cast<std::atomic<TaskState>*>(&state);
    raw->store(static_cast<TaskState>(42), std::memory_order_relaxed);
    CHECK(state.toString() == "Unknown");
}

TEST_CASE("TaskStateAtomic toString reports Running when active") {
    TaskStateAtomic state;
    REQUIRE(state.tryStart());
    REQUIRE(state.transitionToRunning());
    CHECK(state.toString() == "Running");
}

TEST_CASE("TaskState enum values are ordered") {
    CHECK(static_cast<int>(TaskState::NotStarted) == 0);
    CHECK(static_cast<int>(TaskState::Starting) == 1);
    CHECK(static_cast<int>(TaskState::Running) == 2);
    CHECK(static_cast<int>(TaskState::Completed) == 3);
    CHECK(static_cast<int>(TaskState::Failed) == 4);
}
}
