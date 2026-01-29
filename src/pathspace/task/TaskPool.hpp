#pragma once
#include "Task.hpp"
#include "Executor.hpp"
#include "core/Error.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <optional>
#include <unordered_set>
#include <vector>
#include <unordered_map>

namespace SP {


class TaskPool : public Executor {
public:
    explicit TaskPool(size_t threadCount = std::thread::hardware_concurrency());
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&)                    = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    auto submit(std::weak_ptr<Task>&& task) -> std::optional<Error> override;
    auto addTask(std::weak_ptr<Task>&& task) -> std::optional<Error>;
    auto shutdown() -> void override; // Ensures all worker threads are joined before return
    auto size() const -> size_t override;
    auto enableTrace(std::string const& path) -> void;
    auto enableTraceNdjson(std::string const& path) -> void;
    auto flushTrace() -> std::optional<Error>;
    auto traceThreadName(std::string const& name) -> void;

    struct TraceScope {
        TraceScope() = default;
        TraceScope(TaskPool* pool,
                   std::string name,
                   std::string category,
                   std::string path,
                   int64_t startMicros,
                   uint64_t threadId);
        TraceScope(TraceScope&& other) noexcept;
        auto operator=(TraceScope&& other) noexcept -> TraceScope&;
        ~TraceScope();

        TraceScope(TraceScope const&) = delete;
        auto operator=(TraceScope const&) -> TraceScope& = delete;

    private:
        TaskPool*   pool = nullptr;
        std::string name;
        std::string category;
        std::string path;
        int64_t     startMicros = 0;
        uint64_t    threadId = 0;
    };

    auto traceScope(std::string name,
                    std::string category = {},
                    std::string path = {}) -> TraceScope;

private:
    auto workerFunction(size_t workerIndex) -> void;
    auto recordTraceSpan(std::string const& name,
                         std::string const& path,
                         std::string const& category,
                         uint64_t startUs,
                         uint64_t durUs,
                         uint64_t threadId,
                         std::optional<uint64_t> queueWaitUs = std::nullopt) -> void;
    auto recordTraceAsync(std::string const& name,
                          std::string const& path,
                          std::string const& category,
                          uint64_t tsUs,
                          char phase,
                          uint64_t asyncId) -> void;
    auto recordTraceThreadName(uint64_t threadId, std::string const& name) -> void;
    auto recordTraceQueueStart(Task* task, std::string const& label, std::string const& path) -> void;
    auto takeTraceQueueStart(Task* task) -> std::optional<std::pair<int64_t, uint64_t>>;

    std::vector<std::thread>        workers;
    std::queue<std::weak_ptr<Task>> tasks;
    std::mutex                      mutex;
    std::condition_variable         taskCV;
    std::atomic<bool>               shuttingDown{false};
    std::atomic<size_t>             activeWorkers{0};
    std::atomic<size_t>             activeTasks{0};

    struct TaskTraceEvent {
        std::string name;
        std::string path;
        std::string category;
        std::string threadName;
        uint64_t    startUs = 0;
        uint64_t    durUs = 0;
        uint64_t    threadId = 0;
        uint64_t    asyncId = 0;
        uint64_t    queueWaitUs = 0;
        bool        hasQueueWait = false;
        char        phase = 'X';
    };

    std::mutex              traceMutex;
    std::vector<TaskTraceEvent> traceEvents;
    std::unordered_map<Task*, std::pair<int64_t, uint64_t>> traceQueueStarts;
    std::unordered_set<uint64_t> traceNamedThreads;
    std::mutex              workerMetaMutex;
    std::vector<uint64_t>   workerThreadIds;
    std::string             tracePath;
    std::string             traceNdjsonPath;
    std::atomic<bool>       traceEnabled{false};
    std::atomic<int64_t>    traceStartMicros{0};
};

} // namespace SP
