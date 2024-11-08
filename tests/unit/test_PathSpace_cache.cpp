#include "PathSpace.hpp"
#include "path/ConcretePath.hpp"

#include "ext/doctest.h"

#include <chrono>
#include <thread>

using namespace SP;

TEST_SUITE("PathSpace Cache") {
    TEST_CASE("Basic cache operations") {
        PathSpace space;

        /*SUBCASE("Cache hit on read") {
            ConcretePathString path("/test/path");
            space.insert(path, 42);

            auto result1 = space.read<int>(path);
            REQUIRE(result1.has_value());
            REQUIRE(result1.value() == 42);

            auto result2 = space.read<int>(path);
            REQUIRE(result2.has_value());

            auto [hits, misses, rate] = space.getCacheStats();
            REQUIRE(hits > 0);
        }

        SUBCASE("Cache miss on invalid path") {
            ConcretePathString invalidPath("invalid/path");
            auto result = space.read<int>(invalidPath);
            REQUIRE_FALSE(result.has_value());

            auto [hits, misses, rate] = space.getCacheStats();
            REQUIRE(misses > 0);
        }*/
    }

    TEST_CASE("Path type interactions") {
        PathSpace space;

        SUBCASE("String view compatibility") {
            ConcretePathString pathStr("/test/path");
            ConcretePathStringView pathView(pathStr.getPath());

            space.insert(pathStr, 42);

            auto result1 = space.read<int>(pathStr);
            auto result2 = space.read<int>(pathView);

            REQUIRE(result1.has_value());
            REQUIRE(result2.has_value());
            REQUIRE(result1.value() == result2.value());
        }
    }

    TEST_CASE("Cache invalidation") {
        PathSpace space;
        ConcretePathString path("/test/path");

        SUBCASE("Invalidation on insert") {
            space.insert(path, 42);
            auto result1 = space.read<int>(path);

            space.insert(path, 43);
            auto result2 = space.read<int>(path);

            REQUIRE(result2.value() == 43);
        }

        SUBCASE("Invalidation on extract") {
            space.insert(path, 42);
            auto result1 = space.read<int>(path);
            auto result2 = space.extract<int>(path);
            auto result3 = space.read<int>(path);

            REQUIRE(result2.has_value());
            REQUIRE_FALSE(result3.has_value());
        }
    }

    /*TEST_CASE("Thread safety") {
        PathSpace space;
        std::vector<std::thread> threads;

        for (int i = 0; i < 100; i++) {
            threads.emplace_back([&space, i]() {
                ConcretePathString path("/test/" + std::to_string(i));
                space.insert(path, i);

                for (int j = 0; j < 5; j++) {
                    auto result = space.read<int>(path);
                    REQUIRE(result.has_value());
                    REQUIRE(result.value() == i);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        auto [hits, misses, rate] = space.getCacheStats();
        REQUIRE(rate > 0.0);
    }*/

    /*TEST_CASE("Cache stats and control") {
        PathSpace space;
        ConcretePathString path("/test/path");

        SUBCASE("Stats tracking") {
            space.insert(path, 42);

            auto result1 = space.read<int>(path); // miss
            auto result2 = space.read<int>(path); // hit
            auto result3 = space.read<int>(path); // hit

            auto [hits, misses, rate] = space.getCacheStats();
            REQUIRE(hits == 2);
            REQUIRE(misses == 1);
            REQUIRE(rate == doctest::Approx(0.666).epsilon(0.01));
        }

        SUBCASE("Stats reset") {
            space.insert(path, 42);
            auto result = space.read<int>(path);

            space.resetCacheStats();
            auto [hits, misses, rate] = space.getCacheStats();
            REQUIRE(hits == 0);
            REQUIRE(misses == 0);
            REQUIRE(rate == 0.0);
        }
    }*/
}