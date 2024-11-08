#pragma once
#include "SimpleCache.hpp"

namespace SP {

class CacheManager {
public:
    static auto instance() -> CacheManager& {
        static CacheManager instance;
        return instance;
    }

    auto cache() -> SimpleCache& {
        return cache_;
    }

    auto resetStats() -> void {
        cache_ = SimpleCache{};
    }

private:
    CacheManager() = default;
    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    SimpleCache cache_;
};

} // namespace SP