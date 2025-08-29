#include "core/WaitMap.hpp"
#include "log/TaggedLogger.hpp"
#include "path/utils.hpp"

#include <functional>
#include <string>

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock)
    : waitMap(waitMap), path(path), lock(std::move(lock)) {
    // Cache CV pointer to avoid repeated lookups during wait slices
    cvPtr = &this->waitMap.getCv(this->path);
    // Track active waiter immediately to close race with clear()
    if (!counted) {
        counted = true;
        this->waitMap.activeWaiterCount.fetch_add(1, std::memory_order_acq_rel);
    }
}

auto WaitMap::Guard::wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status {
    if (!cvPtr) cvPtr = &this->waitMap.getCv(this->path);
    // Begin waiter tracking on first wait call in this Guard
    if (!counted) {
        counted = true;
        this->waitMap.activeWaiterCount.fetch_add(1, std::memory_order_acq_rel);
    }
    return cvPtr->wait_until(lock, timeout);
}

auto WaitMap::wait(std::string_view path) -> Guard {
    sp_log("WaitMap::wait for path: " + std::string(path), "WaitMap");
    return Guard(*this, path, std::unique_lock<std::mutex>(mutex));
}

// Helper: traverse trie to the node for a concrete path; create nodes as needed.
static WaitMap::TrieNode* getOrCreateTrieNode(std::unique_ptr<WaitMap::TrieNode>& root, std::string_view path) {
    if (!root)
        root = std::make_unique<WaitMap::TrieNode>();

    auto* node   = root.get();
    size_t start = 0;

    // Skip leading slash
    if (!path.empty() && path.front() == '/')
        start = 1;

    while (start <= path.size()) {
        size_t end = path.find('/', start);
        size_t len = (end == std::string_view::npos ? path.size() : end) - start;
        std::string_view name = path.substr(start, len);
        if (!name.empty()) {
            auto it = node->children.find(std::string(name));
            if (it == node->children.end()) {
                it = node->children.emplace(std::string(name), std::make_unique<WaitMap::TrieNode>()).first;
            }
            node = it->second.get();
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return node;
}

// Helper: traverse trie without creating; return nullptr if not present.
static WaitMap::TrieNode* findTrieNode(std::unique_ptr<WaitMap::TrieNode> const& root, std::string_view path) {
    if (!root)
        return nullptr;

    auto* node   = root.get();
    size_t start = 0;

    if (!path.empty() && path.front() == '/')
        start = 1;

    while (start <= path.size()) {
        size_t end = path.find('/', start);
        size_t len = (end == std::string_view::npos ? path.size() : end) - start;
        std::string_view name = path.substr(start, len);
        if (!name.empty()) {
            auto it = node->children.find(std::string(name));
            if (it == node->children.end())
                return nullptr;
            node = it->second.get();
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return node;
}

auto WaitMap::notify(std::string_view path) -> void {
    sp_log("notify(glob view): " + std::string(path) + " concrete=" + std::to_string(is_concrete(path)), "WaitMap");

    // Gather CVs to notify without holding the mutex during notify_all
    std::vector<std::condition_variable*> toNotify;
    std::vector<std::condition_variable*> toNotifyGlob;

    {
        std::lock_guard<std::mutex> lock(mutex);

        if (is_concrete(path)) {
            // Notify trie node CV for concrete path
            if (auto* node = findTrieNode(this->root, path)) {
                sp_log("Found matching concrete path", "WaitMap");
                toNotify.push_back(&node->cv);
            }
            // Also notify compatibility cvMap entry if present


            // Notify any glob waiters that match this concrete path
            for (auto& [pattern, cv] : this->globWaiters) {
                if (is_glob(pattern) && match_paths(pattern, path)) {
                    sp_log("Found matching glob waiter: " + pattern, "WaitMap");
                    toNotifyGlob.push_back(&cv);
                }
            }
        } else {
            // Glob notify: find all concrete registered paths that match
            if (!this->root) {
                sp_log("No trie root; nothing to notify for glob", "WaitMap");
            } else {
                std::function<void(TrieNode*, std::string&)> dfs;
                dfs = [&](TrieNode* node, std::string& current) {
                    // Only notify exact registered concrete paths present in cvMap
                    if (!current.empty() && current[0] != '/')
                        current.insert(current.begin(), '/');

                    // If this exact current path is registered and matches the glob, schedule notify
                    if (!current.empty()) {
                        if (match_paths(path, current)) {
                            sp_log("Queueing notify for matching path: " + current, "WaitMap");
                            toNotify.push_back(&node->cv);

                        }
                    }

                    // Continue traversal
                    for (auto& [name, childPtr] : node->children) {
                        size_t oldSize = current.size();
                        if (current == "/" || current.empty())
                            current = "/" + name;
                        else
                            current += "/" + name;
                        dfs(childPtr.get(), current);
                        current.resize(oldSize);
                    }
                };

                std::string prefix;
                // Root represents "/", ensure we also consider "/" itself when matching
                if (path == "/" || match_paths(path, "/")) {
                    // Notify root node CV if present
                    if (this->root) {
                        sp_log("Queueing notify for matching root path: /", "WaitMap");
                        toNotify.push_back(&this->root->cv);
                    }
                    // Also notify compatibility cvMap entry if present

                }
                dfs(this->root.get(), prefix);
            }
        }
    } // release mutex before notifying

    for (auto* cv : toNotify) {
        cv->notify_all();
    }
    for (auto* cv : toNotifyGlob) {
        cv->notify_all();
    }
}

auto WaitMap::notifyAll() -> void {

    std::vector<std::condition_variable*> toNotify;

    {
        std::lock_guard<std::mutex> lock(mutex);

        // Notify all trie-based concrete waiters
        if (this->root) {
            std::function<void(TrieNode*)> dfsNotify = [&](TrieNode* node) {
                if (!node) return;
                toNotify.push_back(&node->cv);
                for (auto& [_, child] : node->children) {
                    dfsNotify(child.get());
                }
            };
            dfsNotify(this->root.get());
        }



        // Notify all glob waiters
        for (auto& [pattern, cv] : this->globWaiters) {
            toNotify.push_back(&cv);
        }
    } // release mutex

    for (auto* cv : toNotify) {
        cv->notify_all();
    }
}

auto WaitMap::clear() -> void {
    // Step 1: wake all current waiters so they can exit waits before we destroy CVs
    std::vector<std::condition_variable*> toNotify;
    {
        std::lock_guard<std::mutex> lock(mutex);

        // Gather all trie-based CVs
        if (this->root) {
            std::function<void(TrieNode*)> dfsNotify = [&](TrieNode* node) {
                if (!node) return;
                toNotify.push_back(&node->cv);
                for (auto& [_, child] : node->children) {
                    dfsNotify(child.get());
                }
            };
            dfsNotify(this->root.get());
        }

        // Gather all glob CVs
        for (auto& [_, cv] : this->globWaiters) {
            toNotify.push_back(&cv);
        }
    } // release registry lock before notifying

    for (auto* cv : toNotify) {
        cv->notify_all();
    }

    // Step 2: wait for any in-flight waiters to drain so no one is waiting on CVs we are about to destroy
    {
        std::unique_lock<std::mutex> lk(this->activeWaitersMutex);
        this->noActiveWaitersCv.wait(lk, [this] {
            return this->activeWaiterCount.load(std::memory_order_acquire) == 0;
        });
    }

    // Step 3: clear all waiter structures safely
    {
        std::lock_guard<std::mutex> lock(mutex);
        this->globWaiters.clear();
        this->root.reset();
    }
}

auto WaitMap::getCv(std::string_view path) -> std::condition_variable& {
    // Glob waiters: keep a separate registry (note: map CV is used only for glob waits)
    if (is_glob(path)) {
        return this->globWaiters[std::string(path)];
    }

    // Concrete waiters: ensure trie node exists and return its CV for stable storage
    if (!this->root)
        this->root = std::make_unique<TrieNode>();
    TrieNode* node = getOrCreateTrieNode(this->root, path);



    return node->cv;
}

} // namespace SP