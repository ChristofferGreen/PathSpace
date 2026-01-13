#include "core/WaitMap.hpp"
#include "log/TaggedLogger.hpp"
#include "path/utils.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr auto NotifyLockWatchdog = std::chrono::milliseconds(100);

auto thread_id_string() -> std::string {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

} // namespace

namespace SP {

bool WaitMap::debug_enabled() {
    static bool enabled = [] {
        if (const char* flag = std::getenv("PATHSPACE_DEBUG_WAITMAP")) {
            return std::strcmp(flag, "0") != 0;
        }
        return false;
    }();
    return enabled;
}

void WaitMap::debug_log(char const* event,
                        std::string_view path,
                        std::chrono::milliseconds lock_wait_ms,
                        std::chrono::milliseconds wait_ms,
                        std::size_t notified) {
    if (!debug_enabled()) {
        return;
    }
    auto tid = thread_id_string();
    std::fprintf(stderr,
                 "WaitMap[%s] %s path=%.*s lock_ms=%lld wait_ms=%lld notify=%zu\n",
                 tid.c_str(),
                 event,
                 static_cast<int>(path.size()),
                 path.data(),
                 static_cast<long long>(lock_wait_ms.count()),
                 static_cast<long long>(wait_ms.count()),
                 notified);
}

WaitMap::Guard::Guard(WaitMap& waitMap, std::string_view path)
    : waitMap(waitMap), path(path) {}

auto WaitMap::Guard::ensure_entry() -> WaiterEntry* {
    if (!entry) {
        entry = &waitMap.getEntry(path);
    }
    return entry;
}

auto WaitMap::Guard::wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status {
    auto* entryPtr = ensure_entry();
    auto const lockWaitStart = std::chrono::steady_clock::now();
    if (!waitLock.owns_lock()) {
        waitLock = std::unique_lock<std::mutex>(entryPtr->mutex);
    }
    auto const lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lockWaitStart);

    // Begin waiter tracking on first wait call in this Guard
    if (!counted) {
        counted = true;
        this->waitMap.activeWaiterCount.fetch_add(1, std::memory_order_acq_rel);
    }

    auto const waitStart = std::chrono::steady_clock::now();
    auto const status = entryPtr->cv.wait_until(waitLock, timeout);
    auto const waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - waitStart);

    if (WaitMap::debug_enabled()) {
        WaitMap::debug_log("wait_until", path, lockWaitMs, waitMs, 0);
    }
    return status;
}

auto WaitMap::wait(std::string_view path) -> Guard {
    sp_log("WaitMap::wait for path: " + std::string(path), "WaitMap");
    if (WaitMap::debug_enabled()) {
        WaitMap::debug_log("wait", path, std::chrono::milliseconds{0}, std::chrono::milliseconds{0}, 0);
    }
    return Guard(*this, path);
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
    auto const lockAttempt = std::chrono::steady_clock::now();
    std::unique_lock<std::timed_mutex> registryLock(registryMutex, std::defer_lock);
    if (!registryLock.try_lock_for(NotifyLockWatchdog)) {
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lockAttempt);
        sp_log("WaitMap notify lock waited " + std::to_string(waited.count()) + "ms for path " + std::string(path),
               "WaitMap");
        registryLock.lock();
    }
    auto const lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lockAttempt);

    // Gather CVs to notify without holding the mutex during notify_all
    std::vector<WaiterEntry*> toNotify;
    std::vector<WaiterEntry*> toNotifyGlob;

    if (is_concrete(path)) {
        // Notify trie node CV for concrete path
        if (auto* node = findTrieNode(this->root, path)) {
            sp_log("Found matching concrete path", "WaitMap");
            toNotify.push_back(&node->entry);
        }

        // Notify any glob waiters that match this concrete path
        for (auto& [pattern, entry] : this->globWaiters) {
            if (is_glob(pattern) && match_paths(pattern, path)) {
                sp_log("Found matching glob waiter: " + pattern, "WaitMap");
                toNotifyGlob.push_back(&entry);
            }
        }
    } else {
        // Glob notify: find all concrete registered paths that match
        if (!this->root) {
            sp_log("No trie root; nothing to notify for glob", "WaitMap");
        } else {
            std::function<void(TrieNode*, std::string&)> dfs;
            dfs = [&](TrieNode* node, std::string& current) {
                if (!current.empty() && current[0] != '/')
                    current.insert(current.begin(), '/');

                if (!current.empty()) {
                    if (match_paths(path, current)) {
                        sp_log("Queueing notify for matching path: " + current, "WaitMap");
                        toNotify.push_back(&node->entry);
                    }
                }

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
                    toNotify.push_back(&this->root->entry);
                }
            }
            dfs(this->root.get(), prefix);
        }
    }

    registryLock.unlock();

    if (WaitMap::debug_enabled()) {
        WaitMap::debug_log("notify",
                           path,
                           lockWaitMs,
                           std::chrono::milliseconds{0},
                           toNotify.size() + toNotifyGlob.size());
    }

    for (auto* entry : toNotify) {
        entry->cv.notify_all();
    }
    for (auto* entry : toNotifyGlob) {
        entry->cv.notify_all();
    }
}

auto WaitMap::notifyAll() -> void {

    std::vector<WaiterEntry*> toNotify;

    {
        std::lock_guard<std::timed_mutex> lock(registryMutex);

        // Notify all trie-based concrete waiters
        if (this->root) {
            std::function<void(TrieNode*)> dfsNotify = [&](TrieNode* node) {
                if (!node) return;
                toNotify.push_back(&node->entry);
                for (auto& [_, child] : node->children) {
                    dfsNotify(child.get());
                }
            };
            dfsNotify(this->root.get());
        }



        // Notify all glob waiters
        for (auto& [pattern, entry] : this->globWaiters) {
            (void)pattern;
            toNotify.push_back(&entry);
        }
    } // release mutex

    for (auto* entry : toNotify) {
        entry->cv.notify_all();
    }
}

auto WaitMap::clear() -> void {
    // Step 1: wake all current waiters so they can exit waits before we destroy CVs
    std::vector<WaiterEntry*> toNotify;
    {
        std::lock_guard<std::timed_mutex> lock(registryMutex);

        // Gather all trie-based CVs
        if (this->root) {
            std::function<void(TrieNode*)> dfsNotify = [&](TrieNode* node) {
                if (!node) return;
                toNotify.push_back(&node->entry);
                for (auto& [_, child] : node->children) {
                    dfsNotify(child.get());
                }
            };
            dfsNotify(this->root.get());
        }

        // Gather all glob CVs
        for (auto& [_, entry] : this->globWaiters) {
            toNotify.push_back(&entry);
        }
    } // release registry lock before notifying

    for (auto* entry : toNotify) {
        entry->cv.notify_all();
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
        std::lock_guard<std::timed_mutex> lock(registryMutex);
        this->globWaiters.clear();
        this->root.reset();
    }
}

auto WaitMap::getEntry(std::string_view path) -> WaiterEntry& {
    std::lock_guard<std::timed_mutex> lock(registryMutex);

    // Glob waiters: keep a separate registry (note: map CV is used only for glob waits)
    if (is_glob(path)) {
        return this->globWaiters[std::string(path)];
    }

    // Concrete waiters: ensure trie node exists and return its CV for stable storage
    if (!this->root)
        this->root = std::make_unique<TrieNode>();
    TrieNode* node = getOrCreateTrieNode(this->root, path);



    return node->entry;
}

} // namespace SP
