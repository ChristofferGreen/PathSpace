#include <catch2/catch_test_macros.hpp>
#include <pathspace/serialization/InputData.hpp>

using namespace SP;

struct MyStruct {
    int data = 5;
};

template <class Archive>
void serialize(Archive& ar, MyStruct& myStruct) {
    ar(myStruct.data);
}

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

        int b = 3;
        data.deserialize(&b, queue);
        REQUIRE(b == 5);
    }

    SECTION("Custom Struct Serialization/Deserialization", "[Type][InputData]") {
        MyStruct a{35};
        InputData data{a};

        std::queue<std::byte> queue;
        data.serialize(queue);

        MyStruct b{22};
        data.deserialize(&b, queue);
        REQUIRE(b.data == 35);
    }
}