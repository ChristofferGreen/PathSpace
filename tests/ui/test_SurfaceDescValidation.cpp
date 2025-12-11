#include "third_party/doctest.h"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/RuntimeDetail.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Runtime;

TEST_SUITE("SurfaceDesc color management policy") {

TEST_CASE("rejects FP pixel formats for MVP") {
    SurfaceDesc desc{};
    desc.pixel_format = PixelFormat::RGBA16F;
    desc.color_space = ColorSpace::Linear;

    auto status = Detail::validate_color_management_scope(desc);

    CHECK_FALSE(status.has_value());
    CHECK(status.error().code == Error::Code::InvalidType);
}

TEST_CASE("rejects DisplayP3 targets for MVP") {
    SurfaceDesc desc{};
    desc.pixel_format = PixelFormat::RGBA8Unorm;
    desc.color_space = ColorSpace::DisplayP3;

    auto status = Detail::validate_color_management_scope(desc);

    CHECK_FALSE(status.has_value());
    CHECK(status.error().code == Error::Code::InvalidType);
}

TEST_CASE("requires sRGB color space for sRGB formats") {
    SurfaceDesc desc{};
    desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space = ColorSpace::Linear;

    auto status = Detail::validate_color_management_scope(desc);

    CHECK_FALSE(status.has_value());
    CHECK(status.error().code == Error::Code::InvalidType);
}

TEST_CASE("accepts linear unorm 8-bit targets") {
    SurfaceDesc desc{};
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.color_space = ColorSpace::Linear;

    auto status = Detail::validate_color_management_scope(desc);

    CHECK(status.has_value());
}

TEST_CASE("accepts sRGB 8-bit targets") {
    SurfaceDesc desc{};
    desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space = ColorSpace::sRGB;

    auto status = Detail::validate_color_management_scope(desc);

    CHECK(status.has_value());
}

} // TEST_SUITE
