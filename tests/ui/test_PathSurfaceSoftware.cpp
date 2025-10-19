#include "third_party/doctest.h"

#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <vector>

using namespace SP::UI;

namespace {

auto make_desc(int width, int height) -> Builders::SurfaceDesc {
    Builders::SurfaceDesc desc;
    desc.size_px.width = width;
    desc.size_px.height = height;
    desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space = Builders::ColorSpace::sRGB;
    desc.premultiplied_alpha = true;
    return desc;
}

} // namespace

TEST_SUITE("PathSurfaceSoftware") {

TEST_CASE("Buffered frame publication and copy") {
    PathSurfaceSoftware surface{make_desc(32, 16), {}};
    REQUIRE(surface.has_buffered());
    REQUIRE(surface.frame_bytes() == 32u * 16u * 4u);

    auto staging = surface.staging_span();
    REQUIRE(staging.size() == surface.frame_bytes());
    staging[0] = 0x11;
    staging[1] = 0x22;
    staging[2] = 0x33;
    staging[3] = 0x44;

    surface.publish_buffered_frame(PathSurfaceSoftware::FrameInfo{
        .frame_index = 7,
        .revision = 42,
        .render_ms = 3.5,
    });

    std::vector<std::uint8_t> copy(surface.frame_bytes());
    auto result = surface.copy_buffered_frame(copy);
    REQUIRE(result.has_value());
    CHECK(copy[0] == 0x11);
    CHECK(copy[1] == 0x22);
    CHECK(copy[2] == 0x33);
    CHECK(copy[3] == 0x44);
    CHECK(result->info.frame_index == 7);
    CHECK(result->info.revision == 42);
    CHECK(result->info.render_ms == doctest::Approx(3.5));
}

TEST_CASE("Progressive buffer exposes tiles") {
    PathSurfaceSoftware surface{make_desc(64, 64), {}};
    REQUIRE(surface.has_progressive());

    auto& progressive = surface.progressive_buffer();
    REQUIRE(progressive.tile_count() > 0);

    {
        auto writer = surface.begin_progressive_tile(0, TilePass::OpaqueInProgress);
        auto tile = writer.pixels();
        REQUIRE(tile.data != nullptr);
        REQUIRE(tile.dims.width > 0);
        writer.commit(TilePass::OpaqueDone, 1);
    }

    std::vector<std::uint8_t> tile_copy(progressive.tile_dimensions(0).width
                                        * progressive.tile_dimensions(0).height * 4u);
    auto result = progressive.copy_tile(0, tile_copy);
    REQUIRE(result.has_value());
    CHECK(result->pass == TilePass::OpaqueDone);
    CHECK(result->epoch == 0); // not set without AlphaDone
}

TEST_CASE("Resize resets buffers") {
    PathSurfaceSoftware surface{make_desc(16, 16), {}};
    surface.resize(make_desc(8, 8));
    CHECK(surface.frame_bytes() == 8u * 8u * 4u);
    if (surface.has_progressive()) {
        CHECK(surface.progressive_buffer().tile_count() > 0);
    }
}

} // TEST_SUITE
