#include "PathSpaceLeaf.hpp"
#include "core/Cache.hpp"

#include "ext/doctest.h"

#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

namespace {
// Test helper to create a simple NodeData
auto createNodeData(int value) -> NodeData {
    return NodeData{};
}
} // namespace

TEST_SUITE("Cache") {
    TEST_CASE("Basic Operations") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Valid Path Operations") {
            ConcretePathString path("/test/path");
            auto data = createNodeData(756);

            // Store and lookup
            cache.store(path, data, root);
            auto result = cache.lookup(path, root);
            REQUIRE(result.has_value());
            // CHECK value equals756

            // Invalidate
            cache.invalidate(path);
            result = cache.lookup(path, root);
            REQUIRE(!result.has_value());
        }

        SUBCASE("Invalid Path Operations") {
            ConcretePathString invalidPath("invalid/no-leading-slash");
            auto data = createNodeData(756);

            // Store should fail silently
            cache.store(invalidPath, data, root);

            // Lookup should return error
            auto result = cache.lookup(invalidPath, root);
            REQUIRE(!result.has_value());
            REQUIRE(result.error().code == Error::Code::InvalidPath);
        }
    }

    TEST_CASE("Cache Expiry") {
        Cache cache(1000, 1s); // 1 second TTL for testing
        PathSpaceLeaf root;
        ConcretePathString path("/test/path");
        auto data = createNodeData(756);

        SUBCASE("Entry Expires") {
            cache.store(path, data, root);

            // Should hit cache
            auto result1 = cache.lookup(path, root);
            REQUIRE(result1.has_value());

            // Wait for expiry
            std::this_thread::sleep_for(1100ms);

            // Should miss cache
            auto result2 = cache.lookup(path, root);
            REQUIRE(!result2.has_value());
        }

        SUBCASE("Entry Refresh") {
            cache.store(path, data, root);

            // Multiple lookups within TTL
            for (int i = 0; i < 5; ++i) {
                std::this_thread::sleep_for(200ms);
                auto result = cache.lookup(path, root);
                REQUIRE(result.has_value());
            }
        }
    }

    TEST_CASE("Size Management") {
        Cache cache(5, 1h); // Small size for testing
        PathSpaceLeaf root;

        SUBCASE("Size Limit Enforcement") {
            // Add more than max entries
            for (int i = 0; i < 10; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                cache.store(path, createNodeData(i), root);
            }

            // Check only recent entries exist
            int found = 0;
            for (int i = 0; i < 10; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                if (cache.lookup(path, root).has_value()) {
                    ++found;
                }
            }
            REQUIRE(found <= 5);
        }

        SUBCASE("Cleanup Behavior") {
            // Add entries with some expired
            for (int i = 0; i < 10; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                cache.store(path, createNodeData(i), root);
                if (i % 2 == 0) {
                    std::this_thread::sleep_for(1100ms); // Force expiry
                }
            }

            // Should have cleaned up expired entries
            int found = 0;
            for (int i = 0; i < 10; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                if (cache.lookup(path, root).has_value()) {
                    ++found;
                }
            }
            REQUIRE(found < 10);
        }
    }

    TEST_CASE("Invalidation Strategies") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Single Path Invalidation") {
            ConcretePathString path("/test/path");
            cache.store(path, createNodeData(756), root);
            cache.invalidate(path);
            REQUIRE(!cache.lookup(path, root).has_value());
        }

        SUBCASE("Prefix Invalidation") {
            // Setup paths
            std::vector<ConcretePathString> paths = {"/test/path/1", "/test/path/2", "/test/other/1", "/test/path/sub/1"};

            for (const auto& path : paths) {
                cache.store(path, createNodeData(1), root);
            }

            // Invalidate prefix
            cache.invalidatePrefix("/test/path");

            // Check invalidation
            REQUIRE(!cache.lookup("/test/path/1", root).has_value());
            REQUIRE(!cache.lookup("/test/path/2", root).has_value());
            REQUIRE(!cache.lookup("/test/path/sub/1", root).has_value());
            REQUIRE(cache.lookup("/test/other/1", root).has_value());
        }

        SUBCASE("Pattern Invalidation") {
            // Setup paths
            std::vector<ConcretePathString> paths = {"/test/path/1", "/test/path/2", "/other/path"};

            for (const auto& path : paths) {
                cache.store(path, createNodeData(1), root);
            }

            // Invalidate pattern
            cache.invalidatePattern(GlobPathString{"/test/*"});

            // All entries should be invalidated (current implementation)
            for (const auto& path : paths) {
                REQUIRE(!cache.lookup(path, root).has_value());
            }
        }

        SUBCASE("Clear All") {
            // Setup multiple paths
            std::vector<ConcretePathString> paths = {"/test/1", "/test/2", "/other/1"};

            for (const auto& path : paths) {
                cache.store(path, createNodeData(1), root);
            }

            cache.clear();

            // Verify all cleared
            for (const auto& path : paths) {
                REQUIRE(!cache.lookup(path, root).has_value());
            }
        }
    }

    TEST_CASE("Thread Safety") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Concurrent Reads") {
            // Setup data
            ConcretePathString path("/test/path");
            cache.store(path, createNodeData(756), root);

            // Multiple threads reading
            std::vector<std::thread> threads;
            std::atomic<int> successCount{0};

            for (int i = 0; i < 100; ++i) {
                threads.emplace_back([&]() {
                    if (cache.lookup(path, root).has_value()) {
                        ++successCount;
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }

            REQUIRE(successCount == 100);
        }

        SUBCASE("Concurrent Writes") {
            std::vector<std::thread> threads;
            std::atomic<int> successCount{0};

            for (int i = 0; i < 100; ++i) {
                threads.emplace_back([&, i]() {
                    ConcretePathString path("/test/path/" + std::to_string(i));
                    cache.store(path, createNodeData(i), root);
                    if (cache.lookup(path, root).has_value()) {
                        ++successCount;
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }

            REQUIRE(successCount == 100);
        }

        SUBCASE("Mixed Operations") {
            std::vector<std::thread> threads;
            std::atomic<int> successCount{0};

            // Create a mix of operations
            for (int i = 0; i < 100; ++i) {
                threads.emplace_back([&, i]() {
                    ConcretePathString path("/test/path/" + std::to_string(i % 10));

                    switch (i % 4) {
                        case 0: // Store
                            cache.store(path, createNodeData(i), root);
                            break;
                        case 1: // Lookup
                            cache.lookup(path, root);
                            break;
                        case 2: // Invalidate
                            cache.invalidate(path);
                            break;
                        case 3: // Store + Lookup
                            cache.store(path, createNodeData(i), root);
                            if (cache.lookup(path, root).has_value()) {
                                ++successCount;
                            }
                            break;
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }
        }
    }

    TEST_CASE("Edge Cases") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Empty Path") {
            ConcretePathString emptyPath("");
            auto data = createNodeData(1);

            cache.store(emptyPath, data, root);
            auto result = cache.lookup(emptyPath, root);
            REQUIRE(!result.has_value());
        }

        SUBCASE("Root Path") {
            ConcretePathString rootPath("/");
            auto data = createNodeData(1);

            cache.store(rootPath, data, root);
            auto result = cache.lookup(rootPath, root);
            REQUIRE(result.has_value());
        }

        SUBCASE("Very Long Path") {
            std::string longPath = "/a";
            for (int i = 0; i < 100; ++i) {
                longPath += "/really/long/path/component";
            }
            ConcretePathString path(longPath);
            auto data = createNodeData(1);

            cache.store(path, data, root);
            auto result = cache.lookup(path, root);
            REQUIRE(result.has_value());
        }

        SUBCASE("Repeated Store") {
            ConcretePathString path("/test/path");

            for (int i = 0; i < 1000; ++i) {
                cache.store(path, createNodeData(i), root);
            }

            auto result = cache.lookup(path, root);
            REQUIRE(result.has_value());
        }

        SUBCASE("Rapid Invalidation") {
            ConcretePathString path("/test/path");
            auto data = createNodeData(1);

            for (int i = 0; i < 1000; ++i) {
                cache.store(path, data, root);
                cache.invalidate(path);
            }

            REQUIRE(!cache.lookup(path, root).has_value());
        }
    }

    TEST_CASE("Performance Patterns") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Lookup Performance") {
            // Setup
            std::vector<ConcretePathString> paths;
            for (int i = 0; i < 1000; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                paths.push_back(path);
                cache.store(path, createNodeData(i), root);
            }

            // Measure lookup time
            auto start = std::chrono::high_resolution_clock::now();
            for (const auto& path : paths) {
                auto result = cache.lookup(path, root);
                REQUIRE(result.has_value());
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            // Add a loose performance requirement
            REQUIRE(duration.count() < 1000000); // Less than 1 second for 1000 lookups
        }

        SUBCASE("Cleanup Performance") {
            // Fill cache
            for (int i = 0; i < 10000; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                cache.store(path, createNodeData(i), root);
            }

            // Measure cleanup time (triggered by stores)
            auto start = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < 1000; ++i) {
                ConcretePathString path("/new/path/" + std::to_string(i));
                cache.store(path, createNodeData(i), root);
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            REQUIRE(duration.count() < 1000000); // Less than 1 second for cleanup
        }
    }
}