#include "ext/doctest.h"
#include <pathspace/layer/PathView.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace View") {
    SUBCASE("Function Types") {
        auto                       permissions = [](Iterator const& iterator) -> Permission {
            if (iterator.toStringView().starts_with("/legal"))
                return Permission{true, true, true};
            return Permission{false, false, false}; };
        std::shared_ptr<PathSpace> space       = std::make_shared<PathSpace>();
        PathView                   pspace(space, permissions);
        CHECK(pspace.insert("/legal/test", 4).nbrValuesInserted == 1);
        CHECK(pspace.insert("/illegal/test", 4).nbrValuesInserted == 0);
    }
}
