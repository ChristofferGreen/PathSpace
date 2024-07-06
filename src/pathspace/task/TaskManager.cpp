#include "TaskManager.hpp"
#include "pathspace/PathSpace.hpp"
#include "pathspace/path/ConcretePath.hpp"

namespace SP {

static int const pool_size = 2 * std::thread::hardware_concurrency();
static int const max_threads = 256 * pool_size;

TaskManager& TaskManager::getInstance() {
    static TaskManager instance;
    return instance;
}

TaskManager::TaskManager() : memoryPool(sizeof(Task), 1024) {
    this->startWorkerThreads();
}

TaskManager::~TaskManager() {
    this->shutdown();
}

auto TaskManager::scheduleTask(ConcretePathString const &path, std::unique_ptr<ExecutionBase, PoolDeleter<ExecutionBase>> exec, PathSpace &space) -> void {
    {
        std::unique_lock<std::mutex> lock(taskQueueMutex);
        void* taskMemory = memoryPool.allocate();
        auto task = std::unique_ptr<Task, PoolDeleter<Task>>(
            new (taskMemory) Task(path, std::move(exec), space),
            PoolDeleter<Task>(memoryPool)
        );
        taskQueue.push(std::move(task));
    }
    taskQueueCondition.notify_one();
}

auto TaskManager::shutdown() -> void {
    {
        std::unique_lock<std::mutex> lock(this->taskQueueMutex);
        this->shuttingDown = true;
    }
    this->taskQueueCondition.notify_all();
    for (auto &thread : this->workerThreads)
        if (thread.joinable())
            thread.join();
}

auto TaskManager::startWorkerThreads() -> void {
    for (size_t i = 0; i < pool_size; ++i)
        workerThreads.emplace_back(&TaskManager::workerThread, this);
}

auto TaskManager::workerThread() -> void {
    while (true) {
        std::optional<std::unique_ptr<Task, PoolDeleter<Task>>> task;
        {
            std::unique_lock<std::mutex> lock(taskQueueMutex);
            taskQueueCondition.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !taskQueue.empty() || shuttingDown;
            });

            if (shuttingDown && taskQueue.empty()) {
                return;
            }

            if (!taskQueue.empty()) {
                task = std::move(taskQueue.front());
                taskQueue.pop();
            } else {
                continue;
            }
        }

        if (task) {
            std::atomic<bool> alive{true};
            task.value()->exec->execute(task.value()->path, *task.value()->space, alive);

            if (alive.load()) {
                //(*task)->space.removeExecution((*task)->path); // Remove the execution after it completes
            }
        }
    }
}

} // namespace SP