#pragma once
#include "pathspace/path/ConcretePath.hpp"
#include "pathspace/task/Task.hpp"
#include "pathspace/task/ExecutionBase.hpp"
#include "pathspace/task/MemoryPool.hpp"
#include <mutex>
#include <thread>

namespace SP {

struct PathSpace;

class TaskManager {
public:
    TaskManager();
    ~TaskManager();

    static TaskManager& getInstance();

    TaskManager(TaskManager const&) = delete;
    auto operator=(TaskManager const&) -> void = delete;

    auto scheduleTask(ConcretePathString const &path,  std::unique_ptr<ExecutionBase, PoolDeleter<ExecutionBase>> exec, PathSpace &space) -> void;
    auto shutdown() -> void;
    auto startWorkerThreads() -> void;
    auto workerThread() -> void;

private:
    std::queue<std::unique_ptr<Task, PoolDeleter<Task>>> taskQueue;
    std::vector<std::thread> workerThreads;
    std::mutex taskQueueMutex;
    std::condition_variable taskQueueCondition;
    bool shuttingDown = false;
    MemoryPool memoryPool;
};

}