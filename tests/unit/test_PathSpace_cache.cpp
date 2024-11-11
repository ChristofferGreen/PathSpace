// === test_cache_stats.cpp ===
#include "PathSpace.hpp"

#include "ext/doctest.h"

using namespace SP;

TEST_CASE("Cache statistics tracking") {
    PathSpace space(10); // Small cache size for testing

    SUBCASE("Read hits and misses") {
        // Insert test data
        space.insert("/test/value", 42);

        auto stats = space.getCacheStats();
        CHECK(stats.hits == 0);
        CHECK(stats.misses == 0);

        // First read should be a miss
        auto val1 = space.read<int>("/test/value");
        stats = space.getCacheStats();
        CHECK(stats.hits == 0);
        CHECK(stats.misses == 1);

        // Second read should be a hit
        auto val2 = space.read<int>("/test/value");
        stats = space.getCacheStats();
        CHECK(stats.hits == 1);
        CHECK(stats.misses == 1);

        // Reading non-existent path should be a miss
        auto val3 = space.read<int>("/test/nonexistent");
        stats = space.getCacheStats();
        CHECK(stats.hits == 1);
        CHECK(stats.misses == 2);
    }

    SUBCASE("Cache invalidation tracking") {
        space.insert("/test/a", 1);
        space.insert("/test/b", 2);

        // Prime the cache with reads
        space.read<int>("/test/a");
        space.read<int>("/test/b");

        auto stats = space.getCacheStats();
        CHECK(stats.invalidations == 0);

        // Extract should invalidate cache
        space.extract<int>("/test/a");
        stats = space.getCacheStats();
        CHECK(stats.invalidations == 1);

        // Insert with glob pattern should invalidate cache
        space.insert("/test/*", 3);
        stats = space.getCacheStats();
        CHECK(stats.invalidations > 1);
    }

    SUBCASE("Cache bypass doesn't affect stats") {
        space.insert("/test/value", 42);

        OutOptions opts;
        opts.bypassCache = true;

        // Read with bypass shouldn't affect stats
        auto val = space.read<int>("/test/value", opts);
        auto stats = space.getCacheStats();
        CHECK(stats.hits == 0);
        CHECK(stats.misses == 0);
    }

    SUBCASE("Stats reset") {
        space.insert("/test/value", 42);
        space.read<int>("/test/value");
        space.read<int>("/test/value");

        auto stats = space.getCacheStats();
        CHECK(stats.hits > 0);
        CHECK(stats.misses > 0);

        space.resetCacheStats();
        stats = space.getCacheStats();
        CHECK(stats.hits == 0);
        CHECK(stats.misses == 0);
        CHECK(stats.invalidations == 0);
    }
}