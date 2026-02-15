#define private public
#include "pathspace/PathSpaceBase.hpp"
#undef private

#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_SUITE("pathspace.valuehandle") {

TEST_CASE("ValueHandle readInto returns NotSupported when values are disabled") {
    PathSpace space;
    REQUIRE(space.insert("/value", 7).nbrValuesInserted == 1);

    VisitOptions opts;
    opts.includeValues = false;

    bool sawValue = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/value") {
                return VisitControl::Continue;
            }
            sawValue = true;
            int out = 0;
            InputMetadata meta{InputMetadataT<int>{}};
            auto err = handle.readInto(&out, meta);
            REQUIRE(err.has_value());
            CHECK(err->code == Error::Code::NotSupported);
            return VisitControl::Stop;
        },
        opts);
    REQUIRE(result);
    CHECK(sawValue);
}

} // TEST_SUITE
