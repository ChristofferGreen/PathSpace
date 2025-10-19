#include "third_party/doctest.h"
#include <string>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>

using namespace SP;

TEST_CASE("PathAlias - Forwarding insert and read to upstream") {
    // Upstream concrete space where data is actually stored
    auto upstream = std::make_shared<PathSpace>();

    // Alias forwards "/..." to "/dev/..." on the upstream space
    PathAlias alias{upstream, "/dev"};

    SUBCASE("Insert via alias maps to target prefix") {
        auto ret = alias.insert<"/mouse/0/name", std::string>(std::string("mouse0"));
        CHECK(ret.errors.empty());
        CHECK(ret.nbrValuesInserted == 1);

        auto u = upstream->read<"/dev/mouse/0/name", std::string>();
        REQUIRE(u.has_value());
        CHECK(u.value() == "mouse0");
    }

    SUBCASE("Read via alias maps to target prefix") {
        // Prepare data directly in upstream at the target path
        upstream->insert<"/dev/mouse/0/name", std::string>(std::string("M0"));

        auto r = alias.read<"/mouse/0/name", std::string>();
        REQUIRE(r.has_value());
        CHECK(r.value() == "M0");
    }
}

TEST_CASE("PathAlias - Atomic retargeting switches forwarding destination") {
    auto upstream = std::make_shared<PathSpace>();

    PathAlias alias{upstream, "/dev1"};

    SUBCASE("Retarget updates where subsequent operations go") {
        // Initial target: /dev1
        alias.insert<"/x", std::string>(std::string("one"));

        // Switch to /dev2
        alias.setTargetPrefix("/dev2");
        alias.insert<"/x", std::string>(std::string("two"));

        // Validate upstream paths
        auto r1 = upstream->read<"/dev1/x", std::string>();
        REQUIRE(r1.has_value());
        CHECK(r1.value() == "one");

        auto r2 = upstream->read<"/dev2/x", std::string>();
        REQUIRE(r2.has_value());
        CHECK(r2.value() == "two");

        // Reads via alias use the current target
        auto rAlias = alias.read<"/x", std::string>();
        REQUIRE(rAlias.has_value());
        CHECK(rAlias.value() == "two");
    }
}

TEST_CASE("PathAlias - Nested mounting under a parent PathSpace") {
    auto upstream = std::make_shared<PathSpace>();
    PathSpace parent;

    // Keep a raw pointer to manipulate the alias after mounting
    auto aliasUptr = std::make_unique<PathAlias>(upstream, "/root");
    auto* aliasRaw = aliasUptr.get();

    // Mount the alias at /alias in the parent space
    {
        auto ret = parent.insert<"/alias">(std::move(aliasUptr));
        CHECK(ret.errors.empty());
        CHECK(ret.nbrSpacesInserted == 1);
    }

    SUBCASE("Insert via parent forwards through alias") {
        // Insert through the parent under the alias mount
        auto ret = parent.insert<"/alias/a", std::string>(std::string("v"));
        CHECK(ret.errors.empty());
        CHECK(ret.nbrValuesInserted == 1);

        // Upstream should see it under the target prefix "/root"
        auto u = upstream->read<"/root/a", std::string>();
        REQUIRE(u.has_value());
        CHECK(u.value() == "v");

        // Reading through the parent alias also works
        auto r = parent.read<"/alias/a", std::string>();
        REQUIRE(r.has_value());
        CHECK(r.value() == "v");
    }

    SUBCASE("Retarget after mounting affects subsequent forwards") {
        // Write to initial target "/root"
        parent.insert<"/alias/k", std::string>(std::string("old"));

        // Retarget the alias to a new upstream prefix
        aliasRaw->setTargetPrefix("/other");

        // Write to the alias again; should land under "/other"
        parent.insert<"/alias/k", std::string>(std::string("new"));

        // Validate upstream at both locations
        auto oldV = upstream->read<"/root/k", std::string>();
        REQUIRE(oldV.has_value());
        CHECK(oldV.value() == "old");

        auto newV = upstream->read<"/other/k", std::string>();
        REQUIRE(newV.has_value());
        CHECK(newV.value() == "new");

        // Reading via parent alias returns the newest value under current target
        auto viaAlias = parent.read<"/alias/k", std::string>();
        REQUIRE(viaAlias.has_value());
        CHECK(viaAlias.value() == "new");
    }
}