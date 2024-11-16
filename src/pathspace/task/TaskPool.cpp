#include "TaskPool.hpp"
#include "PathSpace.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool(size_t threadCount) {
    activeWorkers = threadCount;
    for (size_t i = 0; i < threadCount; ++i) {
        workers.emplace_back(&TaskPool::workerFunction, this);
    }
}

TaskPool::~TaskPool() {
    sp_log("TaskPool::~TaskPool", "TaskPool");
    shutdown();
}

auto TaskPool::addTask(std::weak_ptr<Task>&& task) -> std::optional<Error> {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!shuttingDown) {
            if (!task.lock()->tryStart())
                return Error{Error::Code::UnknownError, "Failed to start lazy execution"};
            tasks.push(std::move(task));
            taskCV.notify_one();
        }
    }
    return std::nullopt;
}

auto TaskPool::shutdown() -> void {
    sp_log("TaskPool::shutdown", "TaskPool");
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->shuttingDown) {
            return;
        }
        this->shuttingDown = true;
        this->taskCV.notify_all();
    }

    // Wait for workers to finish
    while (this->activeWorkers > 0) {
        std::this_thread::yield();
    }

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
    while (true) {
        GlobPathString      notificationPath;
        PathSpace*          space = nullptr;
        std::weak_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex);
            taskCV.wait(lock, [this] { return this->shuttingDown || !this->tasks.empty(); });

            if (this->shuttingDown && this->tasks.empty()) {
                break;
            }

            if (!this->tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
            }
        }

        if (auto strongTask = task.lock()) {
            sp_log("TaskPool::workerFunction Task locked successfully", "TaskPool");
            notificationPath = strongTask->notificationPath;
            space            = strongTask->space;

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
        if (!notificationPath.empty() && space) {
            std::lock_guard<std::mutex> lock(mutex);
            sp_log("Notifying path: " + std::string(notificationPath.getPath()), "TaskPool");
            if (!shuttingDown)
                space->waitMap.notify(notificationPath);
        }
    }

    --activeWorkers;
}

} // namespace SP