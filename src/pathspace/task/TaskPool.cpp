#include "TaskPool.hpp"
#include "core/NotificationSink.hpp"
#include "log/TaggedLogger.hpp"

namespace SP {

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
    for (size_t i = 0; i < threadCount; ++i) {
        try {
            sp_log("TaskPool::TaskPool spawning worker", "TaskPool");
            workers.emplace_back(&TaskPool::workerFunction, this);
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
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!shuttingDown) {
            if (auto locked = task.lock()) {
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
            } else {
                sp_log("TaskPool::addTask task expired before enqueue", "TaskPool");
                return Error{Error::Code::UnknownError, "Task expired before enqueue"};
            }
        } else {
            sp_log("TaskPool::addTask refused: shutting down", "TaskPool");
            return Error{Error::Code::UnknownError, "Executor shutting down"};
        }
    }
    return std::nullopt;
}

auto TaskPool::submit(std::weak_ptr<Task>&& task) -> std::optional<Error> {
    sp_log("TaskPool::submit called", "TaskPool");
    // Executor interface: forward to existing addTask logic
    return this->addTask(std::move(task));
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

auto TaskPool::workerFunction() -> void {
    sp_log("TaskPool::workerFunction start", "TaskPool");
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

} // namespace SP