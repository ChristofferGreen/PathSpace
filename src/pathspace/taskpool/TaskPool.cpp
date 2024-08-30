#include "TaskPool.hpp"
#include <stdexcept>

namespace SP {

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool(size_t threadCount) : stop(false) {
    if (threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }
    for (size_t i = 0; i < threadCount; ++i) {
        workers.emplace_back(&TaskPool::workerFunction, this);
    }
}

TaskPool::~TaskPool() {
    shutdown();
}

void TaskPool::addTask(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        tasks.push(std::move(task));
    }
    condition.notify_one();
}

void TaskPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TaskPool::resize(size_t newSize) {
    if (newSize == workers.size()) return;

    if (newSize < workers.size()) {
        // Reduce the number of threads
        size_t threadsToRemove = workers.size() - newSize;
        for (size_t i = 0; i < threadsToRemove; ++i) {
            addTask([this]() { throw std::runtime_error("Thread removal"); });
        }
    } else {
        // Increase the number of threads
        size_t threadsToAdd = newSize - workers.size();
        for (size_t i = 0; i < threadsToAdd; ++i) {
            workers.emplace_back(&TaskPool::workerFunction, this);
        }
    }
}

size_t TaskPool::size() const {
    return workers.size();
}

void TaskPool::workerFunction() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this]() { return stop || !tasks.empty(); });
            if (stop && tasks.empty()) {
                return;
            }
            task = std::move(tasks.front());
            tasks.pop();
        }
        try {
            task();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "Thread removal") {
                return;
            }
            // Handle or log other exceptions
        } catch (...) {
            // Handle or log unknown exceptions
        }
    }
}

} // namespace SP