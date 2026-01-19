#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>

using namespace SP;

TEST_SUITE_BEGIN("pathspace.valuehandle.guards");

TEST_CASE("default ValueHandle reports missing data gracefully") {
    ValueHandle handle{};

    CHECK_FALSE(handle.valid());
    CHECK_FALSE(handle.hasValues());
    CHECK(handle.queueDepth() == 0);

    auto snapshot = handle.snapshot();
    CHECK_FALSE(snapshot.has_value());
    CHECK(snapshot.error().code == Error::Code::UnknownError);

    auto readResult = handle.read<int>();
    CHECK_FALSE(readResult.has_value());
    CHECK(readResult.error().code == Error::Code::NotSupported);
}

TEST_SUITE_END();
