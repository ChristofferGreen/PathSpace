#include "ext/doctest.h"

#include <pathspace/type/Serializer.hpp>
#include <pathspace/type/SlidingBuffer.hpp>

#include <cstring>
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

        SUBCASE("compaction behavior") {
            // Fill buffer with test pattern
            std::vector<uint8_t> pattern(100);
            for (int i = 0; i < 100; i++)
                pattern[i] = i;
            buffer.append(pattern.data(), pattern.size());

            // Read half - should not compact
            buffer.advance(50);
            CHECK(buffer.virtualFront() == 50);
            CHECK(buffer.sizeFull() == 100);

            // Read one more byte - should trigger compaction
            buffer.advance(1);
            CHECK(buffer.virtualFront() == 0);
            CHECK(buffer.sizeFull() == 49);

            // Verify data survived compaction
            for (size_t i = 0; i < buffer.size(); i++) {
                CHECK(buffer[i] == i + 51);
            }
        }

        SUBCASE("Serialization Tests") {
            SP::SlidingBuffer buffer;

            SUBCASE("Simple struct") {
                TestStruct original{42, 3.14f};
                auto val = SP::Serializer<TestStruct>::serialize(original, buffer);
                CHECK(!val.has_value());

                auto result = SP::Serializer<TestStruct>::deserialize(buffer);
                REQUIRE(result);
                CHECK(result->x == original.x);
                CHECK(result->y == original.y);
            }

            SUBCASE("complex struct") {
                TestComplexStruct original{.name = "test", .structs = {{1, 1.1f}, {2, 2.2f}, {3, 3.3f}}};

                CHECK(!SP::Serializer<TestComplexStruct>::serialize(original, buffer));

                auto result = SP::Serializer<TestComplexStruct>::deserialize(buffer);
                REQUIRE(result);
                CHECK(result->name == original.name);
                CHECK(result->structs.size() == original.structs.size());
                for (size_t i = 0; i < result->structs.size(); i++) {
                    CHECK(result->structs[i].x == original.structs[i].x);
                    CHECK(result->structs[i].y == original.structs[i].y);
                }
            }
        }
    }

    SUBCASE("Error Handling") {
        SP::SlidingBuffer buffer;

        SUBCASE("empty buffer") {
            auto result = SP::Serializer<TestStruct>::deserialize(buffer);
            CHECK(!result);
        }

        SUBCASE("corrupted header") {
            TestStruct original{42, 3.14f};
            CHECK(!SP::Serializer<TestStruct>::serialize(original, buffer));

            // Corrupt the header
            buffer[0] = 0xFF;

            auto result = SP::Serializer<TestStruct>::deserialize(buffer);
            CHECK(!result);
        }

        SUBCASE("truncated data") {
            TestStruct original{42, 3.14f};
            CHECK(!SP::Serializer<TestStruct>::serialize(original, buffer));

            // Truncate the buffer
            buffer.resize(buffer.size() - 1);

            auto result = SP::Serializer<TestStruct>::deserialize(buffer);
            CHECK(!result);
        }
    }
}