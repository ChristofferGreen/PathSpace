#define private public
#include "task/Task.hpp"
#undef private
#include "core/ExecutionCategory.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace SP;

namespace {

struct DummySink : NotificationSink {
    void notify(const std::string&) override {}
};

struct DummyExecutor : Executor {
    auto submit(std::weak_ptr<Task>&&) -> std::optional<Error> override { return std::nullopt; }
    auto shutdown() -> void override {}
    auto size() const -> size_t override { return 1; }
};

struct RecordingExecutor : Executor {
    using Executor::submit;

    auto submit(std::weak_ptr<Task>&& task) -> std::optional<Error> override {
        lastTask = std::move(task);
        return std::nullopt;
    }
    auto shutdown() -> void override {}
    auto size() const -> size_t override { return 1; }

    std::weak_ptr<Task> lastTask;
};

} // namespace

TEST_SUITE("task.task_create") {
TEST_CASE("Task::Create wires notifier, path, and execution category") {
    auto sink = std::make_shared<DummySink>();
    auto task = Task::Create(std::weak_ptr<NotificationSink>(sink),
                             "/task/path",
                             [] { return 3; },
                             ExecutionCategory::Unknown);

    REQUIRE(task);
    CHECK(task->notificationPath == "/task/path");
    CHECK_FALSE(task->notifier.expired());
    CHECK(task->executionCategory == ExecutionCategory::Immediate);

    DummyExecutor exec;
    task->setExecutor(&exec);
    CHECK(task->executor == &exec);

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());
    task->function(*task, false);
    task->markCompleted();

    int out = 0;
    task->resultCopy(&out);
    CHECK(out == 3);
}

TEST_CASE("Task::category reflects provided category") {
    auto sink = std::make_shared<DummySink>();
    auto task = Task::Create(std::weak_ptr<NotificationSink>(sink),
                             "/task/lazy",
                             [] { return 7; },
                             ExecutionCategory::Lazy);

    REQUIRE(task);
    CHECK(task->category() == ExecutionCategory::Lazy);
}

TEST_CASE("Task::Create with PathSpaceBase wires space pointer and defaults category") {
    auto space = reinterpret_cast<PathSpaceBase*>(0x1234);
    auto task = Task::Create(space,
                             "/task/space",
                             [] { return 10; },
                             ExecutionCategory::Unknown);

    REQUIRE(task);
    CHECK(task->space == space);
    CHECK(task->notificationPath == "/task/space");
    CHECK(task->executionCategory == ExecutionCategory::Immediate);
    CHECK(task->notifier.expired());

    task->setLabel("SpaceTask");
    CHECK(task->getLabel() == "SpaceTask");

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());
    task->function(*task, false);
    task->markCompleted();

    int out = 0;
    task->resultCopy(&out);
    CHECK(out == 10);
}

TEST_CASE("Task::Create rejects non-callable inputs") {
    auto sink = std::make_shared<DummySink>();
    int nonCallable = 5;

    auto taskWithNotifier = Task::Create(std::weak_ptr<NotificationSink>(sink),
                                         "/task/bad",
                                         nonCallable,
                                         ExecutionCategory::Immediate);
    CHECK(taskWithNotifier == nullptr);

    auto taskWithSpace = Task::Create(static_cast<PathSpaceBase*>(nullptr),
                                      "/task/bad2",
                                      nonCallable,
                                      ExecutionCategory::Immediate);
    CHECK(taskWithSpace == nullptr);
}

TEST_CASE("Task::Create without PathSpaceBase keeps space null") {
    auto task = Task::Create([](Task&, bool) {});
    REQUIRE(task);
    CHECK(task->space == nullptr);
    CHECK(task->notificationPath.empty());
    CHECK(static_cast<bool>(task->function));
}

TEST_CASE("Task::resultCopy waits until completion") {
    auto task = std::make_shared<Task>();
    task->resultCopyFn = [](std::any const& from, void* const to) {
        *static_cast<int*>(to) = std::any_cast<int>(from);
    };

    REQUIRE(task->tryStart());
    REQUIRE(task->transitionToRunning());

    std::atomic<bool> started{false};
    std::thread finisher([&]() {
        started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        task->result = 7;
        task->markCompleted();
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    int out = 0;
    task->resultCopy(&out);
    finisher.join();

    CHECK(task->state.isCompleted());
    CHECK(out == 7);
}

TEST_CASE("Executor shared_ptr submit forwards to weak overload") {
    RecordingExecutor exec;
    auto task = Task::Create([](Task&, bool) {});

    auto err = exec.Executor::submit(task);
    CHECK_FALSE(err.has_value());
    CHECK_FALSE(exec.lastTask.expired());
}
}
