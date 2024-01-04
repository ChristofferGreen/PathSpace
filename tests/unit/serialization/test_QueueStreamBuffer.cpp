#include <catch2/catch_test_macros.hpp>
#include <pathspace/serialization/QueueStreamBuffer.hpp>

using namespace SP;

struct MyStruct {
    template <typename A>void serialize(A &ar) {ar(data);}
    int data = 5;
};

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

    SECTION("Simple Serialization", "[Serialization][QueueStreamBuffer]") {
        std::queue<std::byte> byteQueue;
        QueueStreamBuffer qbuf{byteQueue};
        std::ostream os(&qbuf);
        cereal::BinaryOutputArchive oarchive(os);

        MyStruct myStruct;
        myStruct.data = 6;
        oarchive(myStruct);
        REQUIRE(byteQueue.size() > 1);

        MyStruct myStruct2;
        myStruct2.data = 7;
        std::istream is(&qbuf);
        cereal::BinaryInputArchive iarchive(is);
        iarchive(myStruct2);
        REQUIRE(myStruct2.data==6);
    }
}