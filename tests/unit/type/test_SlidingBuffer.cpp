#include "ext/doctest.h"

#include <pathspace/type/SlidingBuffer.hpp>

#include <cstring>
#include <string>
#include <vector>

struct TestStruct {
    int x;
    float y;
};

TEST_CASE("SlidingBuffer") {
    SUBCASE("Basic Buffer Operations") {
        SP::SlidingBuffer buffer;

        SUBCASE("initial state") {
            CHECK(buffer.remaining_size() == 0);
            CHECK(buffer.data.empty());
            CHECK(buffer.virtualFront == 0);
        }

        SUBCASE("adding and reading data") {
            std::vector<uint8_t> testData = {1, 2, 3, 4, 5};
            buffer.append(testData.data(), testData.size());

            CHECK(buffer.remaining_size() == 5);
            CHECK(buffer.current_data()[0] == 1);

            buffer.advance(2);
            CHECK(buffer.remaining_size() == 3);
            CHECK(buffer.current_data()[0] == 3);
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
            CHECK(buffer.virtualFront == 50);
            CHECK(buffer.data.size() == 100);

            // Read one more byte - should trigger compaction
            buffer.advance(1);
            CHECK(buffer.virtualFront == 0);
            CHECK(buffer.data.size() == 49);

            // Verify data survived compaction
            for (size_t i = 0; i < buffer.data.size(); i++) {
                CHECK(buffer.data[i] == i + 51);
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
        }
    }
}