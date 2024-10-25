#pragma once
#include "Task.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace SP {

struct PathSpace;

class TaskPool {
public:
    TaskPool(size_t threadCount = std::thread::hardware_concurrency());
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    auto addTask(std::weak_ptr<Task>&& task) -> void;

    auto shutdown() -> void;
    auto size() const -> size_t;

private:
    auto workerFunction() -> void;

    std::vector<std::jthread> workers;
    std::queue<std::weak_ptr<Task>> tasks;
    mutable std::mutex mutex;
    std::condition_variable taskCV;
    std::atomic<bool> stop;
};

} // namespace SP