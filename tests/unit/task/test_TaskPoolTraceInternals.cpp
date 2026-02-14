#define private public
#include "task/TaskPool.hpp"
#undef private
#include "task/Task.hpp"
#include "third_party/doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;
using Json = nlohmann::json;

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

auto make_temp_path(std::string_view suffix) -> std::filesystem::path {
    auto base = std::filesystem::temp_directory_path();
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::string filename = "pathspace_trace_" + std::to_string(stamp) + "_" + std::to_string(tid) + "_";
    filename.append(suffix.begin(), suffix.end());
    return base / filename;
}

auto read_file(std::filesystem::path const& path) -> std::string {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
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

TEST_CASE("queue wait events fall back to path when label is empty") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    auto task = Task::Create([](Task const&, bool) {});
    task->notificationPath = "/queue/fallback";

    pool.recordTraceQueueStart(task.get(), task->label, task->notificationPath);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) {
                               return e.phase == 'b' && e.name == "Wait /queue/fallback";
                           });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->path == "/queue/fallback");
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

TEST_CASE("TraceScope move constructor preserves a single span") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    {
        auto scopeA = pool.traceScope("MoveCtorSpan", "trace");
        TaskPool::TraceScope scopeB{std::move(scopeA)};
        std::this_thread::sleep_for(1ms);
    }

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    size_t spanCount = std::count_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                                     [](TaskPool::TaskTraceEvent const& e) {
                                         return e.phase == 'X' && e.name == "MoveCtorSpan";
                                     });
    CHECK(spanCount == 1);
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

TEST_CASE("traceCounter clamps start time when trace base is in the future") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");
    pool.traceStartMicros.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);

    pool.traceCounter("FutureCounter", 1.0);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) {
                               return e.phase == 'C' && e.name == "FutureCounter";
                           });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->startUs == 0);
    CHECK(it->hasCounter);
    CHECK(it->counterValue == doctest::Approx(1.0));
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

TEST_CASE("traceSpan uses current thread when no thread id is provided") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.traceSpan("DefaultThreadSpan", "default", "/path", 1, 2);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) { return e.name == "DefaultThreadSpan"; });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->threadId != 0);
    CHECK(it->category == "default");
    CHECK(it->path == "/path");
}

TEST_CASE("recordTraceSpan captures queue wait metadata") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.recordTraceSpan("QueueSpan", "/queue", "queue", 10, 5, 321, 7);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) { return e.name == "QueueSpan"; });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->threadId == 321);
    CHECK(it->startUs == 10);
    CHECK(it->durUs == 5);
    CHECK(it->path == "/queue");
    CHECK(it->category == "queue");
    CHECK(it->hasQueueWait);
    CHECK(it->queueWaitUs == 7);
}

TEST_CASE("recordTraceAsync captures async metadata") {
    TaskPool pool(1);
    pool.enableTrace("trace_unused.json");

    pool.recordTraceAsync("AsyncEvent", "/async/path", "async", 42, 'b', 77);

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    auto it = std::find_if(pool.traceEvents.begin(), pool.traceEvents.end(),
                           [](TaskPool::TaskTraceEvent const& e) { return e.name == "AsyncEvent"; });
    REQUIRE(it != pool.traceEvents.end());
    CHECK(it->phase == 'b');
    CHECK(it->path == "/async/path");
    CHECK(it->category == "async");
    CHECK(it->startUs == 42);
    CHECK(it->asyncId == 77);
    CHECK(it->threadId == 0);
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

TEST_CASE("flushTrace writes escaped JSON strings") {
    TaskPool pool(1);
    auto tracePath = make_temp_path("trace.json");
    pool.enableTrace(tracePath.string());

    std::string name = std::string("Span\"\\\\\n\t") + '\x01';
    std::string path = "/path/\"quote\"\\\n";
    std::string category = "cat\t\"\\";
    pool.recordTraceSpan(name, path, category, 5, 6, 123, 7);

    auto err = pool.flushTrace();
    CHECK_FALSE(err.has_value());

    auto contents = read_file(tracePath);
    CHECK(contents.find("\\u0001") != std::string::npos);
    CHECK(contents.find("\\n") != std::string::npos);
    CHECK(contents.find("\\t") != std::string::npos);
    CHECK(contents.find("\\\"") != std::string::npos);
    CHECK(contents.find("\\\\") != std::string::npos);

    auto parsed = Json::parse(contents);
    bool found = false;
    for (auto const& event : parsed.at("traceEvents")) {
        if (event.contains("name") && event.at("name") == name) {
            REQUIRE(event.contains("args"));
            CHECK(event.at("args").at("path") == path);
            CHECK(event.at("args").at("category") == category);
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("flushTrace writes NDJSON queue wait and thread names") {
    TaskPool pool(1);
    auto tracePath = make_temp_path("trace.ndjson");
    pool.enableTraceNdjson(tracePath.string());

    pool.recordTraceThreadName(999999u, "worker_name");
    pool.recordTraceSpan("QueueSpan", "/queue", "queue", 10, 5, 321, 7);

    auto err = pool.flushTrace();
    CHECK_FALSE(err.has_value());

    auto contents = read_file(tracePath);
    std::istringstream lines(contents);
    std::string line;
    bool sawThreadName = false;
    bool sawQueueWait = false;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        auto entry = Json::parse(line);
        if (entry.contains("thread_name")) {
            sawThreadName = true;
        }
        if (entry.contains("queue_wait_us")) {
            sawQueueWait = true;
            CHECK(entry.at("queue_wait_us") == 7);
        }
    }
    CHECK(sawThreadName);
    CHECK(sawQueueWait);
}

TEST_CASE("flushTrace reports errors when trace path is invalid") {
    TaskPool pool(1);
    auto badDir = make_temp_path("trace_dir");
    std::filesystem::create_directory(badDir);
    pool.enableTrace(badDir.string());

    auto err = pool.flushTrace();
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnknownError);
}
}
