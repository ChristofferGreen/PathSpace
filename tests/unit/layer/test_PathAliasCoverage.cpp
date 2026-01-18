#include "layer/PathAlias.hpp"
#include "PathSpace.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadataT.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("layer.pathalias.coverage") {
TEST_CASE("PathAlias rewrites inserts and reads via target prefix") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/upstream"};

    // Insert through alias, verify upstream value.
    auto ins = alias.in(Iterator{"/node"}, InputData{123});
    REQUIRE(ins.errors.empty());

    auto direct = upstream->read<int>("/upstream/node");
    CHECK(direct.has_value());
    CHECK(*direct == 123);

    // Read back through alias.
    auto viaAlias = alias.read<int>("/node");
    CHECK(viaAlias.has_value());
    CHECK(*viaAlias == 123);

    // Retarget and ensure new inserts go to the updated prefix.
    alias.setTargetPrefix("/newroot/");
    auto ins2 = alias.in(Iterator{"/second"}, InputData{321});
    REQUIRE(ins2.errors.empty());

    auto newVal = upstream->read<int>("/newroot/second");
    CHECK(newVal.has_value());
    CHECK(*newVal == 321);
}

TEST_CASE("PathAlias children listing and notify path mapping") {
    auto upstream = std::make_shared<PathSpace>();
    PathAlias alias{upstream, "/mount"};

    upstream->insert("/mount/a", 1);
    upstream->insert("/mount/b", 2);

    auto children = alias.read<Children>("/");
    REQUIRE(children.has_value());
    CHECK(children->names.size() == 2);

    // Exercise notify mapping; no observable state change needed for coverage.
    alias.notify("/");
    alias.notify("/_system");
}
}
