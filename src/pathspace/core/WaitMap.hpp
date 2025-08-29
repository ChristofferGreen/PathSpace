#pragma once
#include "path/TransparentString.hpp"

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>

namespace SP {

struct WaitMap {
    struct Guard {
        Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock);

        // When a Guard is destroyed, decrement active waiter count if this Guard performed a wait.
        ~Guard() {
            if (counted) {
                auto prev = waitMap.activeWaiterCount.fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    // Wake clear() waiters that might be waiting for all active waiters to drain.
                    std::lock_guard<std::mutex> lg(waitMap.activeWaitersMutex);
                    waitMap.noActiveWaitersCv.notify_all();
                }
            }
        }

        auto wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status;
        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> timeout, Pred pred) {
            if (!cvPtr) cvPtr = &waitMap.getCv(path);
            // Begin waiter tracking on first wait call in this Guard (template path only).
            if (!counted) {
                counted = true;
                waitMap.activeWaiterCount.fetch_add(1, std::memory_order_acq_rel);
            }
            return cvPtr->wait_until(lock, timeout, std::move(pred));
        }

    private:
        WaitMap&                     waitMap;
        std::string                  path;
        std::unique_lock<std::mutex> lock;
        std::condition_variable*     cvPtr = nullptr;
        // Tracks whether this Guard has incremented the active waiter count.
        bool                         counted = false;


    };

    auto wait(std::string_view path) -> Guard;
    auto notify(std::string_view path) -> void;
    auto notifyAll() -> void;
    auto clear() -> void;

    auto hasWaiters() const -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return (this->root != nullptr) || !this->globWaiters.empty();
    }

private:
    friend struct Guard;
public:
    // Trie-based waiter storage for concrete paths.
    // Each node holds a CV for waiters on that exact node name.
    struct TrieNode {
        std::unordered_map<std::string, std::unique_ptr<TrieNode>, TransparentStringHash, std::equal_to<>> children;
        std::condition_variable cv;
    };
private:

    // Root of the trie (created on first use).
    std::unique_ptr<TrieNode> root;

    // Existing API compatibility (current .cpp still uses these; slated for migration):
    auto getCv(std::string_view path) -> std::condition_variable&;


    // Glob waiters are tracked separately by their pattern.
    // During notify, candidates are filtered via prefix/components and matched with match_paths.
    std::unordered_map<std::string, std::condition_variable, TransparentStringHash, std::equal_to<>> globWaiters;

    mutable std::mutex mutex;

    // Active waiter tracking to make clear() safe:
    // - activeWaiterCount counts Guards that have entered a wait (via template wait_until).
    // - noActiveWaitersCv is notified when the last active waiter departs, so clear() can wait if needed.
    // Note: clear() implementation should notify_all() first, then wait for activeWaiterCount==0 before destroying CVs.
    mutable std::mutex              activeWaitersMutex;
    std::condition_variable         noActiveWaitersCv;
    std::atomic<size_t>             activeWaiterCount{0};

};

} // namespace SP