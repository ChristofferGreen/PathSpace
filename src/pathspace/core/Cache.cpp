#include "Cache.hpp"
#include "PathSpaceLeaf.hpp"
#include "core/NodeData.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

Cache::Cache(size_t maxSize, std::chrono::seconds ttl) : maxSize(maxSize), ttl(ttl) {
    sp_log("Cache::Cache", "Function Called");
}

auto Cache::lookup(ConcretePathString const& path, PathSpaceLeaf const& root) const -> Expected<PathSpaceLeaf*> {
    sp_log("Cache::lookup", "Function Called");

    if (!path.isValid()) {
        return std::unexpected(Error{Error::Code::InvalidPath, "Invalid path in cache lookup"});
    }

    PathSpaceLeaf* result = nullptr;
    bool found = entries.if_contains(path, [&result, now = std::chrono::steady_clock::now()](const auto& pair) {
        if (now <= pair.second.expiry) {
            result = pair.second.leaf;
        }
    });

    if (found && result) {
        sp_log("Cache::lookup - Cache hit", "DEBUG");
        return result;
    }

    sp_log("Cache::lookup - Cache miss", "DEBUG");
    return std::unexpected(Error{Error::Code::NoSuchPath, "Path not found in cache"});
}

auto Cache::store(ConcretePathString const& path, NodeData const& data, PathSpaceLeaf& root) -> void {
    sp_log("Cache::store", "Function Called");

    if (!path.isValid()) {
        sp_log("Cache::store - Invalid path", "WARNING");
        return;
    }

    if ((++cleanupCounter % CLEANUP_FREQUENCY) == 0) {
        cleanup();
    }

    // TODO: For now we store the root itself
    // In the future when we have getLeaf functionality, we'd get the specific leaf node
    entries.try_emplace_l(
            path,
            [&](auto& pair) {
                pair.second.leaf = &root;
                pair.second.expiry = std::chrono::steady_clock::now() + ttl;
            },
            CacheEntry{&root, std::chrono::steady_clock::now() + ttl});
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

    // Remove expired entries
    for (const auto& key : all_keys) {
        entries.erase_if(key, [now](const auto& pair) { return now > pair.second.expiry; });
    }

    // Get current size after expiry cleanup
    all_keys.clear();
    entries.for_each([&](const auto& pair) { all_keys.push_back(pair.first); });

    if (all_keys.size() > maxSize) {
        size_t to_remove = all_keys.size() - maxSize;
        for (size_t i = 0; i < to_remove; ++i) {
            entries.erase(all_keys[i]);
        }
    }
}

} // namespace SP