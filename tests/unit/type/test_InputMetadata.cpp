#include "ext/doctest.h"
#include <pathspace/type/InputMetadata.hpp>

using namespace SP;

TEST_CASE("InputMetadata Fundamental") {
    SUBCASE("Int Serialize and Deserialize") {
        using ST = int;
        ST s{5};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        std::vector<SERIALIZATION_TYPE> bytes;
        imeta.serialize(&s, bytes);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Double Serialize and Deserialize") {
        using ST = double;
        ST s{5.35};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57.98};
        std::vector<SERIALIZATION_TYPE> bytes;
        imeta.serialize(&s, bytes);
        imeta.deserializePop(&s2, bytes);
        REQUIRE_EQ(s, s2);
    }
    SUBCASE("Multiple Int Serialize and Deserialize") {
        using ST = int;
        ST s{5};
        ST ss{6};
        ST sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        std::vector<SERIALIZATION_TYPE> bytes;
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
        ST s{5};
        ST ss{6};
        double ss2{23.56};
        double ss3{21.1};
        InputMetadata imeta2(InputMetadataT<double>{});
        ST sss{7};
        InputMetadata imeta(InputMetadataT<ST>{});
        ST s2{57};
        std::vector<SERIALIZATION_TYPE> bytes;
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
}