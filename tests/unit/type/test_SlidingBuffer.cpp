#include "third_party/doctest.h"

#include <pathspace/type/SlidingBuffer.hpp>
#include <pathspace/type/serialization.hpp>

#include <array>
#include <cstring>
#include <numeric> // for std::iota
#include <span>
#include <string>
#include <vector>

struct TestStruct {
    int x;
    float y;
};

struct TestComplexStruct {
    std::string name;
    std::vector<TestStruct> structs;
};

TEST_CASE("SlidingBuffer") {
    SUBCASE("Basic Buffer Operations") {
        SP::SlidingBuffer buffer;

        SUBCASE("initial state") {
            CHECK(buffer.size() == 0);
            CHECK(buffer.empty());
            CHECK(buffer.virtualFront() == 0);
        }

        SUBCASE("adding and reading data") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());

            CHECK(buffer.size() == 5);
            CHECK(buffer.data()[0] == 1);

            buffer.advance(2);
            CHECK(buffer.size() == 3);
            CHECK(buffer.data()[0] == 3);
        }

        SUBCASE("span-based append") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            std::span<uint8_t const> dataSpan{testData};
            buffer.append(dataSpan);

            CHECK(buffer.size() == 5);
            for (size_t i = 0; i < testData.size(); ++i) {
                CHECK(buffer[i] == testData[i]);
            }
        }

        SUBCASE("operator[] const access") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());

            const auto& constBuffer = buffer;
            for (size_t i = 0; i < testData.size(); ++i) {
                CHECK(constBuffer[i] == testData[i]);
            }
        }

        SUBCASE("at() bounds checking") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());

            // Valid access
            CHECK(buffer.at(0) == 1);
            CHECK(buffer.at(4) == 5);

            // Invalid access
            CHECK_THROWS_AS(static_cast<void>(buffer.at(5)), std::out_of_range);

            // Const access
            const auto& constBuffer = buffer;
            CHECK(constBuffer.at(0) == 1);
            CHECK_THROWS_AS(static_cast<void>(constBuffer.at(5)), std::out_of_range);
        }

        SUBCASE("iterator behavior") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());

            // Test range-based for
            size_t index = 0;
            for (const auto& value : buffer) {
                CHECK(value == testData[index++]);
            }

            // Test iterator arithmetic
            auto it = buffer.begin();
            it += 2;
            CHECK(*it == testData[2]);

            // Test const iterators
            const auto& constBuffer = buffer;
            index = 0;
            for (const auto& value : constBuffer) {
                CHECK(value == testData[index++]);
            }
        }

        SUBCASE("raw iterators") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());
            buffer.advance(2);

            // Check raw iterators still see the whole buffer
            auto rawSize = std::distance(buffer.rawBegin(), buffer.rawEnd());
            CHECK(rawSize == 5);
            CHECK(*buffer.rawBegin() == 1);
        }

        SUBCASE("compaction behavior") {
            // Fill buffer with test pattern
            std::vector<uint8_t> pattern(100);
            for (int i = 0; i < 100; i++) {
                pattern[i] = i;
            }
            buffer.append(pattern.data(), pattern.size());

            // Read half - should not compact
            buffer.advance(50);
            CHECK(buffer.virtualFront() == 50);
            CHECK(buffer.rawSize() == 100);

            // Read one more byte - should trigger compaction
            buffer.advance(1);
            CHECK(buffer.virtualFront() == 0);
            CHECK(buffer.rawSize() == 49);

            // Verify data survived compaction
            for (size_t i = 0; i < buffer.size(); i++) {
                CHECK(buffer[i] == i + 51);
            }
        }

        SUBCASE("compaction threshold behavior") {
            // Test with buffer smaller than COMPACT_THRESHOLD
            std::vector<uint8_t> small(32);
            std::iota(small.begin(), small.end(), 0);
            buffer.append(small.data(), small.size());

            buffer.advance(16); // Should not trigger compaction
            CHECK(buffer.virtualFront() == 16);
            CHECK(buffer.rawSize() == 32);

            // Test with buffer larger than COMPACT_THRESHOLD
            std::vector<uint8_t> large(128);
            std::iota(large.begin(), large.end(), 0);
            buffer.append(large.data(), large.size());

            buffer.advance(100); // Should trigger compaction
            CHECK(buffer.virtualFront() == 0);
            CHECK(buffer.rawSize() < 128);
        }

        SUBCASE("resize behavior") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());
            buffer.advance(2);
            buffer.resize(2); // Should compact and resize

            CHECK(buffer.virtualFront() == 0);
            CHECK(buffer.size() == 2);
            CHECK(buffer[0] == 3);
            CHECK(buffer[1] == 4);
        }
    }

    SUBCASE("Serialization Tests") {
        SP::SlidingBuffer buffer;

        SUBCASE("Simple struct") {
            TestStruct original{42, 3.14f};
            auto val = SP::serialize<TestStruct>(original, buffer);
            CHECK(!val.has_value());

            auto result = SP::deserialize<TestStruct>(buffer);
            REQUIRE(result);
            CHECK(result->x == original.x);
            CHECK(result->y == original.y);
        }

        SUBCASE("complex struct") {
            TestComplexStruct original{.name = "test", .structs = {{1, 1.1f}, {2, 2.2f}, {3, 3.3f}}};

            CHECK(!SP::serialize<TestComplexStruct>(original, buffer));

            auto result = SP::deserialize<TestComplexStruct>(buffer);
            REQUIRE(result);
            CHECK(result->name == original.name);
            CHECK(result->structs.size() == original.structs.size());
            for (size_t i = 0; i < result->structs.size(); i++) {
                CHECK(result->structs[i].x == original.structs[i].x);
                CHECK(result->structs[i].y == original.structs[i].y);
            }
        }
    }

    SUBCASE("Error Handling") {
        SP::SlidingBuffer buffer;

        SUBCASE("empty buffer") {
            auto result = SP::deserialize<TestStruct>(buffer);
            CHECK(!result);
        }

        SUBCASE("corrupted header") {
            TestStruct original{42, 3.14f};
            CHECK(!SP::serialize<TestStruct>(original, buffer));

            buffer.at(0) = 0xFF; // Using at() instead of operator[]

            auto result = SP::deserialize<TestStruct>(buffer);
            CHECK(!result);
        }

        SUBCASE("truncated data") {
            TestStruct original{42, 3.14f};
            CHECK(!SP::serialize<TestStruct>(original, buffer));

            buffer.resize(buffer.size() - 1);

            auto result = SP::deserialize<TestStruct>(buffer);
            CHECK(!result);
        }
    }

    SUBCASE("Move Semantics") {
        SP::SlidingBuffer buffer;
        std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
        buffer.append(testData.data(), testData.size());

        SUBCASE("move construction") {
            SP::SlidingBuffer moved{std::move(buffer)};
            CHECK(moved.size() == 5);
            CHECK(buffer.empty()); // NOLINT: intentionally checking moved-from object
        }

        SUBCASE("move assignment") {
            SP::SlidingBuffer other;
            other = std::move(buffer);
            CHECK(other.size() == 5);
            CHECK(buffer.empty()); // NOLINT: intentionally checking moved-from object
        }

        SUBCASE("rvalue operator[]") {
            auto value = std::move(buffer)[2];
            CHECK(value == 3);
        }
    }
}