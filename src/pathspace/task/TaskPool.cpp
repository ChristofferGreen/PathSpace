#include "TaskPool.hpp"
#include "core/NotificationSink.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string_view>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#else
#include <unistd.h>
#endif

namespace SP {
namespace {
auto now_micros() -> int64_t {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

auto execution_category_label(ExecutionCategory category) -> std::string_view {
    switch (category) {
        case ExecutionCategory::Immediate:
            return "Immediate";
        case ExecutionCategory::Lazy:
            return "Lazy";
        case ExecutionCategory::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

auto process_id() -> uint64_t {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

auto current_thread_id() -> uint64_t {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid) == 0 && tid != 0) {
        return tid;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

auto json_escape(std::string_view input) -> std::string {
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default: {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (uch < 0x20) {
                    constexpr char HexDigits[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped.push_back(HexDigits[(uch >> 4) & 0xF]);
                    escaped.push_back(HexDigits[uch & 0xF]);
                } else {
                    escaped.push_back(ch);
                }
            } break;
        }
    }
    return escaped;
}

auto format_task_label(std::string_view prefix, std::string const& path) -> std::string {
    if (path.empty()) {
        return std::string(prefix);
    }
    std::string label;
    label.reserve(prefix.size() + 1 + path.size());
    label.append(prefix);
    label.push_back(' ');
    label.append(path);
    return label;
}

auto task_display_label(Task const& task, std::string const& fallbackPath) -> std::string {
    if (!task.getLabel().empty()) {
        return task.getLabel();
    }
    if (!fallbackPath.empty()) {
        return fallbackPath;
    }
    return "Task";
}

} // namespace

TaskPool& TaskPool::Instance() {
    // Leak-on-exit singleton to avoid destructor-order and atexit races in heavy looped runs
    static TaskPool* instance = []() -> TaskPool* {
        return new TaskPool();
    }();
    return *instance;
}

TaskPool::TaskPool(size_t threadCount) {
    sp_log("TaskPool::TaskPool constructing", "TaskPool");
    if (threadCount == 0) threadCount = 1;
    activeWorkers = 0;
    {
        std::lock_guard<std::mutex> lock(workerMetaMutex);
        workerThreadIds.resize(threadCount, 0);
    }
    for (size_t i = 0; i < threadCount; ++i) {
        try {
            sp_log("TaskPool::TaskPool spawning worker", "TaskPool");
            workers.emplace_back([this, i]() { workerFunction(i); });
            ++activeWorkers;
        } catch (...) {
            sp_log("TaskPool::TaskPool failed to spawn worker", "TaskPool");
            break; // stop spawning if thread creation fails
        }
    }
    sp_log("TaskPool::TaskPool constructed with workers=" + std::to_string(activeWorkers.load()), "TaskPool");
}

TaskPool::~TaskPool() {
    sp_log("TaskPool::~TaskPool", "TaskPool");
    shutdown();
}

auto TaskPool::addTask(std::weak_ptr<Task>&& task) -> std::optional<Error> {
    sp_log("TaskPool::addTask called", "TaskPool");
    std::shared_ptr<Task> lockedTask;
    std::string taskPath;
    std::string taskLabel;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!shuttingDown) {
            if (auto locked = task.lock()) {
                lockedTask = locked;
                // If the task has already started, treat as success and do not re-enqueue.
                if (locked->hasStarted()) {
                    sp_log("TaskPool::addTask: task already started; treating as success (no enqueue)", "TaskPool");
                    return std::nullopt;
                }
                if (!locked->tryStart()) {
                    // If tryStart fails because it is already started, treat as success.
                    if (locked->hasStarted()) {
                        sp_log("TaskPool::addTask: tryStart failed but task has started; treating as success", "TaskPool");
                        return std::nullopt;
                    }
                    sp_log("TaskPool::addTask tryStart failed (not started and cannot start)", "TaskPool");
                    return Error{Error::Code::UnknownError, "Failed to start lazy execution"};
                }
                sp_log("TaskPool::addTask enqueuing task", "TaskPool");
                tasks.push(std::move(task));
                taskCV.notify_one();
                taskPath = locked->notificationPath;
                taskLabel = locked->label;
            } else {
                sp_log("TaskPool::addTask task expired before enqueue", "TaskPool");
                return Error{Error::Code::UnknownError, "Task expired before enqueue"};
            }
        } else {
            sp_log("TaskPool::addTask refused: shutting down", "TaskPool");
            return Error{Error::Code::UnknownError, "Executor shutting down"};
        }
    }
    if (lockedTask && traceEnabled.load(std::memory_order_acquire)) {
        recordTraceQueueStart(lockedTask.get(), taskLabel, taskPath);
    }
    return std::nullopt;
}

auto TaskPool::submit(std::weak_ptr<Task>&& task) -> std::optional<Error> {
    sp_log("TaskPool::submit called", "TaskPool");
    // Executor interface: forward to existing addTask logic
    return this->addTask(std::move(task));
}

auto TaskPool::enableTrace(std::string const& path) -> void {
    auto nowMicros = now_micros();
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        tracePath = path;
        if (!traceEnabled.load(std::memory_order_relaxed)) {
            traceStartMicros.store(nowMicros, std::memory_order_relaxed);
            traceEvents.clear();
            traceQueueStarts.clear();
        }
    }
    traceEnabled.store(true, std::memory_order_release);
    std::vector<uint64_t> threadIds;
    {
        std::lock_guard<std::mutex> lock(workerMetaMutex);
        threadIds = workerThreadIds;
    }
    for (size_t i = 0; i < threadIds.size(); ++i) {
        if (threadIds[i] == 0) continue;
        recordTraceThreadName(threadIds[i], "TaskPool worker " + std::to_string(i));
    }
}

auto TaskPool::enableTraceNdjson(std::string const& path) -> void {
    auto nowMicros = now_micros();
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        traceNdjsonPath = path;
        if (!traceEnabled.load(std::memory_order_relaxed)) {
            traceStartMicros.store(nowMicros, std::memory_order_relaxed);
            traceEvents.clear();
            traceQueueStarts.clear();
        }
    }
    traceEnabled.store(true, std::memory_order_release);
    std::vector<uint64_t> threadIds;
    {
        std::lock_guard<std::mutex> lock(workerMetaMutex);
        threadIds = workerThreadIds;
    }
    for (size_t i = 0; i < threadIds.size(); ++i) {
        if (threadIds[i] == 0) continue;
        recordTraceThreadName(threadIds[i], "TaskPool worker " + std::to_string(i));
    }
}

TaskPool::TraceScope::TraceScope(TaskPool* pool,
                                 std::string name,
                                 std::string category,
                                 std::string path,
                                 int64_t startMicros,
                                 uint64_t threadId)
    : pool(pool),
      name(std::move(name)),
      category(std::move(category)),
      path(std::move(path)),
      startMicros(startMicros),
      threadId(threadId) {}

TaskPool::TraceScope::TraceScope(TraceScope&& other) noexcept {
    *this = std::move(other);
}

auto TaskPool::TraceScope::operator=(TraceScope&& other) noexcept -> TraceScope& {
    if (this == &other) {
        return *this;
    }
    pool = other.pool;
    name = std::move(other.name);
    category = std::move(other.category);
    path = std::move(other.path);
    startMicros = other.startMicros;
    threadId = other.threadId;
    other.pool = nullptr;
    return *this;
}

TaskPool::TraceScope::~TraceScope() {
    if (!pool) return;
    if (!pool->traceEnabled.load(std::memory_order_acquire)) return;
    auto endMicros = now_micros();
    auto startUs = static_cast<uint64_t>(
        std::max<int64_t>(0, startMicros - pool->traceStartMicros.load(std::memory_order_relaxed)));
    auto durUs = static_cast<uint64_t>(std::max<int64_t>(0, endMicros - startMicros));
    pool->recordTraceSpan(name, path, category, startUs, durUs, threadId, std::nullopt);
}

auto TaskPool::traceScope(std::string name,
                          std::string category,
                          std::string path) -> TraceScope {
    if (!traceEnabled.load(std::memory_order_acquire)) {
        return {};
    }
    auto startMicros = now_micros();
    auto threadId = current_thread_id();
    return TraceScope(this,
                      std::move(name),
                      std::move(category),
                      std::move(path),
                      startMicros,
                      threadId);
}

auto TaskPool::traceThreadName(std::string const& name) -> void {
    if (!traceEnabled.load(std::memory_order_acquire)) {
        return;
    }
    auto threadId = current_thread_id();
    recordTraceThreadName(threadId, name);
}

auto TaskPool::traceCounter(std::string name, double value) -> void {
    if (!traceEnabled.load(std::memory_order_acquire)) {
        return;
    }
    auto ts = now_micros();
    auto startUs = static_cast<uint64_t>(
        std::max<int64_t>(0, ts - traceStartMicros.load(std::memory_order_relaxed)));
    auto threadId = current_thread_id();
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        if (!traceEnabled.load(std::memory_order_relaxed)) {
            return;
        }
        traceEvents.push_back(TaskTraceEvent{.name = std::move(name),
                                             .startUs = startUs,
                                             .threadId = threadId,
                                             .counterValue = value,
                                             .hasCounter = true,
                                             .phase = 'C'});
    }
}

auto TaskPool::traceSpan(std::string name,
                         std::string category,
                         std::string path,
                         uint64_t startUs,
                         uint64_t durUs,
                         std::optional<uint64_t> threadId) -> void {
    if (!traceEnabled.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t tid = threadId.value_or(current_thread_id());
    recordTraceSpan(name, path, category, startUs, durUs, tid, std::nullopt);
}

auto TaskPool::traceNowUs() const -> uint64_t {
    if (!traceEnabled.load(std::memory_order_acquire)) {
        return 0;
    }
    auto ts = now_micros();
    auto base = traceStartMicros.load(std::memory_order_relaxed);
    if (ts < base) return 0;
    return static_cast<uint64_t>(ts - base);
}

auto TaskPool::flushTrace() -> std::optional<Error> {
    std::vector<TaskTraceEvent> eventsCopy;
    std::string tracePathCopy;
    std::string traceNdjsonPathCopy;
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        tracePathCopy = tracePath;
        traceNdjsonPathCopy = traceNdjsonPath;
        eventsCopy = traceEvents;
    }

    if (tracePathCopy.empty() && traceNdjsonPathCopy.empty()) {
        return std::nullopt;
    }

    if (!tracePathCopy.empty()) {
        auto writeTraceJson = [&](std::string const& path) -> std::optional<Error> {
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out.is_open()) {
                return Error{Error::Code::UnknownError, "Failed to open trace output: " + path};
            }
            auto pid = process_id();
            out << "{\"traceEvents\":[";
            for (size_t i = 0; i < eventsCopy.size(); ++i) {
                auto const& e = eventsCopy[i];
                if (i != 0) out << ",";
                out << "{\"name\":\"" << json_escape(e.name) << "\"";
                out << ",\"cat\":\"taskpool\"";
                out << ",\"ph\":\"" << e.phase << "\"";
                out << ",\"pid\":" << pid;
                if (e.threadId != 0 || e.phase != 'M') {
                    out << ",\"tid\":" << e.threadId;
                }
                if (e.phase == 'X') {
                    out << ",\"ts\":" << e.startUs;
                    out << ",\"dur\":" << e.durUs;
                    out << ",\"args\":{";
                    if (!e.path.empty()) {
                        out << "\"path\":\"" << json_escape(e.path) << "\"";
                    }
                    if (!e.category.empty()) {
                        if (!e.path.empty()) out << ",";
                        out << "\"category\":\"" << json_escape(e.category) << "\"";
                    }
                    if (e.hasQueueWait) {
                        if (!e.path.empty() || !e.category.empty()) out << ",";
                        out << "\"queue_wait_us\":" << e.queueWaitUs;
                    }
                    out << "}";
                } else if (e.phase == 'M') {
                    out << ",\"args\":{\"name\":\"" << json_escape(e.threadName) << "\"}";
                } else if (e.phase == 'C') {
                    out << ",\"ts\":" << e.startUs;
                    out << ",\"args\":{\"value\":" << e.counterValue << "}";
                } else if (e.phase == 'b' || e.phase == 'e') {
                    out << ",\"ts\":" << e.startUs;
                    out << ",\"id\":" << e.asyncId;
                    out << ",\"args\":{";
                    if (!e.path.empty()) {
                        out << "\"path\":\"" << json_escape(e.path) << "\"";
                    }
                    if (!e.category.empty()) {
                        if (!e.path.empty()) out << ",";
                        out << "\"category\":\"" << json_escape(e.category) << "\"";
                    }
                    out << "}";
                }
                out << "}";
            }
            out << "],\"displayTimeUnit\":\"ms\"}";
            return std::nullopt;
        };
        if (auto error = writeTraceJson(tracePathCopy)) {
            sp_log("TaskPool::flushTrace failed: " + error->message(), "TaskPool");
            return error;
        }
    }
    if (!traceNdjsonPathCopy.empty()) {
        auto writeTraceNdjson = [&](std::string const& path) -> std::optional<Error> {
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out.is_open()) {
                return Error{Error::Code::UnknownError, "Failed to open trace NDJSON output: " + path};
            }
            for (auto const& e : eventsCopy) {
                out << "{\"name\":\"" << json_escape(e.name) << "\"";
                out << ",\"phase\":\"" << e.phase << "\"";
                if (e.phase == 'X') {
                    out << ",\"start_us\":" << e.startUs;
                    out << ",\"dur_us\":" << e.durUs;
                } else if (e.phase == 'C') {
                    out << ",\"ts_us\":" << e.startUs;
                    out << ",\"value\":" << e.counterValue;
                } else if (e.phase == 'b' || e.phase == 'e') {
                    out << ",\"ts_us\":" << e.startUs;
                    out << ",\"id\":" << e.asyncId;
                }
                out << ",\"thread\":" << e.threadId;
                if (!e.path.empty()) {
                    out << ",\"path\":\"" << json_escape(e.path) << "\"";
                }
                if (!e.category.empty()) {
                    out << ",\"category\":\"" << json_escape(e.category) << "\"";
                }
                if (e.hasQueueWait) {
                    out << ",\"queue_wait_us\":" << e.queueWaitUs;
                }
                if (e.phase == 'M' && !e.threadName.empty()) {
                    out << ",\"thread_name\":\"" << json_escape(e.threadName) << "\"";
                }
                out << "}\n";
            }
            return std::nullopt;
        };
        if (auto error = writeTraceNdjson(traceNdjsonPathCopy)) {
            sp_log("TaskPool::flushTrace failed: " + error->message(), "TaskPool");
            return error;
        }
    }
    return std::nullopt;
}

auto TaskPool::shutdown() -> void {
    sp_log("TaskPool::shutdown begin", "TaskPool");
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->shuttingDown) {
            sp_log("TaskPool::shutdown already in progress", "TaskPool");
            // Already shutting down; ensure any joinable threads are joined.
            for (auto& th : this->workers) {
                if (th.joinable()) th.join();
            }
            return;
        }
        this->shuttingDown = true;
        this->taskCV.notify_all();
    }

    // Join all worker threads
    for (auto& th : this->workers) {
        if (th.joinable()) {
            sp_log("TaskPool::shutdown joining worker", "TaskPool");
            th.join();
        }
    }
    // At this point, all workers have exited; activeWorkers should be zero.
    activeWorkers = 0;
    sp_log("TaskPool::shutdown all workers joined", "TaskPool");

    // Clear workers and remaining tasks
    this->workers.clear();
    std::lock_guard<std::mutex> lock(this->mutex);
    while (!this->tasks.empty()) {
        this->tasks.pop();
    }
    sp_log("TaskPool::shutdown ends", "TaskPool");
}

auto TaskPool::size() const -> size_t {
    return this->workers.size();
}

auto TaskPool::workerFunction(size_t workerIndex) -> void {
    sp_log("TaskPool::workerFunction start", "TaskPool");
    thread_local uint64_t threadId = current_thread_id();
    {
        std::lock_guard<std::mutex> lock(workerMetaMutex);
        if (workerIndex >= workerThreadIds.size()) {
            workerThreadIds.resize(workerIndex + 1, 0);
        }
        workerThreadIds[workerIndex] = threadId;
    }
    if (traceEnabled.load(std::memory_order_acquire)) {
        recordTraceThreadName(threadId, "TaskPool worker " + std::to_string(workerIndex));
    }
    while (true) {
        std::string                         notificationPath;
        std::weak_ptr<NotificationSink>     notifier;
        std::weak_ptr<Task>                 task;
        {
            std::unique_lock<std::mutex> lock(mutex);
            taskCV.wait(lock, [this] { return this->shuttingDown || !this->tasks.empty(); });

            if (this->shuttingDown && this->tasks.empty()) {
                sp_log("TaskPool::workerFunction received shutdown with empty queue", "TaskPool");
                break;
            }

            if (!this->tasks.empty()) {
                sp_log("TaskPool::workerFunction dequeuing task", "TaskPool");
                task = std::move(tasks.front());
                tasks.pop();
            }
        }

        if (auto strongTask = task.lock()) {
            sp_log("TaskPool::workerFunction Task locked successfully", "TaskPool");
            notificationPath = strongTask->notificationPath;
            notifier         = strongTask->notifier;

            ++activeTasks;
            auto traceStart = int64_t{0};
            std::optional<uint64_t> queueWaitUs;
            auto queueStart = takeTraceQueueStart(strongTask.get());
            std::string displayLabel = task_display_label(*strongTask, notificationPath);
            if (traceEnabled.load(std::memory_order_acquire)) {
                traceStart = now_micros();
            }
            if (queueStart) {
                auto traceStartBaseline = traceStartMicros.load(std::memory_order_relaxed);
                auto waitEndUs = static_cast<uint64_t>(
                    std::max<int64_t>(0, traceStart - traceStartBaseline));
                recordTraceAsync(format_task_label("Wait", displayLabel),
                                 notificationPath,
                                 "queue",
                                 waitEndUs,
                                 'e',
                                 queueStart->second);
                if (traceStart > 0) {
                    queueWaitUs = static_cast<uint64_t>(std::max<int64_t>(0, traceStart - queueStart->first));
                }
            }
            try {
                sp_log("Transitioning to running", "TaskPool");
                strongTask->transitionToRunning();
                sp_log("Executing task function", "TaskPool");
                strongTask->function(*strongTask, false);
                sp_log("Marking task completed", "TaskPool");
                strongTask->markCompleted();
            } catch (...) {
                strongTask->markFailed();
                sp_log("Exception in running Task", "Error", "Exception");
            }
            if (traceStart != 0 && traceEnabled.load(std::memory_order_acquire)) {
                auto traceEnd = now_micros();
                auto startUs = static_cast<uint64_t>(
                    std::max<int64_t>(0, traceStart - traceStartMicros.load(std::memory_order_relaxed)));
                auto durUs = static_cast<uint64_t>(std::max<int64_t>(0, traceEnd - traceStart));
                recordTraceSpan(displayLabel,
                                notificationPath,
                                std::string{},
                                startUs,
                                durUs,
                                threadId,
                                queueWaitUs);
            }
            --activeTasks;
        } else {
            sp_log("TaskPool::workerFunction Failed to lock task - references lost", "TaskPool");
        }
        if (!notificationPath.empty()) {
            sp_log("TaskPool::workerFunction notifying path: " + notificationPath, "TaskPool");
            if (!shuttingDown) {
                if (auto sink = notifier.lock()) {
                    sink->notify(notificationPath);
                } else {
                    sp_log("TaskPool::workerFunction notifier expired; skipping notify", "TaskPool");
                }
            } else {
                sp_log("TaskPool::workerFunction skipping notify due to shutdown", "TaskPool");
            }
        }
    }

    // Signal this worker is exiting
    sp_log("TaskPool::workerFunction exit", "TaskPool");
    --activeWorkers;
}

auto TaskPool::recordTraceSpan(std::string const& name,
                               std::string const& path,
                               std::string const& category,
                               uint64_t startUs,
                               uint64_t durUs,
                               uint64_t threadId,
                               std::optional<uint64_t> queueWaitUs) -> void {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    TaskTraceEvent event{.name = name,
                         .path = path,
                         .category = category,
                         .startUs = startUs,
                         .durUs = durUs,
                         .threadId = threadId,
                         .phase = 'X'};
    if (queueWaitUs) {
        event.queueWaitUs = *queueWaitUs;
        event.hasQueueWait = true;
    }
    traceEvents.push_back(std::move(event));
}

auto TaskPool::recordTraceAsync(std::string const& name,
                                std::string const& path,
                                std::string const& category,
                                uint64_t tsUs,
                                char phase,
                                uint64_t asyncId) -> void {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    traceEvents.push_back(TaskTraceEvent{.name = name,
                                         .path = path,
                                         .category = category,
                                         .startUs = tsUs,
                                         .threadId = 0,
                                         .asyncId = asyncId,
                                         .phase = phase});
}

auto TaskPool::recordTraceThreadName(uint64_t threadId, std::string const& name) -> void {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (traceNamedThreads.find(threadId) != traceNamedThreads.end()) {
        return;
    }
    traceNamedThreads.insert(threadId);
    traceEvents.push_back(TaskTraceEvent{.name = "thread_name",
                                         .threadName = name,
                                         .threadId = threadId,
                                         .phase = 'M'});
}

auto TaskPool::recordTraceQueueStart(Task* task,
                                     std::string const& label,
                                     std::string const& path) -> void {
    if (!task) return;
    auto nowMicros = now_micros();
    auto traceStart = traceStartMicros.load(std::memory_order_relaxed);
    auto tsUs = static_cast<uint64_t>(std::max<int64_t>(0, nowMicros - traceStart));
    auto asyncId = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(task));
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        traceQueueStarts[task] = {nowMicros, asyncId};
    }
    std::string displayLabel = label.empty() ? path : label;
    recordTraceAsync(format_task_label("Wait", displayLabel), path, "queue", tsUs, 'b', asyncId);
}

auto TaskPool::takeTraceQueueStart(Task* task) -> std::optional<std::pair<int64_t, uint64_t>> {
    if (!task) return std::nullopt;
    std::lock_guard<std::mutex> lock(traceMutex);
    auto it = traceQueueStarts.find(task);
    if (it == traceQueueStarts.end()) return std::nullopt;
    auto value = it->second;
    traceQueueStarts.erase(it);
    return value;
}

} // namespace SP
