#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_CASE("PathSpace listChildren enumerates child names") {
    PathSpace space;

    SUBCASE("Root children are sorted and deduplicated") {
        REQUIRE(space.insert("/beta", 1).nbrValuesInserted == 1);
        REQUIRE(space.insert("/alpha/value", 2).nbrValuesInserted == 1);
        REQUIRE(space.insert("/alpha/branch/leaf", 3).nbrValuesInserted == 1);

        auto names = space.listChildren();
        CHECK(names == std::vector<std::string>{"alpha", "beta"});

        auto alphaChildren = space.listChildren(ConcretePathStringView{"/alpha"});
        CHECK(alphaChildren == std::vector<std::string>{"branch", "value"});
    }

    SUBCASE("Missing paths return an empty list") {
        auto missing = space.listChildren(ConcretePathStringView{"/does/not/exist"});
        CHECK(missing.empty());
    }

    SUBCASE("Nested PathSpaces expose their children") {
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/childA", 1).nbrValuesInserted == 1);
        REQUIRE(nested->insert("/group/childB", 2).nbrValuesInserted == 1);

        REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

        auto mountChildren = space.listChildren(ConcretePathStringView{"/mount"});
        CHECK(mountChildren == std::vector<std::string>{"childA", "group"});

        auto nestedBranch = space.listChildren(ConcretePathStringView{"/mount/group"});
        CHECK(nestedBranch == std::vector<std::string>{"childB"});
    }
}
