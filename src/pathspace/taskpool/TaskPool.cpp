#include "TaskPool.hpp"

namespace SP {

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool(size_t threadCount) {
    for (size_t i = 0; i < threadCount; ++i) {
        workers.emplace_back(&TaskPool::workerFunction, this);
    }
}

TaskPool::~TaskPool() {
    shutdown();
}

auto TaskPool::addTask(std::weak_ptr<Task>&& task) -> void {
    std::lock_guard<std::mutex> lock(mutex);
    if (!stop) {
        tasks.push(std::move(task));
        taskCV.notify_one();
    }
}

void TaskPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop) {
            return;
        }
        stop = true;
        taskCV.notify_all();
    }

    // Wait for active tasks to complete
    while (activeWorkers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    workers.clear();
}

size_t TaskPool::size() const {
    return workers.size();
}

void TaskPool::workerFunction() {
    activeWorkers.fetch_add(1, std::memory_order_release);

    while (true) {
        std::weak_ptr<Task> weakTask;
        {
            std::unique_lock<std::mutex> lock(mutex);
            taskCV.wait(lock, [this]() { return stop || !tasks.empty(); });

            if (stop && tasks.empty()) {
                break;
            }

            if (!tasks.empty()) {
                weakTask = std::move(tasks.front());
                tasks.pop();
            }
        }

        // Lock the weak_ptr and copy the function to prevent races
        if (auto task = weakTask.lock()) {
            if (auto fn = task->function) {
                try {
                    fn(*task, nullptr, false);
                } catch (...) {
                    // Log error but continue processing
                }
            }
        }
    }

    activeWorkers.fetch_sub(1, std::memory_order_release);
}

} // namespace SP