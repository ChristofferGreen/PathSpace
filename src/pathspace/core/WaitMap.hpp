#pragma once
#include "path/TransparentString.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>

namespace SP {

struct WaitMap {
    struct WaiterEntry;
    struct Guard {
        Guard(WaitMap& waitMap, std::string_view path, std::uint64_t initialVersion);

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
            auto* entry = ensure_entry();
            auto const lockWaitStart = std::chrono::steady_clock::now();
            if (!waitLock.owns_lock()) {
                waitLock = std::unique_lock<std::mutex>(entry->mutex);
            }
            auto const lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lockWaitStart);

            // Begin waiter tracking on first wait call in this Guard (template path only).
            if (!counted) {
                counted = true;
                waitMap.activeWaiterCount.fetch_add(1, std::memory_order_acq_rel);
            }

            auto const waitStart = std::chrono::steady_clock::now();
            auto const result = entry->cv.wait_until(waitLock, timeout, std::move(pred));
            auto const waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStart);

            if (WaitMap::debug_enabled()) {
                WaitMap::debug_log("wait_until(pred)",
                                   path,
                                   lockWaitMs,
                                   waitMs,
                                   0);
            }
            return result;
        }

    private:
        struct WaiterEntry*           ensure_entry();

        WaitMap&                     waitMap;
        std::string                  path;
        WaiterEntry*                 entry{nullptr};
        std::unique_lock<std::mutex> waitLock;
        // Tracks whether this Guard has incremented the active waiter count.
        bool                         counted = false;
        std::uint64_t                awaitedVersion{0};
        bool                         versionInitialized{false};


    };

    auto wait(std::string_view path) -> Guard;
    auto notify(std::string_view path) -> void;
    auto notifyAll() -> void;
    auto clear() -> void;

    auto hasWaiters() const -> bool {
        std::lock_guard<std::timed_mutex> lock(registryMutex);
        return (this->root != nullptr) || !this->globWaiters.empty();
    }

private:
    friend struct Guard;
public:
    struct WaiterEntry {
        std::condition_variable cv;
        std::mutex              mutex;
        std::atomic<std::uint64_t> notifyVersion{0};
    };
    // Trie-based waiter storage for concrete paths.
    // Each node holds a CV for waiters on that exact node name.
    struct TrieNode {
        std::unordered_map<std::string, std::unique_ptr<TrieNode>, TransparentStringHash, std::equal_to<>> children;
        WaiterEntry entry;
    };
private:

    // Root of the trie (created on first use).
    std::unique_ptr<TrieNode> root;

    // Existing API compatibility (current .cpp still uses these; slated for migration):
    auto getEntry(std::string_view path) -> WaiterEntry&;


    // Glob waiters are tracked separately by their pattern.
    // During notify, candidates are filtered via prefix/components and matched with match_paths.
    std::unordered_map<std::string, WaiterEntry, TransparentStringHash, std::equal_to<>> globWaiters;

    mutable std::timed_mutex registryMutex;

    // Active waiter tracking to make clear() safe:
    // - activeWaiterCount counts Guards that have entered a wait (via template wait_until).
    // - noActiveWaitersCv is notified when the last active waiter departs, so clear() can wait if needed.
    // Note: clear() implementation should notify_all() first, then wait for activeWaiterCount==0 before destroying CVs.
    mutable std::mutex              activeWaitersMutex;
    std::condition_variable         noActiveWaitersCv;
    std::atomic<size_t>             activeWaiterCount{0};

public:
    static bool debug_enabled();
    static void debug_log(char const* event,
                          std::string_view path,
                          std::chrono::milliseconds lock_wait_ms,
                          std::chrono::milliseconds wait_ms,
                          std::size_t notified);

};

namespace testing {
// Test-only override to force WaitMap debug logging without relying on PATHSPACE_DEBUG_WAITMAP.
std::atomic<bool>& waitMapDebugOverride();
} // namespace testing

} // namespace SP
