#pragma once
#include "Task.hpp"
#include "Executor.hpp"
#include "core/Error.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <optional>

namespace SP {


class TaskPool : public Executor {
public:
    explicit TaskPool(size_t threadCount = std::thread::hardware_concurrency());
    ~TaskPool();

    static TaskPool& Instance();

    TaskPool(TaskPool const&)                    = delete;
    auto operator=(TaskPool const&) -> TaskPool& = delete;

    auto submit(std::weak_ptr<Task>&& task) -> std::optional<Error> override;
    auto addTask(std::weak_ptr<Task>&& task) -> std::optional<Error>;
    auto shutdown() -> void override;
    auto size() const -> size_t override;

private:
    auto workerFunction() -> void;

    std::vector<std::jthread>       workers;
    std::queue<std::weak_ptr<Task>> tasks;
    std::mutex                      mutex;
    std::condition_variable         taskCV;
    std::atomic<bool>               shuttingDown{false};
    std::atomic<size_t>             activeWorkers{0};
    std::atomic<size_t>             activeTasks{0};
};

} // namespace SP