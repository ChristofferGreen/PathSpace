#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <algorithm>

using namespace SP;

TEST_SUITE_BEGIN("pathspace.list");

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

    SUBCASE("Indexed nested mounts surface suffixes") {
        auto first = std::make_unique<PathSpace>();
        REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
        auto second = std::make_unique<PathSpace>();
        REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);

        REQUIRE(space.insert("/node", std::move(first)).nbrSpacesInserted == 1);
        REQUIRE(space.insert("/node", std::move(second)).nbrSpacesInserted == 1);

        auto merged = space.listChildren(ConcretePathStringView{"/node"});
        CHECK(merged.size() == 2);
        CHECK(std::find(merged.begin(), merged.end(), "a") != merged.end());
        CHECK(std::find(merged.begin(), merged.end(), "b[1]") != merged.end());

        auto indexed = space.listChildren(ConcretePathStringView{"/node[1]"});
        CHECK(indexed == std::vector<std::string>{"b"});

        auto missing = space.listChildren(ConcretePathStringView{"/node[9]"});
        CHECK(missing.empty());
    }
}

TEST_SUITE_END();
