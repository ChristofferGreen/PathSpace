#pragma once
#include "Task.hpp"
#include "path/ConcretePath.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

namespace SP {

struct PathSpace;

class TaskPool {
public:
    TaskPool(size_t threadCount = 0);
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&) = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    auto addTask(Task const& task) -> void;
    // auto addTask(std::function<void()> task) -> void;

    auto shutdown() -> void;
    auto size() const -> size_t;

private:
    auto workerFunction() -> void;

    std::vector<std::thread> workers;
    std::queue<Task> tasks;
    std::queue<Task> tasksMainThread;
    std::mutex taskMutex;
    std::condition_variable taskCV;
    std::atomic<bool> stop;
    std::atomic<size_t> availableThreads;
    std::optional<Task> immediateTask;
};

} // namespace SP