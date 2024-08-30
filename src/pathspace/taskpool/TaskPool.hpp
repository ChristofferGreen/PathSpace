#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <variant>
#include <vector>

namespace SP {

class TaskPool {
public:
    using FunctionPointerTask = void (*)(void* const, void*);

    struct Task {
        std::variant<std::function<void()>, FunctionPointerTask> callable;
        void* functionPointer = nullptr;
        void* returnData = nullptr;
    };

    // Get the singleton instance of TaskPool
    static TaskPool& Instance();

    // Delete copy constructor and assignment operator
    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    // Add a task to the pool
    void addTask(std::function<void()> task);
    void addTask(FunctionPointerTask task, void* const functionPointer, void* returnData);

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
    std::queue<Task> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

} // namespace SP