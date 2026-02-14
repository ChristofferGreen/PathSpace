#include "pathspace/PathSpaceBase.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("pathspace.visitoptions") {
TEST_CASE("VisitOptions helper methods reflect child limits") {
    VisitOptions opts;
    CHECK(opts.childLimitEnabled());
    CHECK_FALSE(VisitOptions::isUnlimitedChildren(opts.maxChildren));

    opts.maxChildren = VisitOptions::UnlimitedChildren;
    CHECK_FALSE(opts.childLimitEnabled());
    CHECK(VisitOptions::isUnlimitedChildren(opts.maxChildren));
}

TEST_CASE("PathSpaceJsonOptions defaults mirror expected visit settings") {
    PathSpaceJsonOptions options;
    CHECK(options.visit.root == "/");
    CHECK(options.visit.maxDepth == VisitOptions::UnlimitedDepth);
    CHECK(options.visit.maxChildren == VisitOptions::UnlimitedChildren);
    CHECK_FALSE(options.visit.includeNestedSpaces);
    CHECK(options.visit.includeValues);
    CHECK(options.mode == PathSpaceJsonOptions::Mode::Minimal);
}
}
