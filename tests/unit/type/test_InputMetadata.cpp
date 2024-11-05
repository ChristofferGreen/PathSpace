#include "ext/doctest.h"
#include <pathspace/type/InputMetadata.hpp>

using namespace SP;

TEST_CASE("Type InputMetadata Fundamental") {
    SUBCASE("Int Serialize and Deserialize") {
        using ST = int;
        ST s{5};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        SlidingBuffer bytes;
        imeta.serialize2(&s, bytes);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Double Serialize and Deserialize") {
        using ST = double;
        ST s{5.35};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57.98};
        SlidingBuffer bytes;
        imeta.serialize2(&s, bytes);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Multiple Int Serialize and Deserialize") {
        using ST = int;
        ST s{5};
        ST ss{6};
        ST sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        SlidingBuffer bytes;
        imeta.serialize2(&s, bytes);
        imeta.serialize2(&ss, bytes);
        imeta.serialize2(&sss, bytes);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(s, s2);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(ss, s2);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(sss, s2);
        REQUIRE_EQ(bytes.size(), 0);
    }
    SUBCASE("Multiple Int/Double Serialize and Deserialize") {
        using ST = int;
        ST s{5};
        ST ss{6};
        double ss2{23.56};
        double ss3{21.1};
        InputMetadata imeta2(InputMetadataT<double>{});
        ST sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        SlidingBuffer bytes;
        imeta.serialize2(&s, bytes);
        imeta.serialize2(&ss, bytes);
        imeta2.serialize2(&ss2, bytes);
        imeta.serialize2(&sss, bytes);

        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(s, s2);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(ss, s2);
        imeta2.deserializePop2(&ss3, bytes);
        REQUIRE_EQ(ss3, ss2);
        imeta.deserializePop2(&s2, bytes);
        REQUIRE_EQ(sss, s2);
        REQUIRE_EQ(bytes.size(), 0);
    }
    SUBCASE("Function Pointer") {
        using TestFuncPtr = void (*)(int);
        TestFuncPtr testFunc = [](int x) {};
        InputMetadata im(InputMetadataT<TestFuncPtr>{});

        REQUIRE(im.dataCategory == DataCategory::FunctionPointer);
    }

    SUBCASE("Function Execution Pointer") {
        using TestFuncPtr = int (*)();
        InputMetadata im(InputMetadataT<TestFuncPtr>{});

        REQUIRE(im.executionCategory == ExecutionCategory::FunctionPointer);

        using TestPtr = int*;
        InputMetadata im3(InputMetadataT<TestPtr>{});

        REQUIRE(im3.executionCategory == ExecutionCategory::None);
    }
}