#define private public
#include "task/Task.hpp"
#undef private
#include "task/Future.hpp"
#include "third_party/doctest.h"

#include <chrono>
#include <memory>
#include <any>

using namespace SP;

TEST_SUITE("task.future.coverage") {
TEST_CASE("TaskStateAtomic transitions drive Future readiness") {
    auto task = std::make_shared<Task>();
    task->function = [](Task& t, bool const) { t.result = 42; };
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    // Drive task state machine manually.
    CHECK(task->tryStart());
    CHECK(task->transitionToRunning());
    task->function(*task, false);
    task->markCompleted();

    Future fut = Future::FromShared(task);
    CHECK(fut.valid());
    CHECK(fut.ready());

    int out = 0;
    CHECK(fut.try_copy_result_to(&out));
    CHECK(out == 42);

    out = 0;
    CHECK(fut.copy_result_to(&out));
    CHECK(out == 42);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    CHECK(fut.wait_until_steady(deadline));
}

TEST_CASE("Future handles expired task gracefully") {
    Future invalid;
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(invalid.ready());

    int out = 0;
    CHECK_FALSE(invalid.try_copy_result_to(&out));

    auto deadline = std::chrono::steady_clock::now();
    CHECK_FALSE(invalid.wait_until_steady(deadline));
    invalid.wait(); // Should be a no-op for expired tasks.
}
}
