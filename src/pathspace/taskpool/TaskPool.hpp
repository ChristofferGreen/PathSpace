#pragma once
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include <condition_variable>
#include <atomic>

namespace SP {

class TaskPool {
public:
    // Get the singleton instance of TaskPool
    static TaskPool& Instance();

    // Delete copy constructor and assignment operator
    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    // Add a task to the pool
    void addTask(std::function<void()> task);

    // Shutdown the task pool
    void shutdown();

    // Resize the thread pool
    void resize(size_t newSize);

    // Get the current size of the thread pool
    size_t size() const;

private:
    TaskPool(size_t threadCount = 0);
    ~TaskPool();

    void workerFunction();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

} // namespace SP