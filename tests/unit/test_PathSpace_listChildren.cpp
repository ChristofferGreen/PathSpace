#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <algorithm>

using namespace SP;

TEST_SUITE_BEGIN("pathspace.list");

TEST_CASE("PathSpace listChildren enumerates child names") {
    PathSpace space;

    SUBCASE("Root children are sorted and deduplicated") {
        REQUIRE(space.insert("/beta", 1).nbrValuesInserted == 1);
        REQUIRE(space.insert("/alpha/value", 2).nbrValuesInserted == 1);
        REQUIRE(space.insert("/alpha/branch/leaf", 3).nbrValuesInserted == 1);

        auto names = space.read<Children>("/");
        REQUIRE(names.has_value());
        CHECK(names->names == std::vector<std::string>{"alpha", "beta"});

        auto alphaChildren = space.read<Children>(ConcretePathStringView{"/alpha"});
        REQUIRE(alphaChildren.has_value());
        CHECK(alphaChildren->names == std::vector<std::string>{"branch", "value"});
    }

    SUBCASE("Missing paths return an empty list") {
        auto missing = space.read<Children>(ConcretePathStringView{"/does/not/exist"});
        REQUIRE(missing.has_value());
        CHECK(missing->names.empty());
    }

    SUBCASE("Nested PathSpaces expose their children") {
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/childA", 1).nbrValuesInserted == 1);
        REQUIRE(nested->insert("/group/childB", 2).nbrValuesInserted == 1);

        REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

        auto mountChildren = space.read<Children>(ConcretePathStringView{"/mount"});
        REQUIRE(mountChildren.has_value());
        CHECK(mountChildren->names == std::vector<std::string>{"childA", "group"});

        auto nestedBranch = space.read<Children>(ConcretePathStringView{"/mount/group"});
        REQUIRE(nestedBranch.has_value());
        CHECK(nestedBranch->names == std::vector<std::string>{"childB"});
    }

    SUBCASE("Indexed nested mounts surface suffixes") {
        auto first = std::make_unique<PathSpace>();
        REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
        auto second = std::make_unique<PathSpace>();
        REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);

        REQUIRE(space.insert("/node", std::move(first)).nbrSpacesInserted == 1);
        REQUIRE(space.insert("/node", std::move(second)).nbrSpacesInserted == 1);

        auto merged = space.read<Children>(ConcretePathStringView{"/node"});
        REQUIRE(merged.has_value());
        CHECK(merged->names.size() == 2);
        CHECK(std::find(merged->names.begin(), merged->names.end(), "a") != merged->names.end());
        CHECK(std::find(merged->names.begin(), merged->names.end(), "b[1]") != merged->names.end());

        auto indexed = space.read<Children>(ConcretePathStringView{"/node[1]"});
        REQUIRE(indexed.has_value());
        CHECK(indexed->names == std::vector<std::string>{"b"});

        auto missing = space.read<Children>(ConcretePathStringView{"/node[9]"});
        REQUIRE(missing.has_value());
        CHECK(missing->names.empty());
    }

    SUBCASE("Indexed nested paths traverse deeper segments") {
        auto first = std::make_unique<PathSpace>();
        REQUIRE(first->insert("/inner/grand/leaf", 1).nbrValuesInserted == 1);
        REQUIRE(first->insert("/inner/grand/other", 2).nbrValuesInserted == 1);
        auto second = std::make_unique<PathSpace>();
        REQUIRE(second->insert("/inner/grand/alt", 3).nbrValuesInserted == 1);

        REQUIRE(space.insert("/mount", std::move(first)).nbrSpacesInserted == 1);
        REQUIRE(space.insert("/mount", std::move(second)).nbrSpacesInserted == 1);

        auto firstNames = space.read<Children>(ConcretePathStringView{"/mount[0]/inner/grand"});
        REQUIRE(firstNames.has_value());
        CHECK(firstNames->names == std::vector<std::string>{"leaf", "other"});

        auto secondNames = space.read<Children>(ConcretePathStringView{"/mount[1]/inner/grand"});
        REQUIRE(secondNames.has_value());
        CHECK(secondNames->names == std::vector<std::string>{"alt"});

        auto missingNames = space.read<Children>(ConcretePathStringView{"/mount[9]/inner/grand"});
        REQUIRE(missingNames.has_value());
        CHECK(missingNames->names.empty());
    }

    SUBCASE("Children can be read through alias and trellis layers") {
        auto backing = std::make_shared<PathSpace>();
        REQUIRE(backing->insert("/root/a", 1).nbrValuesInserted == 1);
        REQUIRE(backing->insert("/root/b", 2).nbrValuesInserted == 1);
        REQUIRE(backing->insert("/root/group/c", 3).nbrValuesInserted == 1);

        PathAlias alias{backing, "/root"};
        auto aliasKids = alias.read<Children>("/");
        REQUIRE(aliasKids.has_value());
        CHECK(aliasKids->names == std::vector<std::string>{"a", "b", "group"});

        PathSpaceTrellis trellis{backing};
        trellis.adoptContextAndPrefix(backing->sharedContext(), "/root");

        auto trellisKids = trellis.read<Children>("/");
        REQUIRE(trellisKids.has_value());
        CHECK(trellisKids->names == std::vector<std::string>{"a", "b", "group"});

        auto nestedKids = trellis.read<Children>(ConcretePathStringView{"/group"});
        REQUIRE(nestedKids.has_value());
        CHECK(nestedKids->names == std::vector<std::string>{"c"});
    }

    SUBCASE("Alias retarget updates children view") {
        auto backing = std::make_shared<PathSpace>();
        REQUIRE(backing->insert("/one/x", 1).nbrValuesInserted == 1);
        REQUIRE(backing->insert("/two/y", 2).nbrValuesInserted == 1);

        PathAlias alias{backing, "/one"};
        auto kids = alias.read<Children>("/");
        REQUIRE(kids.has_value());
        CHECK(kids->names == std::vector<std::string>{"x"});

        alias.setTargetPrefix("/two");
        auto kids2 = alias.read<Children>("/");
        REQUIRE(kids2.has_value());
        CHECK(kids2->names == std::vector<std::string>{"y"});
    }

    SUBCASE("Trellis _system children are hidden") {
        auto backing = std::make_shared<PathSpace>();
        REQUIRE(backing->insert("/_system/debug", 1).nbrValuesInserted == 1);
        PathSpaceTrellis trellis{backing};
        trellis.adoptContextAndPrefix(backing->sharedContext(), "");

        auto sysKids = trellis.read<Children>(ConcretePathStringView{"/_system"});
        REQUIRE(sysKids.has_value());
        CHECK(sysKids->names.empty());
    }

    SUBCASE("Alias surfaces indexed nested mounts") {
        auto backing = std::make_shared<PathSpace>();
        auto first = std::make_unique<PathSpace>();
        REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
        auto second = std::make_unique<PathSpace>();
        REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);

        REQUIRE(backing->insert("/root/node", std::move(first)).nbrSpacesInserted == 1);
        REQUIRE(backing->insert("/root/node", std::move(second)).nbrSpacesInserted == 1);

        PathAlias alias{backing, "/root"};
        auto merged = alias.read<Children>(ConcretePathStringView{"/node"});
        REQUIRE(merged.has_value());
        CHECK(merged->names.size() == 2);
        CHECK(std::find(merged->names.begin(), merged->names.end(), "a") != merged->names.end());
        CHECK(std::find(merged->names.begin(), merged->names.end(), "b[1]") != merged->names.end());
    }
}

TEST_SUITE_END();
