#include "SimpleCache.hpp"
#include "PathSpaceLeaf.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

SimpleCache::SimpleCache(size_t maxSize, std::chrono::seconds ttl) : maxSize_(maxSize), ttl_(ttl) {
    sp_log("SimpleCache::SimpleCache", "Function Called");
}

auto SimpleCache::cache(ConcretePathString const& path, std::shared_ptr<PathSpaceLeaf> leaf) -> void {
    sp_log("SimpleCache::cache", "Function Called");

    if (!path.isValid()) {
        sp_log("SimpleCache::cache - Invalid path", "WARNING");
        return;
    }

    std::unique_lock lock(mutex_);
    cleanup();

    cache_[path] = CacheEntry{leaf, std::chrono::steady_clock::now() + ttl_};
}

auto SimpleCache::get(ConcretePathString const& path) -> std::shared_ptr<PathSpaceLeaf> {
    sp_log("SimpleCache::get", "Function Called");

    if (!path.isValid()) {
        sp_log("SimpleCache::get - Invalid path", "WARNING");
        incrementMiss();
        return nullptr;
    }

    std::shared_lock lock(mutex_);

    auto it = cache_.find(path);
    if (it == cache_.end()) {
        incrementMiss();
        return nullptr;
    }

    if (!it->second.isValid()) {
        incrementMiss();
        return nullptr;
    }

    if (auto leaf = it->second.leaf.lock()) {
        incrementHit();
        return leaf;
    }

    incrementMiss();
    return nullptr;
}

auto SimpleCache::get(ConcretePathStringView const& path) -> std::shared_ptr<PathSpaceLeaf> {
    return get(ConcretePathString{path.getPath()});
}

auto SimpleCache::invalidate(ConcretePathString const& path) -> void {
    sp_log("SimpleCache::invalidate", "Function Called");

    if (!path.isValid()) {
        sp_log("SimpleCache::invalidate - Invalid path", "WARNING");
        return;
    }

    std::unique_lock lock(mutex_);
    cache_.erase(path);
}

auto SimpleCache::invalidate(ConcretePathStringView const& path) -> void {
    invalidate(ConcretePathString{path.getPath()});
}

auto SimpleCache::clear() -> void {
    sp_log("SimpleCache::clear", "Function Called");
    std::unique_lock lock(mutex_);
    cache_.clear();
}

auto SimpleCache::size() const -> size_t {
    std::shared_lock lock(mutex_);
    return cache_.size();
}

auto SimpleCache::cleanup() -> void {
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!it->second.isValid()) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }

    if (cache_.size() > maxSize_) {
        size_t toRemove = cache_.size() - maxSize_;
        for (size_t i = 0; i < toRemove && !cache_.empty(); ++i) {
            cache_.erase(cache_.begin());
        }
    }
}

auto SimpleCache::hitCount() const -> uint64_t {
    return hits_.load(std::memory_order_relaxed);
}

auto SimpleCache::missCount() const -> uint64_t {
    return misses_.load(std::memory_order_relaxed);
}

auto SimpleCache::hitRate() const -> double {
    auto hits = hits_.load(std::memory_order_relaxed);
    auto total = hits + misses_.load(std::memory_order_relaxed);
    return total > 0 ? static_cast<double>(hits) / total : 0.0;
}

auto SimpleCache::incrementHit() const -> void {
    hits_.fetch_add(1, std::memory_order_relaxed);
}

auto SimpleCache::incrementMiss() const -> void {
    misses_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace SP