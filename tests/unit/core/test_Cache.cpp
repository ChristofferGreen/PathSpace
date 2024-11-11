#include "PathSpaceLeaf.hpp"
#include "core/Cache.hpp"
#include "core/InOptions.hpp"
#include "core/InsertReturn.hpp"
#include "core/NodeData.hpp"
#include "type/InputData.hpp"

#include "ext/doctest.h"

#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

namespace {

// Helper function to properly create NodeData with a value
auto createNodeData(int value) -> NodeData {
    InputData inputData{value};
    InsertReturn ret;
    InOptions options;
    return NodeData{inputData, options, ret};
}

// Helper function to set up the PathSpaceLeaf hierarchy for a given path
auto setupRoot(PathSpaceLeaf& root, const ConcretePathString& path) -> void {
    // Create a GlobPathStringView for the path
    GlobPathStringView globPath(std::string_view(path.getPath()));

    // Create dummy data to establish the path
    InputData inputData{0};
    InsertReturn ret;
    InOptions options;

    // Use the PathSpaceLeaf's in() function to create the path
    root.in(globPath.begin(), globPath.end(), inputData, options, ret);
}

// Helper to verify cache entry exists and matches the expected path
auto verifyCacheEntry(Cache& cache, PathSpaceLeaf& root, const ConcretePathString& path) -> bool {
    auto result = cache.lookup(path, root);
    if (!result.has_value()) {
        return false;
    }

    // Create string view for the path
    ConcretePathStringView pathView(std::string_view(path.getPath()));

    // Verify that the cached leaf matches the actual path
    auto actualLeaf = root.getLeafNode(pathView.begin(), pathView.end());
    if (!actualLeaf.has_value()) {
        return false;
    }

    return result.value() == actualLeaf.value();
}

} // namespace

TEST_SUITE("Cache") {
    TEST_CASE("Basic Operations") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Store and Lookup") {
            ConcretePathString path("/test/path");
            setupRoot(root, path);
            auto data = createNodeData(756);

            cache.store(path, root);
            REQUIRE(verifyCacheEntry(cache, root, path));

            cache.invalidate(path);
            REQUIRE_FALSE(verifyCacheEntry(cache, root, path));
        }

        SUBCASE("Root Path Operations") {
            ConcretePathString rootPath("/");
            auto data = createNodeData(1);

            cache.store(rootPath, root);
            REQUIRE(verifyCacheEntry(cache, root, rootPath));
        }
    }

    TEST_CASE("Size Management") {
        size_t const MAX_CACHE_SIZE = 5;
        Cache cache(MAX_CACHE_SIZE); // Removed TTL parameter
        PathSpaceLeaf root;

        SUBCASE("Enforce Size Limit") {
            // Fill cache beyond its capacity
            std::vector<ConcretePathString> paths;
            for (size_t i = 0; i < MAX_CACHE_SIZE * 2; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                setupRoot(root, path);
                paths.push_back(path);
                cache.store(path, root);
            }

            // Only check that we have MAX_CACHE_SIZE entries
            size_t found = 0;
            for (const auto& path : paths) {
                if (verifyCacheEntry(cache, root, path)) {
                    ++found;
                }
            }

            REQUIRE(found == MAX_CACHE_SIZE);
        }
    }

    TEST_CASE("Invalidation Strategies") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Prefix Invalidation") {
            // Setup test paths
            std::vector<ConcretePathString> paths = {"/test/path/1", "/test/path/2", "/test/other/1", "/test/path/sub/1"};

            for (const auto& path : paths) {
                setupRoot(root, path);
                cache.store(path, root);
                REQUIRE(verifyCacheEntry(cache, root, path));
            }

            // Invalidate by prefix
            cache.invalidatePrefix("/test/path");

            // Verify correct invalidation
            REQUIRE_FALSE(verifyCacheEntry(cache, root, paths[0])); // /test/path/1
            REQUIRE_FALSE(verifyCacheEntry(cache, root, paths[1])); // /test/path/2
            REQUIRE(verifyCacheEntry(cache, root, paths[2]));       // /test/other/1 should still exist
            REQUIRE_FALSE(verifyCacheEntry(cache, root, paths[3])); // /test/path/sub/1
        }

        SUBCASE("Clear All") {
            std::vector<ConcretePathString> paths = {"/test/1", "/test/2", "/other/1"};

            for (const auto& path : paths) {
                setupRoot(root, path);
                cache.store(path, root);
            }

            cache.clear();

            for (const auto& path : paths) {
                REQUIRE_FALSE(verifyCacheEntry(cache, root, path));
            }
        }
    }

    TEST_CASE("Thread Safety") {
        Cache cache;
        PathSpaceLeaf root;
        std::mutex rootMutex;

        SUBCASE("Concurrent Reads") {
            ConcretePathString path("/test/path");
            setupRoot(root, path);
            cache.store(path, root);

            std::vector<std::thread> threads;
            std::atomic<int> successCount{0};

            for (int i = 0; i < 100; ++i) {
                threads.emplace_back([&]() {
                    if (verifyCacheEntry(cache, root, path)) {
                        ++successCount;
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }

            REQUIRE(successCount == 100);
        }

        SUBCASE("Concurrent Mixed Operations") {
            // Pre-setup paths
            for (int i = 0; i < 10; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                std::lock_guard<std::mutex> lock(rootMutex);
                setupRoot(root, path);
            }

            std::vector<std::thread> threads;
            std::atomic<int> successCount{0};

            for (int i = 0; i < 100; ++i) {
                threads.emplace_back([&, i]() {
                    ConcretePathString path("/test/path/" + std::to_string(i % 10));

                    switch (i % 3) {
                        case 0: { // Store
                            std::lock_guard<std::mutex> lock(rootMutex);
                            cache.store(path, root);
                            break;
                        }
                        case 1: // Lookup
                            if (verifyCacheEntry(cache, root, path)) {
                                ++successCount;
                            }
                            break;
                        case 2: // Invalidate
                            cache.invalidate(path);
                            break;
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }

            // We can't make exact assertions about successCount due to race conditions,
            // but we can verify it's within reasonable bounds
            REQUIRE(successCount > 0);
            REQUIRE(successCount < 100);
        }
    }

    TEST_CASE("Performance Patterns") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Lookup Performance") {
            std::vector<ConcretePathString> paths;
            // Setup phase
            for (int i = 0; i < 1000; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                setupRoot(root, path);
                paths.push_back(path);
                cache.store(path, root);
            }

            // Measure lookup performance
            auto start = std::chrono::high_resolution_clock::now();

            for (const auto& path : paths) {
                REQUIRE(verifyCacheEntry(cache, root, path));
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            // Performance requirement: less than 1 second for 1000 lookups
            REQUIRE(duration.count() < 1000);
        }

        SUBCASE("Cache Cleanup Performance") {
            // Fill cache with initial data
            for (int i = 0; i < 1000; ++i) {
                ConcretePathString path("/test/path/" + std::to_string(i));
                setupRoot(root, path);
                cache.store(path, root);
            }

            // Measure cleanup performance during new insertions
            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 100; ++i) {
                ConcretePathString path("/new/path/" + std::to_string(i));
                setupRoot(root, path);
                cache.store(path, root);
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            // Performance requirement: less than 500ms for cleanup during 100 insertions
            REQUIRE(duration.count() < 500);
        }
    }

    TEST_CASE("Edge Cases") {
        Cache cache;
        PathSpaceLeaf root;

        SUBCASE("Very Long Path") {
            std::string longPath = "/a";
            for (int i = 0; i < 50; ++i) {
                longPath += "/really/long/path/component";
            }

            ConcretePathString path(longPath);
            setupRoot(root, path);
            auto data = createNodeData(1);

            cache.store(path, root);
            REQUIRE(verifyCacheEntry(cache, root, path));
        }

        SUBCASE("Repeated Operations") {
            ConcretePathString path("/test/path");
            setupRoot(root, path);

            // Perform repeated store/invalidate cycles
            for (int i = 0; i < 100; ++i) {
                cache.store(path, root);
                REQUIRE(verifyCacheEntry(cache, root, path));
                cache.invalidate(path);
                REQUIRE_FALSE(verifyCacheEntry(cache, root, path));
            }
        }
    }
}