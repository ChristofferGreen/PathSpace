#define private public
#include "task/Task.hpp"
#undef private
#include "core/ExecutionCategory.hpp"
#include "third_party/doctest.h"

#include <memory>
#include <string>

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
}
