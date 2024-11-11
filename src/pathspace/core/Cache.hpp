#pragma once
#include "PathSpaceLeaf.hpp"
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPath.hpp"
#include "path/GlobPathIterator.hpp"
#include "type/NodeDataHashMap.hpp"

namespace SP {

struct Error;
struct InOptions;
struct InsertReturn;
struct InputData;
struct OutOptions;
struct ConstructiblePath;

struct CacheStats {
    size_t hits{0};
    size_t misses{0};
    size_t invalidations{0};
};

class Cache {
public:
    explicit Cache(size_t maxSize = 1000);

    auto lookup(ConcretePathString const& path, PathSpaceLeaf const& root) const -> Expected<PathSpaceLeaf*>;
    auto store(ConcretePathStringView const& path, PathSpaceLeaf& root) -> void;
    auto invalidate(ConcretePathString const& path) -> void;
    auto invalidatePrefix(ConcretePathString const& path) -> void;
    auto invalidatePattern(GlobPathString const& pattern) -> void;
    auto clear() -> void;

    // Cache control methods
    auto resize(size_t newSize) -> void;
    auto getStats() const -> CacheStats;
    auto resetStats() -> void;

private:
    struct CacheEntry {
        PathSpaceLeaf* leaf;
    };

    auto cleanup() -> void;

    using CacheMap = phmap::parallel_flat_hash_map<ConcretePathString,
                                                   CacheEntry,
                                                   std::hash<ConcretePathString>,
                                                   std::equal_to<ConcretePathString>,
                                                   std::allocator<std::pair<const ConcretePathString, CacheEntry>>,
                                                   8, // Number of submaps for concurrent access
                                                   std::shared_mutex>;

    auto incrementHits() const -> void {
        auto current = stats.load(std::memory_order_relaxed);
        current.hits++;
        stats.store(current, std::memory_order_relaxed);
    }

    auto incrementMisses() const -> void {
        auto current = stats.load(std::memory_order_relaxed);
        current.misses++;
        stats.store(current, std::memory_order_relaxed);
    }

    auto incrementInvalidations() const -> void {
        auto current = stats.load(std::memory_order_relaxed);
        current.invalidations++;
        stats.store(current, std::memory_order_relaxed);
    }

    CacheMap entries;
    size_t maxSize;
    std::atomic<size_t> cleanupCounter{0};
    mutable std::atomic<CacheStats> stats{};
    static constexpr size_t CLEANUP_FREQUENCY = 100;
};

} // namespace SP