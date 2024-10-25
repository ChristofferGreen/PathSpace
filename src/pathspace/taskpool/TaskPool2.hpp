#pragma once
#include "Task2.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace SP {

struct PathSpace;

class TaskPool2 {
public:
    TaskPool2(size_t threadCount = std::thread::hardware_concurrency());
    ~TaskPool2();

    static TaskPool2& Instance();

    TaskPool2(TaskPool2 const&) = delete;
    auto operator=(TaskPool2 const&) -> TaskPool2& = delete;

    auto addTask(std::weak_ptr<Task2>&& task) -> void;

    auto shutdown() -> void;
    auto size() const -> size_t;

private:
    auto workerFunction() -> void;

    std::vector<std::jthread> workers;
    std::queue<std::weak_ptr<Task2>> tasks;
    mutable std::mutex mutex;
    std::condition_variable taskCV;
    std::atomic<bool> stop;
};

} // namespace SP