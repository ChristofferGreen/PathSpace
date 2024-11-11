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
        return std::unexpected(Error{Error::Code::InvalidPath, "Invalid path in cache lookup"});
    }

    PathSpaceLeaf* result = nullptr;
    bool found = entries.if_contains(path, [&result](const auto& pair) { result = pair.second.leaf; });

    if (found && result) {
        sp_log("Cache::lookup - Cache hit", "DEBUG");
        return result;
    }

    sp_log("Cache::lookup - Cache miss", "DEBUG");
    return std::unexpected(Error{Error::Code::NoSuchPath, "Path not found in cache"});
}

auto Cache::store(ConcretePathStringView const& path, NodeData const& data, PathSpaceLeaf& root) -> void {
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

    // Get the specific leaf node instead of storing root
    auto leafResult = root.getLeafNode(path.begin(), path.end());
    if (!leafResult.has_value()) {
        sp_log("Cache::store - Failed to get leaf node", "WARNING");
        return;
    }

    entries.try_emplace_l(path, [&](auto& pair) { pair.second.leaf = leafResult.value(); }, CacheEntry{leafResult.value()});
}

auto Cache::invalidate(ConcretePathString const& path) -> void {
    sp_log("Cache::invalidate", "Function Called");
    entries.erase(path);
}

auto Cache::invalidatePrefix(ConcretePathString const& path) -> void {
    sp_log("Cache::invalidatePrefix", "Function Called");

    auto prefix = path.getPath();
    std::vector<ConcretePathString> to_remove;

    entries.for_each([&](const auto& pair) {
        if (pair.first.getPath().starts_with(prefix)) {
            to_remove.push_back(pair.first);
        }
    });

    for (const auto& key : to_remove) {
        entries.erase(key);
    }
}

auto Cache::invalidatePattern(GlobPathString const& pattern) -> void {
    sp_log("Cache::invalidatePattern", "Function Called");
    clear();
}

auto Cache::clear() -> void {
    sp_log("Cache::clear", "Function Called");
    entries.clear();
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