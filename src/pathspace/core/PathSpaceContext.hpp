#pragma once

#include "core/WaitMap.hpp"
#include "core/NotificationSink.hpp"
#include "task/Executor.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace SP {

/**
 * PathSpaceContext — shared runtime context for PathSpace trees
 *
 * Responsibilities:
 * - Own the wait/notify registry (currently backed by WaitMap; upgradable to WatchRegistry).
 * - Provide a shared NotificationSink for lifetime-safe notifications from tasks.
 * - Expose an injected Executor for task scheduling (no hard dependency on singletons).
 * - Coordinate shutdown signaling and mass wakeups.
 *
 * Integration notes:
 * - PathSpaceBase getters (getNotificationSink/getExecutor) can delegate to this context.
 * - Nested PathSpace instances should share the same Context and carry a distinct path prefix.
 */
class PathSpaceContext final {
public:
    using WaitGuard = WaitMap::Guard;
    using WaitType = WaitMap;

    explicit PathSpaceContext(Executor* exec = nullptr)
        : executorPtr(exec)
        , waitRegistry(std::make_unique<WaitMap>())
        , shuttingDown(false) {}

    ~PathSpaceContext() = default;

    // ----- Executor management -----

    // Set or replace the executor used for task submission.
    void setExecutor(Executor* exec) noexcept {
        executorPtr = exec;
    }

    // Current executor (may be nullptr if not configured).
    Executor* executor() const noexcept {
        return executorPtr;
    }

    // ----- Notification sink management -----

    // Install a shared NotificationSink implementation.
    void setSink(std::shared_ptr<NotificationSink> sinkIn) {
        std::lock_guard<std::mutex> lg(this->mutex);
        this->sink = std::move(sinkIn);
    }

    // Acquire a weak handle to the NotificationSink for safe cross-thread notifications.
    std::weak_ptr<NotificationSink> getSink() const {
        std::lock_guard<std::mutex> lg(this->mutex);
        return std::weak_ptr<NotificationSink>(this->sink);
    }

    // Invalidate the sink so late notifications are safely dropped.
    void invalidateSink() {
        std::lock_guard<std::mutex> lg(this->mutex);
        this->sink.reset();
    }

    // ----- Wait/notify -----

    // Wait for notifications on a concrete or glob path.
    // Guard exposes waitRegistryuntil(...) while holding the registry's lock.
    WaitGuard wait(std::string_view path) {
        ensureWait();
        return waitRegistry->wait(path);
    }

    // Notify waiters (path may be concrete or glob; semantics provided by underlying registry).
    void notify(std::string_view path) {
        ensureWait();
        waitRegistry->notify(path);
        std::shared_ptr<NotificationSink> sinkCopy;
        {
            std::lock_guard<std::mutex> lg(this->mutex);
            if (!this->sink || this->notifyingSink) {
                return;
            }
            this->notifyingSink = true;
            sinkCopy            = this->sink;
        }
        std::string pathCopy(path);
        sinkCopy->notify(pathCopy);
        {
            std::lock_guard<std::mutex> lg(this->mutex);
            this->notifyingSink = false;
        }
    }

    // Notify all waiters (used during shutdown and broad updates).
    void notifyAll() {
        ensureWait();
        waitRegistry->notifyAll();
    }

    // Clear all registered waiters (generally called when clearing the tree).
    void clearWaits() {
        ensureWait();
        waitRegistry->clear();
    }

    // ----- Shutdown coordination -----

    // Mark the context as shutting down and wake all waiters.
    void shutdown() {
        this->shuttingDown.store(true, std::memory_order_release);
        notifyAll();
    }

    // Indicates whether shutdown has been initiated.
    bool isShuttingDown() const noexcept {
        return this->shuttingDown.load(std::memory_order_acquire);
    }

    // Whether there are any registered waiters (concrete or glob)
    bool hasWaiters() const noexcept {
        // ensureWait is non-const; use const_cast for lazy init
        const_cast<PathSpaceContext*>(this)->ensureWait();
        return this->waitRegistry->hasWaiters();
    }

private:
    void ensureWait() {
        if (!waitRegistry) {
            // Late initialization in case context was default-constructed without wait registry
            waitRegistry = std::make_unique<WaitMap>();
        }
    }

    // Shared notification sink for lifetime-safe delivery.
    mutable std::mutex                 mutex;
    std::shared_ptr<NotificationSink>  sink;
    bool                               notifyingSink = false;

    // Preferred executor for task scheduling.
    Executor*                          executorPtr = nullptr;

    // Wait/notify registry (backed by WaitMap for glob/concrete semantics).
    std::unique_ptr<WaitMap>           waitRegistry;

    // Shutdown flag visible across threads.
    std::atomic<bool>                  shuttingDown;
};

} // namespace SP
