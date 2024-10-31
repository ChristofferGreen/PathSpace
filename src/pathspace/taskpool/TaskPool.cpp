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
    shutdown();
}

auto TaskPool::addTask(std::weak_ptr<Task>&& task) -> void {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!shuttingDown) {
            tasks.push(std::move(task));
            taskCV.notify_one();
        }
    }
}

auto TaskPool::shutdown() -> void {
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
}

auto TaskPool::size() const -> size_t {
    return this->workers.size();
}

auto TaskPool::workerFunction() -> void {
    while (true) {
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
            if (auto fn = strongTask->function) {
                try {
                    strongTask->state.transitionToRunning();
                    fn(*strongTask, false);
                    strongTask->state.markCompleted();
                    strongTask->space->waitMap.notify(strongTask->notificationPath);
                } catch (...) {
                    sp_log("Exception in running Task", "Error", "Exception");
                }
            }
        }
    }

    --activeWorkers;
}

} // namespace SP