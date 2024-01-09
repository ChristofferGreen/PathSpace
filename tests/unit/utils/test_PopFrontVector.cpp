#include <catch2/catch_test_macros.hpp>
#include <pathspace/utils/PopFrontVector.hpp>

using namespace SP;

TEST_CASE("PopFrontVector", "[PopFrontVector]") {
    PopFrontVector<int> vec;

    SECTION("New vector is empty") {
        REQUIRE(vec.isEmpty());
        REQUIRE(vec.size() == 0);
    }

    SECTION("Pushing back items") {
        vec.push_back(1);
        vec.push_back(2);
        REQUIRE(vec.size() == 2);
        REQUIRE(vec[0] == 1);
        REQUIRE(vec[1] == 2);
    }

    SECTION("Popping front item") {
        vec.push_back(1);
        vec.push_back(2);
        vec.pop_front();
        REQUIRE(vec.size() == 1);
        REQUIRE(vec[0] == 2);
    }

    SECTION("Emplacing back items") {
        vec.emplace_back(3);
        REQUIRE(vec.size() == 1);
        REQUIRE(vec[0] == 3);
    }

    SECTION("Clearing the vector") {
        vec.push_back(1);
        vec.clear();
        REQUIRE(vec.isEmpty());
        REQUIRE(vec.size() == 0);
    }

    SECTION("Accessing out of bounds throws exception") {
        vec.push_back(1);
        REQUIRE_THROWS_AS(vec[1], std::out_of_range);
    }

    SECTION("Iterators") {
        vec.push_back(1);
        vec.push_back(2);
        auto it = vec.begin();
        REQUIRE(*it == 1);
        ++it;
        REQUIRE(*it == 2);
        ++it;
        REQUIRE(it == vec.end());
    }

    SECTION("Garbage collection works") {
        for (int i = 0; i < 100; ++i) {
            vec.push_back(i);
        }
        for (int i = 0; i < 70; ++i) {
            vec.pop_front();
        }
        REQUIRE(vec.size() == 30);
        REQUIRE(vec[0] == 70);
        // Assuming garbage collection threshold is 30%
        REQUIRE(vec.begin() == vec.end() - 30); // Check if garbage collection has occurred
    }
}