#pragma once
#include "CacheEntry.hpp"
#include "path/ConcretePath.hpp"
#include <shared_mutex>
#include <unordered_map>

namespace SP {

class SimpleCache {
public:
    explicit SimpleCache(size_t maxSize = 1000, std::chrono::seconds ttl = std::chrono::seconds(300));

    auto cache(ConcretePathString const& path, std::shared_ptr<PathSpaceLeaf> leaf) -> void;
    auto get(ConcretePathString const& path) -> std::shared_ptr<PathSpaceLeaf>;
    auto get(ConcretePathStringView const& path) -> std::shared_ptr<PathSpaceLeaf>;
    auto invalidate(ConcretePathString const& path) -> void;
    auto invalidate(ConcretePathStringView const& path) -> void;
    auto clear() -> void;

    // Statistics
    auto size() const -> size_t;
    auto hitCount() const -> uint64_t;
    auto missCount() const -> uint64_t;
    auto hitRate() const -> double;

private:
    auto cleanup() -> void;
    auto incrementHit() const -> void;
    auto incrementMiss() const -> void;

    std::unordered_map<ConcretePathString, CacheEntry> cache_;
    mutable std::shared_mutex mutex_;
    const size_t maxSize_;
    const std::chrono::seconds ttl_;

    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
};

} // namespace SP