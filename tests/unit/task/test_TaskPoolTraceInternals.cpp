#define private public
#include "task/TaskPool.hpp"
#undef private
#include "task/Task.hpp"
#include "third_party/doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

namespace {

auto count_events(TaskPool& pool, char phase, std::string_view name) -> size_t {
    std::lock_guard<std::mutex> lock(pool.traceMutex);
    return std::count_if(pool.traceEvents.begin(), pool.traceEvents.end(), [&](TaskPool::TaskTraceEvent const& e) {
        return e.phase == phase && e.name == name;
    });
}

auto find_span(TaskPool& pool, std::string_view name) -> std::optional<TaskPool::TaskTraceEvent> {
    std::lock_guard<std::mutex> lock(pool.traceMutex);
    for (auto const& e : pool.traceEvents) {
        if (e.phase == 'X' && e.name == name) {
            return e;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_SUITE("task.pool.trace") {
TEST_CASE("queue wait events include fallback labels and queue wait duration") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    std::atomic<bool> done{false};
    auto task = Task::Create([&done](Task const&, bool) {
        done.store(true, std::memory_order_release);
    });

    REQUIRE(task->tryStart());
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.tasks.push(task);
        pool.recordTraceQueueStart(task.get(), task->label, task->notificationPath);
        pool.taskCV.notify_one();
    }
    while (!done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (pool.activeTasks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    // "Wait" events are emitted as async begin/end with no label/path set.
    CHECK(count_events(pool, 'b', "Wait") == 1);
    CHECK(count_events(pool, 'e', "Wait Task") == 1);

    auto span = find_span(pool, "Task");
    REQUIRE(span.has_value());
    CHECK(span->hasQueueWait);
}

TEST_CASE("queue wait events prefer task labels over paths") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    auto task = Task::Create([](Task const&, bool) {});
    task->notificationPath = "/queue/path";
    task->setLabel("LabelledTask");

    pool.recordTraceQueueStart(task.get(), task->label, task->notificationPath);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) {
                               return e.phase == 'b' && e.name == "Wait LabelledTask";
                           });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->path == "/queue/path");
    CHECK(it->category == "queue");
}

TEST_CASE("traceThreadName avoids duplicate records for the same thread") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.traceThreadName("Primary");
    pool.traceThreadName("Secondary");

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    size_t primary = 0;
    size_t secondary = 0;
    for (auto const& e : pool.traceEvents) {
        if (e.phase != 'M') {
            continue;
        }
        if (e.threadName == "Primary") {
            ++primary;
        } else if (e.threadName == "Secondary") {
            ++secondary;
        }
    }

    CHECK(primary == 1);
    CHECK(secondary == 0);
}

TEST_CASE("TraceScope move assignment preserves a single span") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    {
        auto scopeA = pool.traceScope("MoveSpan", "trace");
        TaskPool::TraceScope scopeB;
        scopeB = std::move(scopeA);
        std::this_thread::sleep_for(1ms);
    }

    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        size_t spanCount = std::count_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                                         [](TaskPool::TaskTraceEvent const& e) {
                                             return e.phase == 'X' && e.name == "MoveSpan";
                                         });
        CHECK(spanCount == 1);
    }
    CHECK(find_span(pool, "MoveSpan").has_value());
}

TEST_CASE("trace helpers are no-ops when tracing is disabled") {
    TaskPool pool(1);

    pool.traceThreadName("disabled");
    pool.traceCounter("disabled_counter", 1.0);
    pool.traceSpan("disabled_span", "disabled", "/disabled", 0, 1);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    CHECK(pool.traceEvents.empty());
}

TEST_CASE("traceCounter records counter events when tracing is enabled") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.traceCounter("CounterA", 3.5);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) {
                               return e.phase == 'C' && e.name == "CounterA";
                           });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->hasCounter);
    CHECK(it->counterValue == doctest::Approx(3.5));
    CHECK(it->threadId != 0);
}

TEST_CASE("traceNowUs returns zero when tracing is disabled") {
    TaskPool pool(1);
    CHECK(pool.traceNowUs() == 0);
}

TEST_CASE("traceNowUs returns zero when trace base is in the future") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");
    pool.traceStartMicros.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);

    CHECK(pool.traceNowUs() == 0);
}

TEST_CASE("traceSpan honors explicit thread id") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.traceSpan("ExplicitSpan", "explicit", "/path", 5, 12, 4242);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) { return e.name == "ExplicitSpan"; });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->threadId == 4242);
    CHECK(it->startUs == 5);
    CHECK(it->durUs == 12);
    CHECK(it->category == "explicit");
    CHECK(it->path == "/path");
    CHECK(it->phase == 'X');
}

TEST_CASE("traceScope returns inactive scope when tracing is disabled") {
    TaskPool pool(1);

    auto scope = pool.traceScope("DisabledScope", "disabled", "/disabled");
    (void)scope;

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    CHECK(pool.traceEvents.empty());
}

TEST_CASE("TraceScope move assignment handles self-assignment") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    auto scope = pool.traceScope("SelfMove", "trace");
    scope = std::move(scope);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto count = std::count_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                               [](TaskPool::TaskTraceEvent const& e) { return e.name == "SelfMove"; });
    CHECK(count <= 1);
}
}
