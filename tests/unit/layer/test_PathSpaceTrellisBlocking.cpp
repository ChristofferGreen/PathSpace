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

TEST_CASE("PathSpaceTrellis blocking read succeeds when data already available") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/fanout"}});

    auto insert = trellis.in(Iterator{"/"}, InputData{42});
    CHECK(insert.errors.empty());

    int                  resultValue = 0;
    auto readError = trellis.out(Iterator{"/"},
                                 InputMetadataT<int>{},
                                 Out{} & Block(std::chrono::milliseconds{100}),
                                 &resultValue);
    REQUIRE_FALSE(readError.has_value());
    CHECK(resultValue == 42);
}

TEST_CASE("PathSpaceTrellis rejects system path reads with InvalidPermissions") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/fanout"}});

    int out = 0;
    auto err = trellis.out(Iterator{"/_system/state"}, InputMetadataT<int>{}, Out{}, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis fan-out ignores disabled sources") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/first"}});
    trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/second"}});

    // Disable the first source.
    trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"/first"}});

    REQUIRE(backing->insert("/second", 7).errors.empty());

    int value = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &value);
    REQUIRE_FALSE(err.has_value());
    CHECK(value == 7);
}
}
