#pragma once
#include "core/Error.hpp"
#include "path/ConcretePath.hpp"
#include "path/GlobPath.hpp"
#include <chrono>
#include <parallel_hashmap/phmap.h>

namespace SP {

class PathSpaceLeaf;
class NodeData;

/**
 * @brief L1-style cache for PathSpace operations
 *
 * Provides a fast lookup layer in front of PathSpaceLeaf storage. Returns raw
 * pointers to PathSpaceLeaf nodes owned by the main storage. The cache does not
 * own any PathSpaceLeaf instances - it just provides faster lookups to existing ones.
 */
class Cache {
public:
    /**
     * @brief Constructs a new Cache
     *
     * @param maxSize Maximum number of entries to store
     * @param ttl Time-to-live for cache entries
     */
    explicit Cache(size_t maxSize = 1000);

    /**
     * @brief Looks up a PathSpaceLeaf in the cache
     *
     * Thread-safe method that returns a pointer to the cached PathSpaceLeaf if found
     * and not expired. The pointer is owned by the main storage, not the cache.
     *
     * @param path The path to look up
     * @param root The root storage for verification
     * @return Expected<PathSpaceLeaf*> containing pointer to leaf if found
     */
    auto lookup(ConcretePathString const& path, PathSpaceLeaf const& root) const -> Expected<PathSpaceLeaf*>;

    /**
     * @brief Stores data in the cache
     *
     * Thread-safe method to cache data at a path. Will store the data's leaf node
     * from the root storage.
     *
     * @param path The path where the data is stored
     * @param data The data being stored
     * @param root The root storage containing the data's leaf node
     */
    auto store(ConcretePathStringView const& path, NodeData const& data, PathSpaceLeaf& root) -> void;

    /**
     * @brief Invalidates a single path in the cache
     */
    auto invalidate(ConcretePathString const& path) -> void;

    /**
     * @brief Invalidates all paths with the given prefix
     */
    auto invalidatePrefix(ConcretePathString const& path) -> void;

    /**
     * @brief Invalidates all paths matching a pattern
     *
     * Currently implements a conservative approach by clearing the entire cache.
     */
    auto invalidatePattern(GlobPathString const& pattern) -> void;

    /**
     * @brief Clears all entries from the cache
     */
    auto clear() -> void;

private:
    struct CacheEntry {
        PathSpaceLeaf* leaf;
    };

    /**
     * @brief Removes expired entries and enforces size limits
     */
    auto cleanup() -> void;

    static constexpr size_t CLEANUP_FREQUENCY = 100;

    using CacheMap = phmap::parallel_flat_hash_map<ConcretePathString,
                                                   CacheEntry,
                                                   std::hash<ConcretePathString>,
                                                   std::equal_to<ConcretePathString>,
                                                   std::allocator<std::pair<const ConcretePathString, CacheEntry>>,
                                                   8,
                                                   std::shared_mutex>;

    CacheMap entries;
    const size_t maxSize;
    std::atomic<size_t> cleanupCounter{0};
};

} // namespace SP