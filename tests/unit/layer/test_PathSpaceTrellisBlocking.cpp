#include "layer/PathSpaceTrellis.hpp"
#include "PathSpace.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadataT.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"

#include <memory>
#include <chrono>
#include <string>

using namespace SP;

TEST_SUITE("layer.pathspace.trellis.blocking") {
TEST_CASE("PathSpaceTrellis move-only insert routes single target") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/a"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/b"}});

    auto nested = std::make_unique<PathSpace>();
    auto insert = trellis.in(Iterator{"/"}, InputData{nested});
    CHECK(insert.errors.empty());
}

TEST_CASE("PathSpaceTrellis blocking fan-out times out when empty") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/only"}});

    int out = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{} & Block(std::chrono::milliseconds{5}), &out);
    REQUIRE(err.has_value());
}
}
