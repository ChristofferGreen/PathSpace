#pragma once
#include <chrono>
#include <memory>

namespace SP {

class PathSpaceLeaf;

struct CacheEntry {
    std::weak_ptr<PathSpaceLeaf> leaf;
    std::chrono::steady_clock::time_point expiry;

    auto isExpired() const -> bool {
        return std::chrono::steady_clock::now() > expiry;
    }

    auto isValid() const -> bool {
        return !leaf.expired() && !isExpired();
    }
};

} // namespace SP