#include "ext/doctest.h"
#include <pathspace/type/InputData.hpp>

using namespace SP;

struct MyStruct {
    template <typename A>
    void serialize(A& ar) {
        ar(data);
    }

    int data = 5;
};

TEST_CASE("Type InputData") {
    SUBCASE("Simple Construction") {
        int a{};
        InputData data{a};
    }

    /*SUBCASE("Simple Serialization/Deserialization") {
        int a = 5;
        InputData data{a};

        std::queue<std::byte> queue;
        data.serialize(queue);

        a = 3;
        data.deserialize(queue);
        REQUIRE(a == 5);
    }*/

    /*SUBCASE("Custom Struct Serialization/Deserialization") {
        MyStruct a{35};
        InputData data{a};

        std::queue<std::byte> queue;
        data.serialize(queue);

        a.data = 22;
        REQUIRE(a.data == 22);
        data.deserialize(queue);
        REQUIRE(a.data == 35);
    }*/
}