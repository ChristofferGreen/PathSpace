#include <catch2/catch_test_macros.hpp>
#include <pathspace/serialization/InputData.hpp>

using namespace SP;

struct MyStruct {
    template <typename A>
    void serialize(A &ar) {ar(data);}
    
    int data = 5;
};

TEST_CASE("InputData", "[Type][InputData]") {
    SECTION("Simple Construction", "[Type][InputData]") {
        int a{};
        InputData data{a};
    }

    SECTION("Simple Serialization/Deserialization", "[Type][InputData]") {
        int a = 5;
        InputData data{a};

        std::queue<std::byte> queue;
        data.serialize(queue);

        a = 3;
        data.deserialize(queue);
        REQUIRE(a == 5);
    }

    SECTION("Custom Struct Serialization/Deserialization", "[Type][InputData]") {
        MyStruct a{35};
        InputData data{a};

        std::queue<std::byte> queue;
        data.serialize(queue);

        a.data = 22;
        REQUIRE(a.data == 22);
        data.deserialize(queue);
        REQUIRE(a.data == 35);
    }
}