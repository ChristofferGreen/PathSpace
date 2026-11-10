#define private public
#include "task/Task.hpp"
#include "task/TaskT.hpp"
#undef private

#include "PathSpace.hpp"
#include "task/Future.hpp"
#include "task/IFutureAny.hpp"
#include "third_party/doctest.h"

#include <any>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

namespace {

auto makeFailedTask() -> std::shared_ptr<Task> {
    auto task          = std::make_shared<Task>();
    task->function     = [](Task&, bool const) { throw std::runtime_error("boom"); };
    task->resultCopyFn = [](std::any const& from, void* const to) { *static_cast<int*>(to) = std::any_cast<int>(from); };
    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());
    // What a pool worker does with a task whose function throws.
    try {
        task->function(*task, false);
        task->markCompleted();
    } catch (...) {
        task->markFailed();
    }
    REQUIRE(task->state.isFailed());
    return task;
}

// Runs `blocking` on a helper thread and reports whether it returned within the limit. When it did
// not, `unblock` must make it return so the helper can be joined and the test can fail cleanly.
template <typename Blocking, typename Unblock>
auto returnsWithin(std::chrono::milliseconds limit, Blocking&& blocking, Unblock&& unblock) -> bool {
    std::promise<void> done;
    auto               finished = done.get_future();
    std::thread        helper([&] {
        blocking();
        done.set_value();
    });
    bool const inTime = finished.wait_for(limit) == std::future_status::ready;
    if (!inTime) {
        unblock();
    }
    helper.join();
    return inTime;
}

} // namespace

TEST_SUITE("task.failure.statemachine") {

TEST_CASE("Future::wait returns when the task failed") {
    auto   task = makeFailedTask();
    Future fut  = Future::FromShared(task);

    bool const returned = returnsWithin(
        2s, [&] { fut.wait(); },
        [&] { task->state.state.store(TaskState::Completed); });
    CHECK(returned);
    CHECK_FALSE(fut.ready());
}

TEST_CASE("Future::copy_result_to reports a failed task instead of waiting for a result") {
    auto   task = makeFailedTask();
    Future fut  = Future::FromShared(task);

    int        out      = 17;
    bool       copied   = true;
    bool const returned = returnsWithin(
        2s, [&] { copied = fut.copy_result_to(&out); },
        [&] {
            task->result = 0;
            task->state.state.store(TaskState::Completed);
        });
    CHECK(returned);
    if (returned) {
        CHECK_FALSE(copied);
        CHECK(out == 17);
    }
}

TEST_CASE("Future timed waits end as soon as the task failed") {
    auto   task = makeFailedTask();
    Future fut  = Future::FromShared(task);

    // A failed task never becomes ready, so the timed wait reports false; it must do so without
    // spinning until the deadline. The helper bounds the test should it spin anyway.
    bool       ready    = true;
    bool const returned = returnsWithin(
        2s, [&] { ready = fut.wait_for(std::chrono::hours(1)); },
        [&] { task->state.state.store(TaskState::Completed); });
    CHECK(returned);
    if (returned) {
        CHECK_FALSE(ready);
    }
}

TEST_CASE("a throwing TaskT releases the waiters on its typed future") {
    auto task = TaskT<int>::Create([]() -> int { throw std::runtime_error("boom"); });
    REQUIRE(task);
    auto legacy = task->legacy_task();
    REQUIRE(legacy);
    REQUIRE(legacy->tryStart());
    REQUIRE(legacy->transitionToRunning());
    try {
        legacy->function(*legacy, false);
        legacy->markCompleted();
    } catch (...) {
        legacy->markFailed();
    }

    auto fut = task->future();
    auto any = task->any_future();

    bool const waitReturned = returnsWithin(
        2s, [&] { fut.wait(); },
        [&] { task->promise_.set_value(0); });
    CHECK(waitReturned);
    if (!waitReturned) {
        return;
    }
    CHECK_FALSE(fut.ready());
    CHECK_FALSE(any.ready());

    int  out = 23;
    // Blocking accessors must return false rather than block: no value will ever arrive.
    CHECK_FALSE(fut.get(out));
    CHECK_FALSE(any.copy_to(&out));
    CHECK(out == 23);
    CHECK_FALSE(any.wait_for(std::chrono::hours(1)));
    CHECK_FALSE(fut.wait_for(std::chrono::hours(1)));
}

TEST_CASE("a failed shared state cannot be fulfilled afterwards") {
    PromiseT<int> promise;
    auto          fut = promise.get_future();
    promise.shared_state()->fail();
    CHECK_FALSE(promise.set_value(4));
    CHECK_FALSE(fut.ready());
    int out = 0;
    CHECK_FALSE(fut.try_get(out));

    PromiseT<int> fulfilled;
    REQUIRE(fulfilled.set_value(9));
    fulfilled.shared_state()->fail();
    CHECK(fulfilled.get_future().ready());
    CHECK(fulfilled.get_future().try_get(out));
    CHECK(out == 9);
}

// OPEN DECISION (docs/State_Machine_Audit.md, T3): what a read or take returns for an execution
// whose callable threw. Today the failed task stays at the front of the node forever: every read
// reports "Task is not completed" (the same error as for a task that is still running) and blocks
// until its timeout, and values queued behind the task are unreachable. The test states the
// minimum any resolution has to provide; skipped until the error contract is chosen (it fails today).
TEST_CASE("a failed execution does not block its node forever" * doctest::skip()) {
    PathSpace space;
    REQUIRE(space.insert("/work", []() -> int { throw std::runtime_error("boom"); }).errors.empty());
    REQUIRE(space.insert("/work", 5).errors.empty());

    // Returns after the worker announced the task's end (or after the timeout).
    auto pending = space.read<int>("/work", Block{500ms});
    REQUIRE_FALSE(pending.has_value());

    auto failed = space.take<int>("/work");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message.value_or("") != "Task is not completed");

    auto behind = space.take<int>("/work");
    REQUIRE(behind.has_value());
    CHECK(*behind == 5);
}

}
