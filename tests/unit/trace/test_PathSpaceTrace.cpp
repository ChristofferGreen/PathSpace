#define private public
#include "task/TaskPool.hpp"
#undef private
#include "trace/PathSpaceTrace.hpp"
#include "third_party/doctest.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace SP;
using namespace SP::PathSpaceTrace;
using namespace std::chrono_literals;

namespace {

struct TaskPoolTraceGuard {
    TaskPool& pool;
    bool traceEnabled;
    int64_t traceStartMicros;
    std::string tracePath;
    std::string traceNdjsonPath;
    std::vector<TaskPool::TaskTraceEvent> traceEvents;
    std::unordered_map<Task*, std::pair<int64_t, uint64_t>> traceQueueStarts;
    std::unordered_set<uint64_t> traceNamedThreads;

    TaskPoolTraceGuard()
        : pool(TaskPool::Instance()) {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        traceEnabled = pool.traceEnabled.load(std::memory_order_relaxed);
        traceStartMicros = pool.traceStartMicros.load(std::memory_order_relaxed);
        tracePath = pool.tracePath;
        traceNdjsonPath = pool.traceNdjsonPath;
        traceEvents = pool.traceEvents;
        traceQueueStarts = pool.traceQueueStarts;
        traceNamedThreads = pool.traceNamedThreads;

        pool.traceEnabled.store(false, std::memory_order_relaxed);
        pool.traceStartMicros.store(0, std::memory_order_relaxed);
        pool.tracePath.clear();
        pool.traceNdjsonPath.clear();
        pool.traceEvents.clear();
        pool.traceQueueStarts.clear();
        pool.traceNamedThreads.clear();
    }

    ~TaskPoolTraceGuard() {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        pool.tracePath = tracePath;
        pool.traceNdjsonPath = traceNdjsonPath;
        pool.traceEvents = traceEvents;
        pool.traceQueueStarts = traceQueueStarts;
        pool.traceNamedThreads = traceNamedThreads;
        pool.traceStartMicros.store(traceStartMicros, std::memory_order_relaxed);
        pool.traceEnabled.store(traceEnabled, std::memory_order_relaxed);
    }
};

void reset_pathspace_trace_state() {
    std::lock_guard<std::mutex> lock(gMutex);
    gTotals.clear();
    gGroupId.store(0, std::memory_order_release);
    gDepth = 0;
}

} // namespace

TEST_SUITE("trace.pathspace") {
TEST_CASE("current_thread_id returns a non-zero identifier") {
    auto id = current_thread_id();
    CHECK(id != 0);
}

TEST_CASE("ScopedOp ignores work when trace is disabled") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    BeginGroup(77);
    {
        ScopedOp scope(Op::Read);
        std::this_thread::sleep_for(1ms);
    }

    std::lock_guard<std::mutex> lock(gMutex);
    CHECK(gTotals.empty());
}

TEST_CASE("ScopedOp ignores work when no active group") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    {
        ScopedOp scope(Op::Read);
        std::this_thread::sleep_for(1ms);
    }

    std::lock_guard<std::mutex> lock(gMutex);
    CHECK(gTotals.empty());
}

TEST_CASE("ScopedOp records totals for active group and skips nested ops") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    BeginGroup(11);

    {
        ScopedOp outer(Op::Read);
        std::this_thread::sleep_for(1ms);
        {
            ScopedOp inner(Op::Insert);
            std::this_thread::sleep_for(1ms);
        }
    }

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto const& [threadId, totals] : gTotals) {
            (void)threadId;
            if (totals.groupId == 11) {
                found = true;
                CHECK(totals.readUs > 0);
                CHECK(totals.insertUs == 0);
                CHECK(totals.takeUs == 0);
            }
        }
    }
    CHECK(found);
}

TEST_CASE("EndGroup emits trace spans and resets totals") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    BeginGroup(22);

    {
        ScopedOp readOp(Op::Read);
        std::this_thread::sleep_for(1ms);
    }
    {
        ScopedOp insertOp(Op::Insert);
        std::this_thread::sleep_for(1ms);
    }
    {
        ScopedOp takeOp(Op::Take);
        std::this_thread::sleep_for(1ms);
    }

    auto startUs = pool.traceNowUs();
    std::this_thread::sleep_for(1ms);
    auto endUs = pool.traceNowUs();

    size_t before = 0;
    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        before = pool.traceEvents.size();
    }

    EndGroup(pool, 22, startUs, endUs);

    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        CHECK(pool.traceEvents.size() > before);
        auto has_name = [&](std::string const& name) {
            return std::any_of(pool.traceEvents.begin() + static_cast<long>(before),
                               pool.traceEvents.end(),
                               [&](TaskPool::TaskTraceEvent const& e) { return e.name == name; });
        };
        CHECK(has_name("PathSpace"));
        CHECK(has_name("read"));
        CHECK(has_name("insert"));
        CHECK(has_name("take"));
    }

    bool cleared = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        cleared = std::all_of(gTotals.begin(), gTotals.end(), [](auto const& entry) {
            return entry.second.groupId == 0 &&
                   entry.second.readUs == 0 &&
                   entry.second.insertUs == 0 &&
                   entry.second.takeUs == 0;
        });
    }
    CHECK(cleared);
}

TEST_CASE("EndGroup scales spans when totals exceed frame duration") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gTotals[123] = ThreadTotals{.groupId = 99, .readUs = 60, .insertUs = 30, .takeUs = 10};
    }

    uint64_t startUs = 1000;
    uint64_t endUs   = 1050; // frameDur = 50 < total (100) to trigger scaling

    size_t before = 0;
    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        before = pool.traceEvents.size();
    }

    EndGroup(pool, 99, startUs, endUs);

    std::vector<TaskPool::TaskTraceEvent> events;
    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        events.assign(pool.traceEvents.begin() + static_cast<long>(before), pool.traceEvents.end());
    }

    auto findEvent = [&](std::string const& name) -> std::optional<TaskPool::TaskTraceEvent> {
        for (auto const& e : events) {
            if (e.name == name) return e;
        }
        return std::nullopt;
    };

    auto root = findEvent("PathSpace");
    REQUIRE(root.has_value());
    CHECK(root->durUs == 50);

    auto read = findEvent("read");
    auto insert = findEvent("insert");
    auto take = findEvent("take");
    REQUIRE(read.has_value());
    REQUIRE(insert.has_value());
    REQUIRE(take.has_value());
    CHECK(read->durUs == 30);
    CHECK(insert->durUs == 15);
    CHECK(take->durUs == 5);
}

TEST_CASE("EndGroup is a no-op for zero or reversed time windows") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    size_t before = 0;
    {
        std::lock_guard<std::mutex> lock(pool.traceMutex);
        before = pool.traceEvents.size();
    }

    EndGroup(pool, 101, 0, 100);      // start is zero
    EndGroup(pool, 102, 200, 150);    // end before start

    std::lock_guard<std::mutex> lock(pool.traceMutex);
    CHECK(pool.traceEvents.size() == before);
}

TEST_CASE("EndGroup clears totals when no time is recorded") {
    TaskPoolTraceGuard guard;
    reset_pathspace_trace_state();

    TaskPool& pool = TaskPool::Instance();
    pool.enableTraceNdjson("trace_unused.ndjson");

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gTotals[456] = ThreadTotals{.groupId = 333, .readUs = 0, .insertUs = 0, .takeUs = 0};
    }

    EndGroup(pool, 333, 100, 200);

    bool cleared = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        cleared = gTotals[456].groupId == 0 &&
                  gTotals[456].readUs == 0 &&
                  gTotals[456].insertUs == 0 &&
                  gTotals[456].takeUs == 0;
    }
    CHECK(cleared);
}
}
