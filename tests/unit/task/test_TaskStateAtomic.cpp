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
}
