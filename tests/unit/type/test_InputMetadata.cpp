#include "third_party/doctest.h"
#include <pathspace/type/InputMetadata.hpp>

using namespace SP;

TEST_SUITE_BEGIN("type.input");

TEST_CASE("Type InputMetadata Fundamental") {
    SUBCASE("Int Serialize and Deserialize") {
        using ST = int;
        ST            s{5};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST            s2{57};
        SlidingBuffer bytes;
        imeta.serialize(&s, bytes);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Double Serialize and Deserialize") {
        using ST = double;
        ST            s{5.35};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST            s2{57.98};
        SlidingBuffer bytes;
        imeta.serialize(&s, bytes);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Multiple Int Serialize and Deserialize") {
        using ST = int;
        ST            s{5};
        ST            ss{6};
        ST            sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST            s2{57};
        SlidingBuffer bytes;
        imeta.serialize(&s, bytes);
        imeta.serialize(&ss, bytes);
        imeta.serialize(&sss, bytes);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(ss, s2);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(sss, s2);
        REQUIRE_EQ(bytes.size(), 0);
    }
    SUBCASE("Multiple Int/Double Serialize and Deserialize") {
        using ST = int;
        ST            s{5};
        ST            ss{6};
        double        ss2{23.56};
        double        ss3{21.1};
        InputMetadata imeta2(InputMetadataT<double>{});
        ST            sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST            s2{57};
        SlidingBuffer bytes;
        imeta.serialize(&s, bytes);
        imeta.serialize(&ss, bytes);
        imeta2.serialize(&ss2, bytes);
        imeta.serialize(&sss, bytes);

        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(ss, s2);
        imeta2.deserializePop(&ss3, bytes);
        REQUIRE_EQ(ss3, ss2);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(sss, s2);
        REQUIRE_EQ(bytes.size(), 0);
    }
    SUBCASE("Function Pointer") {
        using TestFuncPtr      = void (*)(int);
        TestFuncPtr   testFunc = [](int x) {};
        InputMetadata im(InputMetadataT<TestFuncPtr>{});

        REQUIRE(im.dataCategory == DataCategory::FunctionPointer);
    }

    SUBCASE("Function Execution Pointer") {
        using TestFuncPtr = int (*)();
        InputMetadata im(InputMetadataT<TestFuncPtr>{});

        REQUIRE(im.functionCategory == FunctionCategory::FunctionPointer);

        using TestPtr = int*;
        InputMetadata im3(InputMetadataT<TestPtr>{});

        REQUIRE(im3.functionCategory == FunctionCategory::None);
    }
}

TEST_CASE("String-like metadata serialize/deserialize") {
    SUBCASE("std::string round-trips") {
        std::string   value{"hello"};
        std::string   out;
        InputMetadata meta(InputMetadataT<std::string>{});
        SlidingBuffer bytes;

        REQUIRE(meta.serialize != nullptr);
        meta.serialize(&value, bytes);
        REQUIRE(bytes.size() > 0);

        REQUIRE(meta.deserialize != nullptr);
        meta.deserialize(&out, bytes);
        CHECK(out == value);
    }

    SUBCASE("string literal exposes only serialize") {
        InputMetadata meta(InputMetadataT<char const[6]>{});
        SlidingBuffer bytes;
        char const    literal[] = "alpha";
        REQUIRE(meta.serialize != nullptr);
        meta.serialize(literal, bytes);
        CHECK(bytes.size() == sizeof(uint32_t) + 5);
        CHECK(meta.deserialize == nullptr);
    }
}

TEST_CASE("InputMetadata execution and pointer categories") {
    SUBCASE("std::function classified as execution") {
        using Fn = std::function<int()>;
        InputMetadata meta(InputMetadataT<Fn>{});
        CHECK(meta.dataCategory == DataCategory::Execution);
        CHECK(meta.functionCategory == FunctionCategory::StdFunction);
        CHECK(meta.serialize == nullptr);
        CHECK(meta.deserialize == nullptr);
    }

    SUBCASE("unique_ptr classified and not POD preferred") {
        using Ptr = std::unique_ptr<int>;
        InputMetadata meta(InputMetadataT<Ptr>{});
        CHECK(meta.dataCategory == DataCategory::UniquePtr);
        CHECK_FALSE(meta.podPreferred);
        CHECK(meta.serialize == nullptr);
        CHECK(meta.deserialize == nullptr);
    }
}

TEST_CASE("InputMetadata handles errors and alpaca-compatible types") {
    SUBCASE("Fundamental deserialization throws on short buffer") {
        SlidingBuffer bytes;
        int           out = 0;
        CHECK_THROWS_AS((ValueSerializationHelper<int>::Deserialize(&out, bytes)), std::runtime_error);
    }

    SUBCASE("String helper catches truncated buffers and wrong target type") {
        SlidingBuffer empty;
        std::string   out;
        CHECK_THROWS_AS(StringSerializationHelper<std::string>::Deserialize(&out, empty), std::runtime_error);

        SlidingBuffer sizeOnly;
        uint32_t      sz = 3;
        sizeOnly.append(reinterpret_cast<uint8_t*>(&sz), sizeof(sz));
        CHECK_THROWS_AS(StringSerializationHelper<std::string>::Deserialize(&out, sizeOnly), std::runtime_error);

        SlidingBuffer literalBuf;
        literalBuf.append(reinterpret_cast<uint8_t*>(&sz), sizeof(sz));
        literalBuf.append(reinterpret_cast<uint8_t const*>("abc"), 3);
        char storage[4]{};
        CHECK_THROWS_AS((StringSerializationHelper<char[4]>::Deserialize(storage, literalBuf)), std::runtime_error);
    }

    SUBCASE("Alpaca-compatible struct round-trips and uses pop variant") {
        struct Point {
            int x{0};
            int y{0};
            auto operator==(Point const&) const -> bool = default;
        };

        InputMetadata meta(InputMetadataT<Point>{});
        Point         src{7, 9};
        Point         dst{};
        SlidingBuffer bytes;

        REQUIRE(meta.serialize != nullptr);
        REQUIRE(meta.deserialize != nullptr);
        meta.serialize(&src, bytes);
        meta.deserialize(&dst, bytes);
        CHECK(dst == src);

        // Exercise deserializePop to clear the buffer.
        bytes = SlidingBuffer{};
        meta.serialize(&src, bytes);
        meta.deserializePop(&dst, bytes);
        CHECK(bytes.size() == 0);
        CHECK(dst == src);
    }
}

TEST_SUITE_END();
