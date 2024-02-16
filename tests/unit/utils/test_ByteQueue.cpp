#include <catch2/catch_test_macros.hpp>
#include <pathspace/utils/ByteQueue.hpp>
#include <pathspace/utils/ByteQueueSerializer.hpp>
#include <cstddef>
#include <stdexcept>

using namespace SP;

TEST_CASE("ByteQueue Tests", "[ByteQueue]") {
    ByteQueue bq;

    SECTION("Initial State") {
        REQUIRE(bq.begin() == bq.end());
    }

    SECTION("Push Back and Pop Front") {
        bq.push_back(std::byte(0x01));
        bq.push_back(std::byte(0x02));
        REQUIRE(bq.front() == std::byte(0x01));
        bq.pop_front();
        REQUIRE(bq.front() == std::byte(0x02));
    }

    SECTION("Bounds Checking") {
        bq.push_back(std::byte(0x01));
        REQUIRE(bq[0] == std::byte(0x01));
    }

    SECTION("Serialization and Deserialization") {
        // Populate ByteQueue
        for (int i = 0; i < 10; ++i) {
            bq.push_back(std::byte(i));
        }

        // Serialize
        std::stringstream ss;
        {
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(bq);
        }

        // Deserialize into a new ByteQueue
        ByteQueue newBq;
        {
            cereal::BinaryInputArchive iarchive(ss);
            iarchive(newBq);
        }

        // Check if the contents are same
        auto it1 = bq.begin();
        auto it2 = newBq.begin();
        while (it1 != bq.end() && it2 != newBq.end()) {
            REQUIRE(*it1 == *it2);
            ++it1;
            ++it2;
        }
        REQUIRE(it1 == bq.end());
        REQUIRE(it2 == newBq.end());
    }

    SECTION("Serialization and Deserialization Methods") {
        int obj{57};
        int obj2{};
        ByteQueue bq;
        serialize_to_bytequeue(bq, obj);
        deserialize_from_bytequeue(bq, obj2);
        REQUIRE(obj2 == obj);
    }
}
