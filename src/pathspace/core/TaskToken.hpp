#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>

namespace SP {

class PathSpace;

class TaskToken {
private:
    std::atomic<bool> valid{true};
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<size_t> outstandingTasks{0};
    std::atomic<bool> everUsed{false};

public:
    bool isValid() const {
        return valid.load(std::memory_order_seq_cst);
    }

    void invalidate() {
        valid.store(false, std::memory_order_seq_cst);
    }

    void registerTask() {
        everUsed.store(true, std::memory_order_release);
        outstandingTasks.fetch_add(1, std::memory_order_seq_cst);
    }

    void unregisterTask() {
        if (outstandingTasks.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    bool wasEverUsed() const {
        return everUsed.load(std::memory_order_acquire);
    }

    size_t getTaskCount() const {
        return outstandingTasks.load(std::memory_order_seq_cst);
    }

    void waitForTasks() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return outstandingTasks.load(std::memory_order_seq_cst) == 0; });
    }
};

} // namespace SP