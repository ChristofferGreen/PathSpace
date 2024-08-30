#pragma once
#include "taskpool/Task.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace SP {

struct TaskPool {
    TaskPool(size_t threadCount = 0);
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    void addTask(std::function<void()> task);
    void addTask(FunctionPointerTask task, void* const functionPointer, void* returnData);

    void shutdown();

    void resize(size_t newSize);

    size_t size() const;

private:
    void workerFunction();

    std::vector<std::thread> workers;
    std::queue<Task> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

} // namespace SP