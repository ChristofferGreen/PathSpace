#include "Cache.hpp"
#include "PathSpaceLeaf.hpp"
#include "core/NodeData.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

Cache::Cache(size_t maxSize) : maxSize(maxSize) {
    sp_log("Cache::Cache", "Function Called");
}

auto Cache::lookup(ConcretePathString const& path, PathSpaceLeaf const& root) const -> Expected<PathSpaceLeaf*> {
    sp_log("Cache::lookup", "Function Called");

    if (!path.isValid()) {
        incrementMisses();
        return std::unexpected(Error{Error::Code::InvalidPath, "Invalid path in cache lookup"});
    }

    PathSpaceLeaf* result = nullptr;
    bool found = entries.if_contains(path, [&result](const auto& pair) { result = pair.second.leaf; });

    if (found && result) {
        sp_log("Cache::lookup - Cache hit", "DEBUG");
        incrementHits();
        return result;
    }

    sp_log("Cache::lookup - Cache miss", "DEBUG");
    incrementMisses();
    return std::unexpected(Error{Error::Code::NoSuchPath, "Path not found in cache"});
}

auto Cache::store(ConcretePathStringView const& path, PathSpaceLeaf& root) -> void {
    sp_log("Cache::store", "Function Called");

    if (!path.isValid()) {
        sp_log("Cache::store - Invalid path", "ERROR");
        return;
    }

    // If we're at capacity, remove oldest entries
    if (entries.size() >= maxSize) {
        std::vector<ConcretePathString> allPaths;
        entries.for_each([&](const auto& pair) { allPaths.push_back(pair.first); });

        size_t toRemove = entries.size() - maxSize + 1;
        for (size_t i = 0; i < toRemove && i < allPaths.size(); ++i) {
            entries.erase(allPaths[i]);
        }
    }

    auto leafResult = root.getLeafNode(path.begin(), path.end());
    if (!leafResult.has_value()) {
        sp_log("Cache::store - Failed to get leaf node", "WARNING");
        return;
    }

    entries.try_emplace_l(path, [&](auto& pair) { pair.second.leaf = leafResult.value(); }, CacheEntry{leafResult.value()});
}

auto Cache::invalidate(ConcretePathString const& path) -> void {
    sp_log("Cache::invalidate", "Function Called");
    if (entries.erase(path) > 0) {
        incrementInvalidations();
    }
}

auto Cache::invalidatePrefix(ConcretePathString const& path) -> void {
    sp_log("Cache::invalidatePrefix", "Function Called");
    auto prefix = path.getPath();
    size_t count = 0;

    std::vector<ConcretePathString> to_remove;
    entries.for_each([&](const auto& pair) {
        if (pair.first.getPath().starts_with(prefix)) {
            to_remove.push_back(pair.first);
            count++;
        }
    });

    for (const auto& key : to_remove) {
        entries.erase(key);
    }

    if (count > 0) {
        auto current = stats.load(std::memory_order_relaxed);
        current.invalidations += count;
        stats.store(current, std::memory_order_relaxed);
    }
}

auto Cache::invalidatePattern(GlobPathString const& pattern) -> void {
    sp_log("Cache::invalidatePattern", "Function Called");

    // Count entries that will be invalidated
    size_t count = 0;
    entries.for_each([&](const auto&) { count++; });

    if (count > 0) {
        auto current = stats.load(std::memory_order_relaxed);
        current.invalidations += count;
        stats.store(current, std::memory_order_relaxed);
        entries.clear();
    }
}

auto Cache::clear() -> void {
    sp_log("Cache::clear", "Function Called");
    entries.clear();
}

auto Cache::getStats() const -> CacheStats {
    return stats.load(std::memory_order_relaxed);
}

auto Cache::resetStats() -> void {
    stats.store(CacheStats{}, std::memory_order_relaxed);
}

auto Cache::resize(size_t newSize) -> void {
    sp_log("Cache::resize", "Function Called");
    maxSize = newSize;
    cleanup(); // Force cleanup to match new size
}

auto Cache::cleanup() -> void {
    sp_log("Cache::cleanup", "Function Called");
    auto now = std::chrono::steady_clock::now();

    std::vector<ConcretePathString> all_keys;
    entries.for_each([&](const auto& pair) { all_keys.push_back(pair.first); });

    if (all_keys.size() > maxSize) {
        size_t to_remove = all_keys.size() - maxSize;
        for (size_t i = 0; i < to_remove; ++i) {
            entries.erase(all_keys[i]);
        }
    }
}

} // namespace SP