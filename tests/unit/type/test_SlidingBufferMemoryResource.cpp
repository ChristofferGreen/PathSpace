#include "third_party/doctest.h"

#include "CountingMemoryResource.hpp"
#include <pathspace/type/SlidingBuffer.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>

using SP::SlidingBuffer;
using SP::testing::CountingMemoryResource;

TEST_SUITE("type.sliding_buffer.memory_resource") {
TEST_CASE("default buffer uses the default resource") {
    SlidingBuffer buffer;
    CHECK(buffer.memoryResource() == std::pmr::get_default_resource());
}

TEST_CASE("storage is allocated from and returned to the given resource") {
    CountingMemoryResource counting;
    {
        SlidingBuffer buffer{&counting};
        CHECK(buffer.memoryResource() == &counting);
        CHECK(counting.allocations() == 1); // initial capacity reservation

        std::array<std::uint8_t, 1024> bytes{};
        buffer.append(bytes.data(), bytes.size()); // grows past the initial capacity
        CHECK(counting.allocations() >= 2);
        CHECK(counting.liveAllocations() == 1);
        CHECK(counting.liveBytes() >= bytes.size());
    }
    CHECK(counting.liveAllocations() == 0);
    CHECK(counting.liveBytes() == 0);
}

TEST_CASE("advance, compaction and resize keep using the resource") {
    CountingMemoryResource counting;
    {
        SlidingBuffer buffer{&counting};
        std::array<std::uint8_t, 256> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(i);
        }
        buffer.append(bytes.data(), bytes.size());
        buffer.advance(200); // past half of a buffer above the compaction threshold
        CHECK(buffer.virtualFront() == 0);
        CHECK(buffer.size() == 56);
        CHECK(buffer[0] == 200);
        buffer.resize(1024);
        CHECK(buffer.size() == 1024);
        CHECK(buffer.memoryResource() == &counting);
    }
    CHECK(counting.liveAllocations() == 0);
}

TEST_CASE("copy construction uses the default resource, allocator-extended copy the given one") {
    CountingMemoryResource counting;
    SlidingBuffer          source{&counting};
    std::array<std::uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
    source.append(bytes.data(), bytes.size());

    SlidingBuffer plainCopy{source};
    CHECK(plainCopy.memoryResource() == std::pmr::get_default_resource());

    auto const before = counting.allocations();
    SlidingBuffer extended{source, &counting};
    CHECK(extended.memoryResource() == &counting);
    CHECK(counting.allocations() == before + 1);
    REQUIRE(extended.size() == bytes.size());
    CHECK(extended[7] == 8);
}

TEST_CASE("move construction keeps the source resource, allocator-extended move re-homes it") {
    CountingMemoryResource counting;
    SlidingBuffer          source{&counting};
    std::array<std::uint8_t, 4> bytes{9, 8, 7, 6};
    source.append(bytes.data(), bytes.size());

    SlidingBuffer moved{std::move(source)};
    CHECK(moved.memoryResource() == &counting);
    REQUIRE(moved.size() == bytes.size());

    SlidingBuffer rehomed{std::move(moved), std::pmr::get_default_resource()};
    CHECK(rehomed.memoryResource() == std::pmr::get_default_resource());
    REQUIRE(rehomed.size() == bytes.size());
    CHECK(rehomed[0] == 9);
}

TEST_CASE("assignRaw copies into the buffer's own resource and clamps the front offset") {
    CountingMemoryResource counting;
    SlidingBuffer          buffer{&counting};

    std::array<std::uint8_t, 4> raw{1, 2, 3, 4};
    buffer.assignRaw(raw, 2);
    CHECK(buffer.memoryResource() == &counting);
    REQUIRE(buffer.size() == 2);
    CHECK(buffer[0] == 3);

    std::array<std::uint8_t, 2> small{5, 6};
    buffer.assignRaw(small, 99);
    CHECK(buffer.memoryResource() == &counting);
    CHECK(buffer.size() == 0);
}
}
