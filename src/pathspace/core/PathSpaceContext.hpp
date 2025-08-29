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
        : executor_(exec)
        , wait_(std::make_unique<WaitMap>())
        , shuttingDown_(false) {}

    ~PathSpaceContext() = default;

    // ----- Executor management -----

    // Set or replace the executor used for task submission.
    void setExecutor(Executor* exec) noexcept {
        executor_ = exec;
    }

    // Current executor (may be nullptr if not configured).
    Executor* executor() const noexcept {
        return executor_;
    }

    // ----- Notification sink management -----

    // Install a shared NotificationSink implementation.
    void setSink(std::shared_ptr<NotificationSink> sink) {
        std::lock_guard<std::mutex> lg(mutex_);
        sink_ = std::move(sink);
    }

    // Acquire a weak handle to the NotificationSink for safe cross-thread notifications.
    std::weak_ptr<NotificationSink> getSink() const {
        std::lock_guard<std::mutex> lg(mutex_);
        return std::weak_ptr<NotificationSink>(sink_);
    }

    // Invalidate the sink so late notifications are safely dropped.
    void invalidateSink() {
        std::lock_guard<std::mutex> lg(mutex_);
        sink_.reset();
    }

    // ----- Wait/notify -----

    // Wait for notifications on a concrete or glob path.
    // Guard exposes wait_until(...) while holding the registry's lock.
    WaitGuard wait(std::string_view path) {
        ensureWait_();
        return wait_->wait(path);
    }

    // Notify waiters (path may be concrete or glob; semantics provided by underlying registry).
    void notify(std::string_view path) {
        ensureWait_();
        wait_->notify(path);
    }

    // Notify all waiters (used during shutdown and broad updates).
    void notifyAll() {
        ensureWait_();
        wait_->notifyAll();
    }

    // Clear all registered waiters (generally called when clearing the tree).
    void clearWaits() {
        ensureWait_();
        wait_->clear();
    }

    // ----- Shutdown coordination -----

    // Mark the context as shutting down and wake all waiters.
    void shutdown() {
        shuttingDown_.store(true, std::memory_order_release);
        notifyAll();
    }

    // Indicates whether shutdown has been initiated.
    bool isShuttingDown() const noexcept {
        return shuttingDown_.load(std::memory_order_acquire);
    }

    // Whether there are any registered waiters (concrete or glob)
    bool hasWaiters() const noexcept {
        // ensureWait_ is non-const; use const_cast for lazy init
        const_cast<PathSpaceContext*>(this)->ensureWait_();
        return wait_->hasWaiters();
    }

private:
    void ensureWait_() {
        if (!wait_) {
            // Late initialization in case context was default-constructed without wait registry
            wait_ = std::make_unique<WaitMap>();
        }
    }

    // Shared notification sink for lifetime-safe delivery.
    mutable std::mutex                 mutex_;
    std::shared_ptr<NotificationSink>  sink_;

    // Preferred executor for task scheduling.
    Executor*                          executor_ = nullptr;

    // Wait/notify registry (backed by WaitMap for glob/concrete semantics).
    std::unique_ptr<WaitMap>           wait_;

    // Shutdown flag visible across threads.
    std::atomic<bool>                  shuttingDown_;
};

} // namespace SP