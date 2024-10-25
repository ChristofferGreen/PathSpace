#include "TaskPool2.hpp"
#include "utils/TaggedLogger.hpp"

#include <stdexcept>

namespace SP {

TaskPool2& TaskPool2::Instance() {
    static TaskPool2 instance;
    return instance;
}

TaskPool2::TaskPool2(size_t threadCount) : stop(false) {
    for (size_t i = 0; i < threadCount; ++i)
        workers.emplace_back(&TaskPool2::workerFunction, this);
}

TaskPool2::~TaskPool2() {
    shutdown();
}

auto TaskPool2::addTask(std::weak_ptr<Task2>&& task) -> void {
    std::lock_guard<std::mutex> lock(this->mutex);
    tasks.emplace(std::move(task));
    taskCV.notify_one();
}

void TaskPool2::shutdown() {
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        if (stop)
            return; // Prevent multiple shutdowns
        this->stop = true;
        this->taskCV.notify_all();
    }

    // Clear remaining tasks
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        while (!tasks.empty())
            tasks.pop();
    }

    this->workers.clear();
}

size_t TaskPool2::size() const {
    return workers.size();
}

void TaskPool2::workerFunction() {
    while (true) {
        // 1. Aquire a task
        std::weak_ptr<Task2> taskWeakPtr;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            taskCV.wait(lock, [this]() { return this->stop || !this->tasks.empty(); });

            if (this->stop && this->tasks.empty())
                break;

            if (!tasks.empty()) {
                taskWeakPtr = std::move(tasks.front());
                tasks.pop();
            } else {
                continue;
            }
        }
        // 2. Launch task
        if (auto task = taskWeakPtr.lock())
            if (task->function)
                task->function(*task.get(), nullptr, false);
    }
}

} // namespace SP