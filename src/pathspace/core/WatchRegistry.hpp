#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SP {

/**
 * WatchRegistry — v1 scaffolding (concrete-path support only)
 *
 * Purpose
 * -------
 * A wait/notify registry optimized for concrete (non-glob) paths. Internally,
 * it maintains a trie keyed by path components. Each trie node contains a
 * condition_variable used to wake waiters waiting on that exact path.
 *
 * Notes
 * -----
 * - This is the initial version that supports only concrete paths. Glob
 *   subscriptions will be added in a later phase.
 * - This API mirrors the minimal surface of WaitMap to ease integration:
 *     - Guard wait(std::string_view)
 *     - void notify(std::string_view)
 *     - void notifyAll()
 *     - void clear()
 *     - bool hasWaiters() const
 * - Thread-safety:
 *     - All operations synchronize on a single mutex.
 *     - clear() resets the trie; callers should avoid calling clear() while
 *       threads are actively waiting (recommended flow: notifyAll() then clear()).
 */
class WatchRegistry {
public:
    // Define TrieNode before Guard so Guard methods can access its members.
    struct TrieNode {
        std::unordered_map<std::string, std::shared_ptr<TrieNode>> children;
        std::condition_variable                                     cv;
        std::size_t                                                 waiters = 0;
    };

    struct Guard {
        Guard(WatchRegistry& reg, std::string path, std::unique_lock<std::mutex> lock, std::shared_ptr<TrieNode> node) noexcept
            : reg_(reg), path_(std::move(path)), lock_(std::move(lock)), node_(std::move(node)) {}

        Guard(Guard&& other) noexcept
            : reg_(other.reg_)
            , path_(std::move(other.path_))
            , lock_(std::move(other.lock_))
            , node_(std::move(other.node_)) {
        }

        Guard(Guard const&)            = delete;
        Guard& operator=(Guard const&) = delete;

        ~Guard() {
            if (node_) {
                // Decrement waiter count under the existing unique_lock (avoid double-locking)
                if (!lock_.owns_lock()) {
                    lock_.lock();
                }
                if (node_->waiters > 0)
                    --node_->waiters;
                if (reg_.totalWaiters_ > 0)
                    --reg_.totalWaiters_;
                node_.reset();
            }
        }

        // Untimed wait until notification (or spurious wakeup)
        void wait() {
            if (!node_) return;
            node_->cv.wait(lock_);
        }

        // Timed wait until the specified deadline; returns std::cv_status
        auto wait_until(std::chrono::time_point<std::chrono::system_clock> deadline) -> std::cv_status {
            if (!node_) return std::cv_status::no_timeout;
            return node_->cv.wait_until(lock_, deadline);
        }

        // Predicate-based wait_until; returns true if predicate satisfied
        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> deadline, Pred pred) {
            if (!node_) return true;
            return node_->cv.wait_until(lock_, deadline, std::move(pred));
        }

    private:
        WatchRegistry&                reg_;
        std::string                   path_;
        std::unique_lock<std::mutex>  lock_;
        std::shared_ptr<TrieNode>     node_;
    };

    WatchRegistry()  = default;
    ~WatchRegistry() = default;

    WatchRegistry(WatchRegistry const&)            = delete;
    WatchRegistry& operator=(WatchRegistry const&) = delete;

    // Acquire a guard for a concrete (non-glob) path. The guard holds a lock
    // on the registry mutex and is tied to the path node's condition variable.
    // Call wait()/wait_until(...) on the guard to block.
    Guard wait(std::string_view path) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto node = getOrCreateTrieNodeUnlocked(path);
        ++node->waiters;
        ++totalWaiters_;
        return Guard(*this, std::string(path), std::move(lock), std::move(node));
    }

    // Notify waiters registered on the exact (concrete) path.
    void notify(std::string_view path) {
        std::lock_guard<std::mutex> lg(mutex_);
        auto node = findTrieNodeUnlocked(path);
        if (node) {
            node->cv.notify_all();
        }
    }

    // Notify all registered waiters on all paths.
    void notifyAll() {
        std::lock_guard<std::mutex> lg(mutex_);
        if (!root_) return;
        dfsNotifyAll_(root_.get());
    }

    // Remove all nodes and reset counters.
    // Callers should ensure no active waiters are blocked before clearing.
    void clear() {
        std::lock_guard<std::mutex> lg(mutex_);
        root_.reset();
        totalWaiters_ = 0;
    }

    // Returns true if there are any registered waiters.
    bool hasWaiters() const {
        std::lock_guard<std::mutex> lg(mutex_);
        return totalWaiters_ > 0;
    }

private:

    // Split a path into components without allocating per segment string_views persistently.
    static void splitPath_(std::string_view path, std::vector<std::string_view>& out) {
        out.clear();
        std::size_t start = 0;
        if (!path.empty() && path.front() == '/')
            start = 1;
        while (start <= path.size()) {
            std::size_t end = path.find('/', start);
            std::size_t len = (end == std::string_view::npos ? path.size() : end) - start;
            std::string_view name = path.substr(start, len);
            if (!name.empty())
                out.push_back(name);
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        // Special case: root path "/" maps to zero components; handle at callers.
    }

    std::shared_ptr<TrieNode> getOrCreateTrieNodeUnlocked(std::string_view path) {
        if (!root_) root_ = std::make_shared<TrieNode>();
        if (path == "/" || path.empty())
            return root_;

        scratch_.clear();
        splitPath_(path, scratch_);
        std::shared_ptr<TrieNode> node = root_;
        for (auto const& comp : scratch_) {
            auto it = node->children.find(std::string(comp));
            if (it == node->children.end()) {
                it = node->children.emplace(std::string(comp), std::make_shared<TrieNode>()).first;
            }
            node = it->second;
        }
        return node;
    }

    std::shared_ptr<TrieNode> findTrieNodeUnlocked(std::string_view path) const {
        if (!root_) return nullptr;
        if (path == "/" || path.empty())
            return root_;

        // Use a local scratch since this is const. We avoid storing per-instance mutable state.
        std::vector<std::string_view> comps;
        comps.reserve(8);
        splitPath_(path, comps);
        std::shared_ptr<TrieNode> node = root_;
        for (auto const& comp : comps) {
            auto it = node->children.find(std::string(comp));
            if (it == node->children.end())
                return nullptr;
            node = it->second;
        }
        return node;
    }

    void dfsNotifyAll_(TrieNode* node) {
        if (!node) return;
        node->cv.notify_all();
        for (auto& kv : node->children) {
            dfsNotifyAll_(kv.second.get());
        }
    }

    // Registry state
    mutable std::mutex                 mutex_;
    std::shared_ptr<TrieNode>          root_;
    std::size_t                        totalWaiters_{0};

    // Reusable buffer to avoid reallocations during getOrCreate calls (protected by mutex_)
    std::vector<std::string_view>      scratch_;
};

} // namespace SP