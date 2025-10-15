#include "ext/doctest.h"

#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

using namespace SP::UI;

namespace {

auto make_destination(TileDimensions const& dims) -> std::vector<std::uint8_t> {
    auto const bytes_per_tile = static_cast<std::size_t>(dims.width) * dims.height * 4u;
    return std::vector<std::uint8_t>(bytes_per_tile, 0);
}

} // namespace

TEST_SUITE("ProgressiveSurfaceBuffer") {

TEST_CASE("Opaque commit produces readable tile") {
    ProgressiveSurfaceBuffer buffer{64, 64, 32};
    REQUIRE(buffer.tile_count() == 4);

    auto dims = buffer.tile_dimensions(0);
    REQUIRE(dims.width == 32);
    REQUIRE(dims.height == 32);

    auto writer = buffer.begin_tile_write(0, TilePass::OpaqueInProgress);
    auto tile_pixels = writer.pixels();
    REQUIRE(tile_pixels.data != nullptr);
    REQUIRE(tile_pixels.dims.width == dims.width);

    // Write a sentinel color into the top-left pixel.
    tile_pixels.data[0] = 0x12;
    tile_pixels.data[1] = 0x34;
    tile_pixels.data[2] = 0x56;
    tile_pixels.data[3] = 0x78;

    writer.commit(TilePass::OpaqueDone, /*epoch*/ 0);

    auto destination = make_destination(dims);
    auto copy_result = buffer.copy_tile(0, destination);
    REQUIRE(copy_result.has_value());
    CHECK(copy_result->pass == TilePass::OpaqueDone);
    CHECK(copy_result->epoch == 0);

    CHECK(destination[0] == 0x12);
    CHECK(destination[1] == 0x34);
    CHECK(destination[2] == 0x56);
    CHECK(destination[3] == 0x78);
}

TEST_CASE("Alpha commit stores epoch and overwrites pixels") {
    ProgressiveSurfaceBuffer buffer{64, 64, 32};

    {
        auto writer = buffer.begin_tile_write(0, TilePass::OpaqueInProgress);
        auto tile_pixels = writer.pixels();
        std::fill_n(tile_pixels.data, static_cast<std::size_t>(tile_pixels.dims.width) * tile_pixels.dims.height * 4u, 0);
        writer.commit(TilePass::OpaqueDone, 0);
    }

    auto writer = buffer.begin_tile_write(0, TilePass::AlphaInProgress);
    auto tile_pixels = writer.pixels();
    // Update the sentinel pixel to a new value.
    tile_pixels.data[0] = 0xAA;
    tile_pixels.data[1] = 0xBB;
    tile_pixels.data[2] = 0xCC;
    tile_pixels.data[3] = 0xDD;
    writer.commit(TilePass::AlphaDone, 7);

    auto dims = buffer.tile_dimensions(0);
    auto destination = make_destination(dims);
    auto copy_result = buffer.copy_tile(0, destination);
    REQUIRE(copy_result.has_value());
    CHECK(copy_result->pass == TilePass::AlphaDone);
    CHECK(copy_result->epoch == 7);
    CHECK(destination[0] == 0xAA);
    CHECK(destination[1] == 0xBB);
    CHECK(destination[2] == 0xCC);
    CHECK(destination[3] == 0xDD);
}

TEST_CASE("Copy skips tiles with odd sequence and abort clears pass") {
    ProgressiveSurfaceBuffer buffer{64, 64, 32};
    auto dims = buffer.tile_dimensions(0);

    {
        auto writer = buffer.begin_tile_write(0, TilePass::OpaqueInProgress);
        auto destination = make_destination(dims);
        auto copy_result = buffer.copy_tile(0, destination);
        CHECK_FALSE(copy_result.has_value()); // seq is odd while writer active
        // Writer goes out of scope without commit -> abort.
    }

    auto destination = make_destination(dims);
    auto copy_result = buffer.copy_tile(0, destination);
    REQUIRE(copy_result.has_value());
    CHECK(copy_result->pass == TilePass::None);
    CHECK(copy_result->epoch == 0);
}

TEST_CASE("Copy requires destination capacity") {
    ProgressiveSurfaceBuffer buffer{64, 64, 32};
    auto writer = buffer.begin_tile_write(0, TilePass::OpaqueInProgress);
    writer.commit(TilePass::OpaqueDone, 0);

    auto dims = buffer.tile_dimensions(0);
    auto too_small = std::vector<std::uint8_t>(static_cast<std::size_t>(dims.width) * 4u - 1, 0);
    auto copy_small = buffer.copy_tile(0, std::span<std::uint8_t>(too_small));
    CHECK_FALSE(copy_small.has_value());
}

} // TEST_SUITE
