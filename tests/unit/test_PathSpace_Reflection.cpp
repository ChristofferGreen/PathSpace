#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

struct RS {
    float x = 0.0;
    float y = 0.0;
    float z = 0.0;
};

TEST_SUITE("pathspace.reflection") {
TEST_CASE("PathSpace Reflection") {
    PathSpace pspace;

    SUBCASE("Basic Reflection") {
        CHECK(pspace.insert<"/ref">(RS{1.0, 2.0, 3.0}).nbrValuesInserted == 1);
        CHECK(pspace.read<"/ref/x", float>().value() == 1.0);
        CHECK(pspace.insert<"/ref/y">(45.5).nbrValuesInserted == 1);
        CHECK(pspace.read<"/ref/y", float>().value() == 45.5);
    }
}

TEST_CASE("PathSpace Cache Based Programming") {
    PathSpace pspace;

    SUBCASE("Basic Cache") {
        for (int i = 0; i < 1000; ++i)
            pspace.insert<"/c">(i);
        pspace.read<"/c", int>([](int& i) -> int { return i * 2; });
        for (int i = 0; i < 1000; ++i)
            CHECK(pspace.take<"/c">().value() == i * 2);
    }

    SUBCASE("Basic Cache") {
    }
}

TEST_CASE("PathSpace Scripting") {
}

TEST_CASE("PathSpace Scripting UI") {
}
}
