#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace SP::UI;
using SP::ConcretePathString;
using SP::ConcretePathStringView;
using SP::PathSpace;

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

TEST_SUITE("PathWindowView") {

TEST_CASE("present copies buffered frame") {
    PathSurfaceSoftware surface{make_desc(4, 4)};
    auto stage = surface.staging_span();
    REQUIRE(stage.size() == surface.frame_bytes());
    for (std::size_t i = 0; i < stage.size(); ++i) {
        stage[i] = static_cast<std::uint8_t>(i & 0xFF);
    }
    surface.publish_buffered_frame({
        .frame_index = 5,
        .revision = 9,
        .render_ms = 4.5,
    });

    PathWindowView view;
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{8},
        .framebuffer = framebuffer,
        .dirty_tiles = {},
    };

    auto stats = view.present(surface, {}, request);
    CHECK(stats.presented);
    CHECK(stats.buffered_frame_consumed);
    CHECK_FALSE(stats.used_progressive);
    CHECK(stats.progressive_rects_coalesced == 0);
    CHECK(stats.progressive_skip_seq_odd == 0);
    CHECK(stats.progressive_recopy_after_seq_change == 0);
    CHECK(stats.frame.frame_index == 5);
    CHECK(stats.error.empty());
    CHECK(stats.present_ms >= 0.0);
    CHECK(framebuffer == std::vector<std::uint8_t>(stage.begin(), stage.end()));
}

TEST_CASE("present copies progressive tiles when buffered missing") {
    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{make_desc(4, 4), opts};

    auto writer = surface.begin_progressive_tile(0, TilePass::AlphaInProgress);
    auto tile_pixels = writer.pixels();
    REQUIRE(tile_pixels.data != nullptr);
    // Fill top-left tile with pattern.
    for (int row = 0; row < tile_pixels.dims.height; ++row) {
        for (int col = 0; col < tile_pixels.dims.width; ++col) {
            auto idx = static_cast<std::size_t>(row * tile_pixels.stride_bytes + col * 4);
            tile_pixels.data[idx + 0] = 10;
            tile_pixels.data[idx + 1] = 20;
            tile_pixels.data[idx + 2] = 30;
            tile_pixels.data[idx + 3] = 255;
        }
    }
    writer.commit(TilePass::AlphaDone, 3);

    PathWindowView view;
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    std::array<std::size_t, 1> dirty_tiles{{0}};
    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{4},
        .framebuffer = framebuffer,
        .dirty_tiles = dirty_tiles,
    };

    auto stats = view.present(surface, policy, request);
    CHECK(stats.presented);
    CHECK_FALSE(stats.buffered_frame_consumed);
    CHECK(stats.used_progressive);
    CHECK(stats.progressive_tiles_copied == 1);
    CHECK(stats.progressive_rects_coalesced == 1);
    CHECK(stats.progressive_skip_seq_odd == 0);
    CHECK(stats.progressive_recopy_after_seq_change == 0);
    CHECK(stats.frame.revision == 3);
    CHECK(stats.error.empty());
    CHECK(stats.present_ms >= 0.0);

    auto row_stride = surface.row_stride_bytes();
    for (int row = 0; row < 2; ++row) {
        auto base = static_cast<std::size_t>(row) * row_stride;
        CHECK(framebuffer[base + 0] == 10);
        CHECK(framebuffer[base + 1] == 20);
        CHECK(framebuffer[base + 2] == 30);
        CHECK(framebuffer[base + 3] == 255);
        CHECK(framebuffer[base + 4] == 10);
        CHECK(framebuffer[base + 5] == 20);
        CHECK(framebuffer[base + 6] == 30);
        CHECK(framebuffer[base + 7] == 255);
    }
    // Remaining rows untouched (still zero).
    auto base = static_cast<std::size_t>(2) * surface.row_stride_bytes();
    CHECK(framebuffer[base + 0] == 0);
}

TEST_CASE("progressive copy records skip when tile write in-flight") {
    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{make_desc(2, 2), opts};
    auto writer = surface.begin_progressive_tile(0, TilePass::OpaqueInProgress);

    PathWindowView view;
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    std::array<std::size_t, 1> dirty_tiles{{0}};
    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{8},
        .framebuffer = framebuffer,
        .dirty_tiles = dirty_tiles,
    };

    auto stats = view.present(surface, policy, request);
    CHECK(stats.skipped);
    CHECK_FALSE(stats.presented);
    CHECK_FALSE(stats.used_progressive);
    CHECK(stats.progressive_tiles_copied == 0);
    CHECK(stats.progressive_rects_coalesced == 1);
    CHECK(stats.progressive_skip_seq_odd == 1);
    CHECK(stats.progressive_recopy_after_seq_change == 0);
}

TEST_CASE("always fresh skips when buffered frame missing") {
    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{make_desc(2, 2), opts};
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    PathWindowView view;
    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowView::PresentMode::AlwaysFresh;
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now(),
        .framebuffer = framebuffer,
        .dirty_tiles = {},
    };
    auto stats = view.present(surface, policy, request);
    CHECK(stats.skipped);
    CHECK_FALSE(stats.presented);
    CHECK_FALSE(stats.used_progressive);
    CHECK(stats.progressive_rects_coalesced == 0);
    CHECK(stats.progressive_skip_seq_odd == 0);
    CHECK(stats.progressive_recopy_after_seq_change == 0);
    CHECK(stats.present_ms >= 0.0);
}

TEST_CASE("WritePresentMetrics stores presenter results in PathSpace") {
    PathSpace space;
    PathWindowView::PresentStats stats{};
    stats.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    stats.presented = true;
    stats.skipped = false;
    stats.frame.frame_index = 42;
    stats.frame.revision = 77;
    stats.frame.render_ms = 5.5;
    stats.present_ms = 2.0;
    stats.buffered_frame_consumed = true;
    stats.used_progressive = true;
    stats.frame_age_frames = 2;
    stats.frame_age_ms = 66.0;
    stats.stale = true;
    stats.progressive_tiles_copied = 3;
    stats.progressive_rects_coalesced = 2;
    stats.progressive_skip_seq_odd = 1;
    stats.progressive_recopy_after_seq_change = 1;
    stats.wait_budget_ms = 1.25;
    stats.auto_render_on_present = false;
    stats.vsync_aligned = false;
    stats.error = "ok";

    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    policy.staleness_budget = std::chrono::milliseconds{9};
    policy.frame_timeout = std::chrono::milliseconds{33};
    policy.max_age_frames = 5;
    policy.auto_render_on_present = false;
    policy.vsync_align = false;
    policy.staleness_budget_ms_value = 9.0;
    policy.frame_timeout_ms_value = 33.0;

    auto targetPath = ConcretePathString{"/renderers/r/targets/surfaces/main"};
    auto writeStatus = Builders::Diagnostics::WritePresentMetrics(
        space,
        ConcretePathStringView{targetPath.getPath()},
        stats,
        policy);
    REQUIRE(writeStatus);

    auto base = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(space.read<uint64_t, std::string>(base + "/frameIndex").value() == 42);
    CHECK(space.read<uint64_t, std::string>(base + "/revision").value() == 77);
    CHECK(space.read<double, std::string>(base + "/renderMs").value() == doctest::Approx(5.5));
    CHECK(space.read<double, std::string>(base + "/presentMs").value() == doctest::Approx(2.0));
    CHECK_FALSE(space.read<bool, std::string>(base + "/lastPresentSkipped").value());
    CHECK(space.read<bool, std::string>(base + "/presented").value());
    CHECK(space.read<bool, std::string>(base + "/bufferedFrameConsumed").value());
    CHECK(space.read<bool, std::string>(base + "/usedProgressive").value());
    CHECK(space.read<uint64_t, std::string>(base + "/progressiveTilesCopied").value() == 3);
    CHECK(space.read<uint64_t, std::string>(base + "/progressiveRectsCoalesced").value() == 2);
    CHECK(space.read<uint64_t, std::string>(base + "/progressiveSkipOddSeq").value() == 1);
    CHECK(space.read<uint64_t, std::string>(base + "/progressiveRecopyAfterSeqChange").value() == 1);
    CHECK(space.read<double, std::string>(base + "/waitBudgetMs").value() == doctest::Approx(1.25));
    CHECK(space.read<double, std::string>(base + "/presentedAgeMs").value() == doctest::Approx(66.0));
    CHECK(space.read<uint64_t, std::string>(base + "/presentedAgeFrames").value() == 2);
    CHECK(space.read<bool, std::string>(base + "/stale").value());
    CHECK(space.read<std::string, std::string>(base + "/presentMode").value() == "AlwaysLatestComplete");
    CHECK(space.read<double, std::string>(base + "/stalenessBudgetMs").value() == doctest::Approx(9.0));
    CHECK(space.read<double, std::string>(base + "/frameTimeoutMs").value() == doctest::Approx(33.0));
    CHECK(space.read<uint64_t, std::string>(base + "/maxAgeFrames").value() == 5);
    CHECK_FALSE(space.read<bool, std::string>(base + "/autoRenderOnPresent").value());
    CHECK_FALSE(space.read<bool, std::string>(base + "/vsyncAlign").value());
    auto lastError = space.read<std::string, std::string>(base + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "ok");
}

} // TEST_SUITE
