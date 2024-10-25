#pragma once
#include "taskpool/Task.hpp"

#include <cstdint>
#include <memory>

namespace SP {

struct TaskStorage {
    TaskStorage() = default;

    // Store a task and get its ID
    uint64_t store(std::shared_ptr<Task> task) {
        uint64_t id = next_task_id++;
        std::lock_guard<std::mutex> lock(mutex);
        tasks[id] = std::move(task);
        return id;
    }

    // Remove a specific task
    void remove(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.erase(id);
    }

    // Clear all tasks
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.clear();
    }

    // Get task count (for debugging/monitoring)
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return tasks.size();
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<Task>> tasks;
    std::atomic<uint64_t> next_task_id{0};
};

} // namespace SP