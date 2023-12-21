#include <catch2/catch_test_macros.hpp>
#include <pathspace/serialization/QueueStreamBuffer.hpp>

using namespace SP;

struct MyStruct {
    int data;
};

// Non-member serialize function for MyStruct
template <class Archive>
void serialize(Archive& ar, MyStruct& myStruct) {
    ar(myStruct.data);
}

TEST_CASE("QueueStreamBuffer", "[Serialization][QueueStreamBuffer]") {
    SECTION("Simple Construction", "[Serialization][QueueStreamBuffer]") {
        std::queue<std::byte> queue;
        QueueStreamBuffer qsb{queue};
    }

    SECTION("Simple Serialization", "[Serialization][QueueStreamBuffer]") {
        std::queue<std::byte> byteQueue;
        QueueStreamBuffer qbuf{byteQueue};
        std::ostream os(&qbuf);
        cereal::BinaryOutputArchive oarchive(os);

        MyStruct myStruct;
        oarchive(myStruct);
        REQUIRE(byteQueue.size() > 1);
    }
}