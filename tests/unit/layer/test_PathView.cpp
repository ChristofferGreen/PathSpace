#include "ext/doctest.h"
#include <pathspace/layer/PathView.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace View") {
    SUBCASE("Function Types") {
        auto     permissions = [](Iterator const& iterator) -> Permission { return Permission{true, true, true}; };
        PathView pspace(permissions);
        pspace.insert("/test", 4);
    }
}
