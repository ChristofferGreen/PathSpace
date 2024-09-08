#include "TaskPool.hpp"
#include <stdexcept>

namespace SP {

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool(size_t threadCount)
    : stop(false), availableThreads(0) {
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
        std::lock_guard<std::mutex> lock(taskMutex);
        tasks.emplace(Task{std::move(task)});
    }
    taskCV.notify_one();
}

auto TaskPool::addTask(FunctionPointerTask task, void* const functionPointer, void* returnData, ConcretePathString const& path, PathSpace const& space) -> void {
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        tasks.emplace(Task{task, functionPointer, returnData});
    }
    taskCV.notify_one();
}
/*
auto TaskPool::addFunctionPointerTaskDirect(FunctionPointerTask task, void* const functionPointer, void* returnData) -> void {
    std::unique_lock<std::mutex> lock(taskMutex);
    if (availableThreads > 0) {
        immediateTask = Task{task, functionPointer, returnData};
    } else {
        tasks.emplace(Task{task, functionPointer, returnData});
    }
    lock.unlock();
    taskCV.notify_one();
}*/

void TaskPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(taskMutex);
        stop = true;
    }
    taskCV.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TaskPool::resize(size_t newSize) {
    if (newSize == workers.size())
        return;

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
        Task task;
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            availableThreads++;
            taskCV.wait(lock, [this]() { return stop || !tasks.empty() || immediateTask.has_value(); });

            if (stop && tasks.empty() && !immediateTask.has_value()) {
                availableThreads--;
                return;
            }

            if (immediateTask.has_value()) {
                task = std::move(*immediateTask);
                immediateTask.reset();
            } else if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
            }
            availableThreads--;
        }

        // Execute the task
        try {
            if (std::holds_alternative<std::function<void()>>(task.callable)) {
                std::get<std::function<void()>>(task.callable)();
            } else if (std::holds_alternative<FunctionPointerTask>(task.callable)) {
                auto functionPointerTask = std::get<FunctionPointerTask>(task.callable);
                functionPointerTask(task.functionPointer, task.returnData);
            }
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