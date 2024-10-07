#include "TaskPool.hpp"
#include <stdexcept>

namespace SP {

TaskPool& TaskPool::Instance() {
    static TaskPool instance;
    return instance;
}

TaskPool::TaskPool(size_t threadCount) : stop(false), availableThreads(0) {
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

auto TaskPool::addTask(Task const& task) -> void {
    std::lock_guard<std::mutex> lock(taskMutex);
    if (task.executionOptions.location == ExecutionOptions::Location::MainThread)
        tasksMainThread.emplace(task);
    else
        tasks.emplace(task);
    taskCV.notify_one();
}

/*void TaskPool::addTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        tasks.emplace(Task{std::move(task)});
    }
    taskCV.notify_one();
}*/

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

size_t TaskPool::size() const {
    return workers.size();
}

void TaskPool::workerFunction() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            availableThreads++;
            taskCV.wait(lock, [this]() { return this->stop || !this->tasks.empty() || this->immediateTask.has_value(); });

            if (this->stop && this->tasks.empty() && !this->immediateTask.has_value()) {
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

        /*if (std::holds_alternative<std::function<void()>>(task.callable)) {
            std::get<std::function<void()>>(task.callable)();
        } else if (std::holds_alternative<FunctionPointerTask>(task.callable)) {
            auto functionPointerTask = std::get<FunctionPointerTask>(task.callable);
            functionPointerTask(task.functionPointer);
        }*/
        task.taskExecutor(task);
    }
}

} // namespace SP