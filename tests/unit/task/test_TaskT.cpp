#define private public
#include "task/TaskT.hpp"
#undef private
#include "task/TaskPool.hpp"
#include "third_party/doctest.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

namespace {

struct RecordingSink : NotificationSink {
    void notify(const std::string& notificationPath) override {
        std::lock_guard<std::mutex> lock(mutex);
        notifications.push_back(notificationPath);
        cv.notify_one();
    }

    std::mutex              mutex;
    std::condition_variable cv;
    std::vector<std::string> notifications;
};

} // namespace

TEST_SUITE("task.taskt") {
TEST_CASE("TaskT schedules work and fulfills future without notifier") {
    TaskPool pool(1);

    auto sink = std::make_shared<RecordingSink>();
    auto task = TaskT<int>::Create(sink, "/taskt/noop", [] { return 5; }, ExecutionCategory::Immediate, &pool);
    REQUIRE(task);

    task->setLabel("ComputeFive");
    CHECK(task->legacy_task());
    CHECK(task->legacy_task()->getLabel() == "ComputeFive");

    auto err = task->schedule(&pool);
    CHECK_FALSE(err.has_value());

    auto fut = task->future();
    int out = 0;
    CHECK(fut.get(out));
    CHECK(out == 5);

    auto any = task->any_future();
    int anyOut = 0;
    CHECK(any.copy_to(&anyOut));
    CHECK(anyOut == 5);
}

TEST_CASE("TaskT notifies sink on completion") {
    TaskPool pool(1);

    auto sink = std::make_shared<RecordingSink>();
    auto task = TaskT<int>::Create(sink, "/notify/path", [] { return 9; }, ExecutionCategory::Immediate, &pool);
    REQUIRE(task);

    auto err = task->schedule(&pool);
    CHECK_FALSE(err.has_value());

    auto fut = task->future();
    int out = 0;
    CHECK(fut.get(out));
    CHECK(out == 9);

    std::unique_lock<std::mutex> lock(sink->mutex);
    bool notified = sink->cv.wait_for(lock, 500ms, [&] {
        return !sink->notifications.empty();
    });
    CHECK(notified);
    if (notified) {
        CHECK(sink->notifications.front() == "/notify/path");
    }
}

TEST_CASE("TaskT Create without notifier executes and resolves future") {
    TaskPool pool(1);

    auto task = TaskT<int>::Create([] { return 11; }, ExecutionCategory::Immediate, &pool);
    REQUIRE(task);
    REQUIRE(task->legacy_task());
    CHECK(task->legacy_task()->notificationPath.empty());
    CHECK(task->legacy_task()->notifier.expired());

    auto err = task->schedule(&pool);
    CHECK_FALSE(err.has_value());

    auto fut = task->future();
    int out = 0;
    CHECK(fut.get(out));
    CHECK(out == 11);
}

TEST_CASE("TaskT schedule returns error when executor is missing") {
    auto sink = std::make_shared<RecordingSink>();
    auto task = TaskT<int>::Create(sink, "/taskt/error", [] { return 1; }, ExecutionCategory::Immediate, nullptr);
    REQUIRE(task);

    auto err = task->schedule(nullptr);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);
}

TEST_CASE("TaskT schedule fails when legacy task is missing") {
    TaskPool pool(1);
    auto task = std::shared_ptr<TaskT<int>>(new TaskT<int>());
    REQUIRE(task);

    auto err = task->schedule(&pool);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);
}
}
