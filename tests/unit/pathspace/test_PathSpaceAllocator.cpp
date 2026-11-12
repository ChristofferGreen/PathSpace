#include "core/LockedMemoryResource.hpp"

#include "CountingMemoryResource.hpp"
#include "PathSpaceNestingTestSupport.hpp"

#include <atomic>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

using SP::LockedMemoryResource;
using SP::testing::CountingMemoryResource;

TEST_SUITE_BEGIN("pathspace.allocator");

TEST_CASE("a default space allocates from the default resource") {
    PathSpace space;
    CHECK(space.memoryResource() == std::pmr::get_default_resource());
}

TEST_CASE("tree nodes and payload buffers come from the supplied resource") {
    CountingMemoryResource counting;
    {
        PathSpace space{counting};
        CHECK(space.memoryResource() == &counting);
        CHECK(counting.allocations() == 0); // an empty tree allocates nothing

        REQUIRE(space.insert("/a/b/c", 42).errors.empty());
        auto const afterInsert = counting.allocations();
        CHECK(afterInsert > 0);

        auto value = space.read<int>("/a/b/c");
        REQUIRE(value.has_value());
        CHECK(*value == 42);
        CHECK(counting.allocations() == afterInsert); // reads do not allocate tree storage
    }
    CHECK(counting.liveAllocations() == 0);
    CHECK(counting.liveBytes() == 0);
}

TEST_CASE("the context carries the resource and nested construction honors it") {
    CountingMemoryResource counting;
    auto                   context = std::make_shared<PathSpaceContext>();
    REQUIRE(context->setMemoryResource(&counting).has_value());
    {
        PathSpace space{context};
        CHECK(space.memoryResource() == &counting);
        REQUIRE(space.insert("/x", std::string("payload")).errors.empty());
        CHECK(counting.allocations() > 0);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("clear returns node storage and the space stays usable") {
    CountingMemoryResource counting;
    PathSpace              space{counting};
    for (int i = 0; i < 100; ++i) {
        REQUIRE(space.insert("/n/" + std::to_string(i), i).errors.empty());
    }
    auto const live = counting.liveAllocations();
    space.clear();
    CHECK(counting.liveAllocations() < live);
    REQUIRE(space.insert("/n/0", 7).errors.empty());
    CHECK(*space.read<int>("/n/0") == 7);
}

TEST_CASE("take releases payload storage back to the resource") {
    CountingMemoryResource counting;
    PathSpace              space{counting};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(space.insert("/q", std::string(1000, 'x')).errors.empty());
    }
    auto const bytesBefore = counting.liveBytes();
    for (int i = 0; i < 10; ++i) {
        auto taken = space.take<std::string>("/q");
        REQUIRE(taken.has_value());
        CHECK(taken->size() == 1000);
    }
    CHECK(counting.liveBytes() <= bytesBefore);
    CHECK_FALSE(space.read<std::string>("/q").has_value());
}

TEST_CASE("copies and clones allocate from the source's resource") {
    CountingMemoryResource counting;
    {
        PathSpace source{counting};
        REQUIRE(source.insert("/a/b", 1).errors.empty());
        REQUIRE(source.insert("/a/c", std::string("two")).errors.empty());

        auto const before = counting.allocations();
        PathSpace  copy{source};
        CHECK(copy.memoryResource() == &counting);
        CHECK(counting.allocations() > before);
        CHECK(*copy.read<int>("/a/b") == 1);

        PathSpace cloned = source.clone();
        CHECK(cloned.memoryResource() == &counting);
        CHECK(*cloned.read<std::string>("/a/c") == "two");

        PathSpace assigned;
        assigned = source;
        CHECK(assigned.memoryResource() == std::pmr::get_default_resource()); // keeps its own
        CHECK(*assigned.read<int>("/a/b") == 1);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("nested spaces copied with their parent allocate from the parent's resource") {
    CountingMemoryResource counting;
    {
        PathSpace source{counting};
        REQUIRE(source.insert("/mount", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
        REQUIRE(source.insert("/mount/inner", 5).errors.empty());
        PathSpace copy = source.clone();
        CHECK(*copy.read<int>("/mount/inner") == 5);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("stress: many inserts, reads and takes with a custom resource") {
    CountingMemoryResource counting;
    {
        PathSpace   space{counting};
        constexpr int Count = 3000;
        for (int i = 0; i < Count; ++i) {
            auto const path = "/s/" + std::to_string(i % 50) + "/" + std::to_string(i);
            REQUIRE(space.insert(path, i).errors.empty());
        }
        for (int i = 0; i < Count; ++i) {
            auto const path  = "/s/" + std::to_string(i % 50) + "/" + std::to_string(i);
            auto       value = space.read<int>(path);
            REQUIRE(value.has_value());
            CHECK(*value == i);
        }
        for (int i = 0; i < Count; i += 2) {
            auto const path  = "/s/" + std::to_string(i % 50) + "/" + std::to_string(i);
            auto       value = space.take<int>(path);
            REQUIRE(value.has_value());
            CHECK(*value == i);
        }
        CHECK(counting.allocations() > 0);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("concurrent inserts and reads share a thread-safe resource") {
    CountingMemoryResource counting;
    {
        PathSpace space{counting};
        constexpr int Threads   = 4;
        constexpr int PerThread = 500;
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < Threads; ++t) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < PerThread; ++i) {
                    auto const path = "/t" + std::to_string(t) + "/" + std::to_string(i);
                    if (!space.insert(path, i).errors.empty()) {
                        failures.fetch_add(1);
                        continue;
                    }
                    auto value = space.read<int>(path);
                    if (!value.has_value() || *value != i) {
                        failures.fetch_add(1);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        CHECK(failures.load() == 0);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("a monotonic arena behind LockedMemoryResource outlives the space and is released at once") {
    CountingMemoryResource upstream;
    {
        std::pmr::monotonic_buffer_resource arena{&upstream};
        LockedMemoryResource                shared{&arena};
        {
            PathSpace space{shared};
            std::vector<std::thread> workers;
            for (int t = 0; t < 3; ++t) {
                workers.emplace_back([&, t] {
                    for (int i = 0; i < 200; ++i) {
                        (void)space.insert("/m" + std::to_string(t) + "/" + std::to_string(i), i);
                    }
                });
            }
            for (auto& worker : workers) {
                worker.join();
            }
            CHECK(*space.read<int>("/m2/199") == 199);
        }
        // The space is gone but a monotonic arena only gives memory back when it is released.
        CHECK(upstream.liveAllocations() > 0);
    }
    CHECK(upstream.liveAllocations() == 0);
}

TEST_SUITE_END();
