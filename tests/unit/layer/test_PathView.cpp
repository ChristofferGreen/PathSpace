#include "ext/doctest.h"
#include <pathspace/layer/PathView.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathSpace View") {
    std::shared_ptr<PathSpace> space = std::make_shared<PathSpace>();
    SUBCASE("Capability Types") {
        auto     permissions = [](Iterator const& iterator) -> Permission {
            if (iterator.toStringView().starts_with("/legal"))
                return Permission{true, true, true};
            return Permission{false, false, false}; };
        PathView pspace(space, permissions);
        CHECK(pspace.insert("/legal/test", 4).nbrValuesInserted == 1);
        CHECK(pspace.insert("/illegal/test", 4).nbrValuesInserted == 0);
    }

    SUBCASE("Mouse Space") {
        // CHECK(pspace->insert("/os/dev/io/pointer", PathIO{}).nbrValuesInserted == 1);
        // CHECK(pspace->read<"/os/devices/io/pointer/position", std::tuple<int, int>>() == std::make_tuple(0, 0));
        // CHECK(pspace->read<"/os/devices/io/pointer/position/0", std::tuple<int, int>>() == std::make_tuple(0, 0));
    }
}
