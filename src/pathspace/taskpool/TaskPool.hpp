#pragma once
#include <mutex>
#include <queue>
#include <thread>

namespace SP {

struct PathSpace;

struct TaskPool {
    TaskPool();
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> void = delete;

    auto add(void (*executionWrapper)(void* const functionPointer, void* returnData),
             void* const functionPointer,
             void* returnData) -> void;

private:
    // std::queue<std::unique_ptr<Task, PoolDeleter<Task>>> taskQueue;
    std::vector<std::thread> workerThreads;
    std::mutex taskQueueMutex;
    bool shuttingDown = false;
};

} // namespace SP