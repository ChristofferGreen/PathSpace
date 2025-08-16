#include "ext/doctest.h"
#include <string>
#include <pathspace/layer/PathIO.hpp>
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathIO base — unsupported operations") {
    SUBCASE("Direct usage returns UnsupportedOperation") {
        PathIO io;

        auto r1 = io.read<"/any/path", std::string>();
        CHECK_FALSE(r1.has_value());

        auto r2 = io.read<"/any/path", int>();
        CHECK_FALSE(r2.has_value());
    }
}

TEST_CASE("PathIO — mountability under PathSpace") {
    SUBCASE("Mounted at arbitrary prefix forwards but remains unsupported") {
        PathSpace space;
        space.insert<"/io">(std::make_unique<PathIO>());

        auto p = space.read<"/io/arbitrary/path", std::string>();
        CHECK_FALSE(p.has_value());
    }
}