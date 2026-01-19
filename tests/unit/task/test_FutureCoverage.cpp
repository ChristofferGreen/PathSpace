#define private public
#include "task/Task.hpp"
#undef private
#include "task/Future.hpp"
#include "third_party/doctest.h"

#include <chrono>
#include <memory>
#include <any>
#include <thread>

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

TEST_CASE("Future wait_until_steady times out when task is not completed") {
    auto task = std::make_shared<Task>();
    task->function = [](Task& t, bool const) { t.result = 7; };
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    // Start the task but never transition it to Completed so wait_until_steady
    // must respect the deadline and return false.
    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());

    Future fut = Future::FromShared(task);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    CHECK_FALSE(fut.wait_until_steady(deadline));

    // Finish the task afterward to avoid leaving it in Running for other tests.
    task->markCompleted();
}

TEST_CASE("Future wait spins until task completes") {
    auto task = std::make_shared<Task>();
    task->function = [](Task& t, bool const) { t.result = 99; };
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());

    Future fut = Future::FromShared(task);
    std::thread finisher([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        task->result = 99;
        task->markCompleted();
    });

    fut.wait(); // Should spin/yield until finisher marks completion.
    finisher.join();

    int out = 0;
    CHECK(fut.copy_result_to(&out));
    CHECK(out == 99);
}

TEST_CASE("Future try_copy_result_to succeeds after completion without waiting") {
    auto task = std::make_shared<Task>();
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());
    task->result = 123;
    task->markCompleted();

    Future fut = Future::FromShared(task);
    int    out = 0;
    CHECK(fut.try_copy_result_to(&out));
    CHECK(out == 123);
}

TEST_CASE("Future copy_result_to returns false once task expires") {
    auto task = std::make_shared<Task>();
    task->function = [](Task& t, bool const) { t.result = 1; };
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());
    task->markCompleted();

    Future fut = Future::FromShared(task);
    task.reset(); // drop last shared ownership so the weak_ptr expires

    int out = 0;
    CHECK_FALSE(fut.copy_result_to(&out));
}
}
