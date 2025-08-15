#pragma once
#include "path/TransparentString.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>

namespace SP {

struct WaitMap {
    struct Guard {
        Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock);

        auto wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status;
        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> timeout, Pred pred) {
            return waitMap.getCv(path).wait_until(lock, timeout, std::move(pred));
        }

    private:
        WaitMap&                     waitMap;
        std::string                  path;
        std::unique_lock<std::mutex> lock;
    };

    auto wait(std::string_view path) -> Guard;
    auto notify(std::string_view path) -> void;
    auto notifyAll() -> void;
    auto clear() -> void;

    auto hasWaiters() const -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return !this->cvMap.empty();
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
    using WaitMapType = std::unordered_map<std::string, std::condition_variable, TransparentStringHash, std::equal_to<>>;

    // Glob waiters are tracked separately by their pattern.
    // During notify, candidates are filtered via prefix/components and matched with match_paths.
    std::unordered_map<std::string, std::condition_variable, TransparentStringHash, std::equal_to<>> globWaiters;

    mutable std::mutex mutex;
    WaitMapType        cvMap;
};

} // namespace SP