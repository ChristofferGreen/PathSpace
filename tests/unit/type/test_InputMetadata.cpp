#include "third_party/doctest.h"
#include <pathspace/type/InputMetadata.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    SUBCASE("std::string_view serializes without providing deserialize") {
        std::string_view view{"view"};
        InputMetadata    meta(InputMetadataT<std::string_view>{});
        SlidingBuffer    bytes;

        REQUIRE(meta.serialize != nullptr);
        meta.serialize(&view, bytes);
        CHECK(bytes.size() == sizeof(uint32_t) + view.size());
        CHECK(meta.deserialize == nullptr);
    }

    SUBCASE("std::string deserializePop consumes buffer") {
        std::string   value{"buffer-pop"};
        std::string   out;
        InputMetadata meta(InputMetadataT<std::string>{});
        SlidingBuffer bytes;

        REQUIRE(meta.serialize != nullptr);
        meta.serialize(&value, bytes);
        REQUIRE(meta.deserializePop != nullptr);

        meta.deserializePop(&out, bytes);
        CHECK(out == value);
        CHECK(bytes.size() == 0);
    }

    SUBCASE("std::string deserialize throws when advertised size exceeds buffer") {
        InputMetadata meta(InputMetadataT<std::string>{});
        SlidingBuffer bytes;
        // Write header with size but omit payload.
        uint32_t size = 5;
        bytes.append(reinterpret_cast<uint8_t*>(&size), sizeof(size));
        std::string out;
        CHECK_THROWS_AS(StringSerializationHelper<std::string>::Deserialize(&out, bytes), std::runtime_error);
    }

    SUBCASE("string_view deserialize rejects non-string targets") {
        SlidingBuffer bytes;
        std::string   payload{"view-only"};

        StringSerializationHelper<std::string>::Serialize(&payload, bytes);

        std::string_view out;
        CHECK_THROWS_AS(StringSerializationHelper<std::string_view>::Deserialize(&out, bytes), std::runtime_error);
    }
}

TEST_CASE("InputMetadata covers typeInfo mapping and std::invoke_result deductions") {
    struct Callable {
        int operator()() const { return 3; }
    } callable;

    InputMetadata callableMeta(InputMetadataT<Callable>{});
    CHECK(callableMeta.typeInfo == &typeid(int));
    CHECK(callableMeta.dataCategory == DataCategory::Execution);

    // function pointer returns type maps through invoke_result
    using FnPtr = int (*)();
    InputMetadata fnMeta(InputMetadataT<FnPtr>{});
    CHECK(fnMeta.functionCategory == FunctionCategory::FunctionPointer);
    CHECK(fnMeta.typeInfo == &typeid(int));
}

TEST_CASE("InputMetadata handles std::function and unique_ptr categories") {
    std::function<int()> fn = [] { return 11; };
    InputMetadata funcMeta(InputMetadataT<decltype(fn)>{});
    CHECK(funcMeta.dataCategory == DataCategory::Execution);
    CHECK(funcMeta.functionCategory == FunctionCategory::StdFunction);
    CHECK(funcMeta.typeInfo == &typeid(int));
    CHECK(funcMeta.serialize == nullptr);
    CHECK(funcMeta.deserialize == nullptr);
    CHECK(funcMeta.deserializePop == nullptr);

    auto ptr = std::make_unique<int>(7);
    InputMetadata ptrMeta(InputMetadataT<decltype(ptr)>{});
    CHECK(ptrMeta.dataCategory == DataCategory::UniquePtr);
    CHECK(ptrMeta.functionCategory == FunctionCategory::None);
    CHECK(ptrMeta.typeInfo == &typeid(std::unique_ptr<int>));
    CHECK(ptrMeta.serialize == nullptr);
    CHECK(ptrMeta.deserialize == nullptr);
    CHECK(ptrMeta.deserializePop == nullptr);
    CHECK_FALSE(ptrMeta.podPreferred);
}

TEST_CASE("InputMetadata maps string views and literals to std::string metadata") {
    InputMetadata viewMeta(InputMetadataT<std::string_view>{});
    CHECK(viewMeta.typeInfo == &typeid(std::string));
    CHECK(viewMeta.dataCategory == DataCategory::SerializedData);
    CHECK(viewMeta.serialize != nullptr);
    CHECK(viewMeta.deserialize == nullptr);
    CHECK(viewMeta.deserializePop == nullptr);

    InputMetadata litMeta(InputMetadataT<char const[4]>{});
    CHECK(litMeta.typeInfo == &typeid(std::string));
    CHECK(litMeta.dataCategory == DataCategory::SerializedData);
    CHECK(litMeta.serialize != nullptr);
    CHECK(litMeta.deserialize == nullptr);
    CHECK(litMeta.deserializePop == nullptr);
}

TEST_CASE("InputMetadata treats shared_ptr as non-serializable") {
    auto shared = std::make_shared<int>(5);
    InputMetadata sharedMeta(InputMetadataT<decltype(shared)>{});

    CHECK(sharedMeta.dataCategory == DataCategory::None);
    CHECK(sharedMeta.functionCategory == FunctionCategory::None);
    CHECK(sharedMeta.typeInfo == &typeid(std::shared_ptr<int>));
    CHECK(sharedMeta.serialize == nullptr);
    CHECK(sharedMeta.deserialize == nullptr);
    CHECK(sharedMeta.deserializePop == nullptr);
    CHECK_FALSE(sharedMeta.podPreferred);
}

TEST_CASE("ValueSerializationHelper fundamental paths serialize and reject undersized buffers") {
    SlidingBuffer buffer;
    int           value = 1234;

    ValueSerializationHelper<int>::Serialize(&value, buffer);
    REQUIRE(buffer.size() == sizeof(int));

    int out = 0;
    ValueSerializationHelper<int>::Deserialize(&out, buffer);
    CHECK(out == value);

    SlidingBuffer small;
    small.append(reinterpret_cast<uint8_t*>(&value), sizeof(int) - 1); // too small
    CHECK_THROWS_AS(ValueSerializationHelper<int>::Deserialize(&out, small), std::runtime_error);
}

TEST_CASE("StringSerializationHelper detects truncated payloads") {
    SlidingBuffer bytes;
    uint32_t size = 4;
    bytes.append(reinterpret_cast<uint8_t*>(&size), sizeof(size)); // omit payload

    std::string out;
    CHECK_THROWS_AS(StringSerializationHelper<std::string>::Deserialize(&out, bytes), std::runtime_error);
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

    SUBCASE("capturing lambda routes through std::function execution path") {
        int  captures = 0;
        auto lambda   = [captures]() mutable -> int {
            ++captures;
            return captures;
        };
        using LambdaT = decltype(lambda);
        InputMetadata meta(InputMetadataT<LambdaT>{});
        CHECK(meta.dataCategory == DataCategory::Execution);
        CHECK(meta.functionCategory == FunctionCategory::StdFunction);
        CHECK(meta.serialize == nullptr);
        CHECK(meta.deserialize == nullptr);
    }

    SUBCASE("trivially copyable POD types are preferred and non-serializable pointers are not") {
        struct Trivial {
            int   a{1};
            float b{2.f};
            auto operator==(Trivial const&) const -> bool = default;
        };

        InputMetadata podMeta(InputMetadataT<Trivial>{});
        CHECK(podMeta.podPreferred);
        CHECK(podMeta.serialize != nullptr);
        CHECK(podMeta.deserialize != nullptr);

        using FnPtr = void (*)();
        InputMetadata fnMeta(InputMetadataT<FnPtr>{});
        CHECK(fnMeta.functionCategory == FunctionCategory::FunctionPointer);
        CHECK(fnMeta.serialize == nullptr);
        CHECK(fnMeta.deserialize == nullptr);
    }
}

TEST_CASE("InputMetadata covers additional literals, optionals, unique_ptr, and function pointers") {
    SUBCASE("string literal maps to std::string typeInfo and serialize-only path") {
        InputMetadata meta(InputMetadataT<char[6]>{});
        CHECK(meta.typeInfo == &typeid(std::string));
        CHECK(meta.dataCategory == DataCategory::SerializedData);
        CHECK(meta.serialize != nullptr);
        CHECK(meta.deserialize == nullptr);
    }

    SUBCASE("std::optional<int> is alpaca-compatible but not POD preferred") {
        using Opt = std::optional<int>;
        InputMetadata meta(InputMetadataT<Opt>{});
        CHECK(meta.dataCategory == DataCategory::SerializationLibraryCompatible);
        CHECK(meta.podPreferred);
        REQUIRE(meta.serialize != nullptr);
        REQUIRE(meta.deserialize != nullptr);

        Opt           value = 9;
        Opt           out{};
        SlidingBuffer bytes;
        meta.serialize(&value, bytes);
        meta.deserialize(&out, bytes);
        CHECK(out == value);
    }

    SUBCASE("unique_ptr is categorized but never serializable") {
        using Ptr = std::unique_ptr<int>;
        InputMetadata meta(InputMetadataT<Ptr>{});
        CHECK(meta.dataCategory == DataCategory::UniquePtr);
        CHECK_FALSE(meta.podPreferred);
        CHECK(meta.serialize == nullptr);
        CHECK(meta.deserialize == nullptr);
    }

    SUBCASE("function pointer is classified as FunctionPointer and keeps its type") {
        using Fn = double (*)(int);
        InputMetadata meta(InputMetadataT<Fn>{});
        CHECK(meta.functionCategory == FunctionCategory::None);
        CHECK(meta.dataCategory == DataCategory::FunctionPointer);
        CHECK(meta.typeInfo == &typeid(Fn));
    }
}


TEST_CASE("InputMetadata treats raw and shared pointers as non-serializable") {
    int value = 3;
    InputMetadata rawMeta(InputMetadataT<int*>{});
    CHECK(rawMeta.dataCategory == DataCategory::None);
    CHECK_FALSE(rawMeta.podPreferred);
    CHECK(rawMeta.serialize == nullptr);
    CHECK(rawMeta.deserialize == nullptr);

    auto shared = std::make_shared<int>(5);
    InputMetadata sharedMeta(InputMetadataT<std::shared_ptr<int>>{});
    CHECK(sharedMeta.dataCategory == DataCategory::None);
    CHECK_FALSE(sharedMeta.podPreferred);
    CHECK(sharedMeta.serialize == nullptr);
    CHECK(sharedMeta.deserialize == nullptr);

    using Unique = std::unique_ptr<int>;
    InputMetadata uniqueMeta(InputMetadataT<Unique>{});
    CHECK(uniqueMeta.dataCategory == DataCategory::UniquePtr);
    CHECK_FALSE(uniqueMeta.podPreferred);
    CHECK(uniqueMeta.serialize == nullptr);
    CHECK(uniqueMeta.deserialize == nullptr);
}

TEST_CASE("InputMetadata Alpaca-compatible containers serialize/deserialize") {
    SUBCASE("std::array<int,3> round-trips and stays POD-preferred") {
        std::array<int, 3> value{1, 2, 3};
        std::array<int, 3> out{0, 0, 0};
        InputMetadata       meta(InputMetadataT<decltype(value)>{});
        SlidingBuffer       bytes;

        REQUIRE(meta.serialize != nullptr);
        REQUIRE(meta.deserialize != nullptr);
        CHECK(meta.podPreferred);

        meta.serialize(&value, bytes);
        meta.deserialize(&out, bytes);
        CHECK(out == value);
    }

    SUBCASE("std::pair<int,int> round-trips via Alpaca") {
        std::pair<int, int> value{7, 9};
        std::pair<int, int> out{};
        InputMetadata       meta(InputMetadataT<decltype(value)>{});
        SlidingBuffer       bytes;

        REQUIRE(meta.serialize != nullptr);
        REQUIRE(meta.deserialize != nullptr);
        CHECK_FALSE(meta.podPreferred); // std::pair is not trivially copyable here

        meta.serialize(&value, bytes);
        meta.deserialize(&out, bytes);
        CHECK(out == value);
    }

    SUBCASE("std::vector<int> round-trips and is not POD preferred") {
        std::vector<int> value{1, 2, 3};
        std::vector<int> out{};
        InputMetadata    meta(InputMetadataT<decltype(value)>{});
        SlidingBuffer    bytes;

        REQUIRE(meta.serialize != nullptr);
        REQUIRE(meta.deserialize != nullptr);
        CHECK_FALSE(meta.podPreferred);

        meta.serialize(&value, bytes);
        meta.deserialize(&out, bytes);
        CHECK(out == value);
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
