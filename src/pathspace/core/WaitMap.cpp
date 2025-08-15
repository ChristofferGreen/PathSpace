#include "core/WaitMap.hpp"
#include "log/TaggedLogger.hpp"
#include "path/utils.hpp"

#include <functional>
#include <string>

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock)
    : waitMap(waitMap), path(path), lock(std::move(lock)) {}

auto WaitMap::Guard::wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status {
    return this->waitMap.getCv(path).wait_until(lock, timeout);
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

    std::lock_guard<std::mutex> lock(mutex);

    sp_log("Currently registered waiters:", "WaitMap");
    for (auto& [rp, _] : this->cvMap) {
        sp_log("  - " + rp, "WaitMap");
    }

    if (is_concrete(path)) {
        // Notify exact concrete waiters (cvMap) if present
        auto it = cvMap.find(path);
        if (it != cvMap.end()) {
            sp_log("Found matching concrete path", "WaitMap");
            it->second.notify_all();
        }

        // Notify any glob waiters that match this concrete path
        for (auto& [pattern, cv] : this->globWaiters) {
            if (is_glob(pattern) && match_paths(pattern, path)) {
                sp_log("Found matching glob waiter: " + pattern, "WaitMap");
                cv.notify_all();
            }
        }
        return;
    }

    // Glob notify: find all concrete registered paths that match
    if (!this->root) {
        sp_log("No trie root; nothing to notify for glob", "WaitMap");
        return;
    }

    std::function<void(TrieNode*, std::string&)> dfs;
    dfs = [&](TrieNode* node, std::string& current) {
        // Only notify exact registered concrete paths present in cvMap
        if (!current.empty() && current[0] != '/')
            current.insert(current.begin(), '/');

        // If this exact current path is registered and matches the glob, notify
        if (!current.empty()) {
            if (match_paths(path, current)) {
                auto it = cvMap.find(current);
                if (it != cvMap.end()) {
                    sp_log("Notifying waiters for matching path: " + current, "WaitMap");
                    it->second.notify_all();
                }
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
        auto it = cvMap.find("/");
        if (it != cvMap.end()) {
            sp_log("Notifying waiters for matching root path: /", "WaitMap");
            it->second.notify_all();
        }
    }
    dfs(this->root.get(), prefix);
}

auto WaitMap::notifyAll() -> void {
    sp_log("notifyAll called", "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);

    // Notify all concrete waiters
    for (auto& [path, cv] : this->cvMap) {
        sp_log("Notifying path: " + path, "WaitMap");
        cv.notify_all();
    }

    // Notify all glob waiters
    for (auto& [pattern, cv] : this->globWaiters) {
        sp_log("Notifying glob waiter: " + pattern, "WaitMap");
        cv.notify_all();
    }
}

auto WaitMap::clear() -> void {
    std::lock_guard<std::mutex> lock(mutex);
    this->cvMap.clear();
    this->globWaiters.clear();
    this->root.reset();
}

auto WaitMap::getCv(std::string_view path) -> std::condition_variable& {
    // Glob waiters: keep a separate registry
    if (is_glob(path)) {
        return this->globWaiters[std::string(path)];
    }

    // Concrete waiters: ensure trie node exists and return cv from flat registry for compatibility
    // Maintain cvMap for hasWaiters() and for waiter CVs; also mirror into trie for efficient glob notify.
    if (!this->root)
        this->root = std::make_unique<TrieNode>();
    (void)getOrCreateTrieNode(this->root, path);

    return this->cvMap[std::string(path)];
}

} // namespace SP