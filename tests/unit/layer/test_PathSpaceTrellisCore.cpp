#include "layer/PathSpaceTrellis.hpp"
#include "PathSpace.hpp"
#include "path/Iterator.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"
#include "type/InputData.hpp"
#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("layer.pathspace.trellis.core") {
TEST_CASE("PathSpaceTrellis handles missing backing") {
    PathSpaceTrellis trellis{nullptr};

    auto insert = trellis.in(Iterator{"/value"}, InputData{42});
    REQUIRE_FALSE(insert.errors.empty());
    CHECK(insert.errors[0].code == Error::Code::InvalidPermissions);

    int outValue = 0;
    auto err = trellis.out(Iterator{"/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathSpaceTrellis enable/disable and fan-out read") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Enable a source
    auto enable = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/foo"}});
    REQUIRE(enable.errors.empty());

    // Insert a value at the enabled source
    auto ins = backing->insert("/foo", 123);
    REQUIRE(ins.errors.empty());

    int outValue = 0;
    auto err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK_FALSE(err.has_value());
    CHECK(outValue == 123);

    // Disable the source, subsequent read should fail with NoObjectFound
    auto disable = trellis.in(Iterator{"/_system/disable"}, InputData{std::string{"/foo"}});
    REQUIRE(disable.errors.empty());

    err = trellis.out(Iterator{"/"}, InputMetadataT<int>{}, Out{}, &outValue);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("PathSpaceTrellis system command validation") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Missing command segment
    auto insert = trellis.in(Iterator{"/_system"}, InputData{std::string{"x"}});
    REQUIRE_FALSE(insert.errors.empty());
    CHECK(insert.errors[0].code == Error::Code::InvalidPath);

    // Unknown command
    auto unknown = trellis.in(Iterator{"/_system/reload"}, InputData{std::string{"/foo"}});
    REQUIRE_FALSE(unknown.errors.empty());
    CHECK(unknown.errors[0].code == Error::Code::InvalidPath);
}

TEST_CASE("PathSpaceTrellis notify and join/strip helpers") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis{backing};

    // Register one source and verify notify fan-out ignores system paths.
    auto enable = trellis.in(Iterator{"/_system/enable"}, InputData{std::string{"/mount/a"}}); // already canonical
    REQUIRE(enable.errors.empty());

    trellis.notify("/");
    trellis.notify("/_system"); // should be ignored silently
}
}
