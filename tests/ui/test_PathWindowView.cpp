#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

using namespace SP::UI;
namespace Runtime = SP::UI::Runtime;
using SP::ConcretePathString;
using SP::ConcretePathStringView;
using SP::PathSpace;

namespace {

auto make_desc(int width, int height) -> Runtime::SurfaceDesc {
    Runtime::SurfaceDesc desc;
    desc.size_px.width = width;
    desc.size_px.height = height;
    desc.pixel_format = Runtime::PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space = Runtime::ColorSpace::sRGB;
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

#if defined(__APPLE__)
TEST_CASE("present shares iosurface when enabled") {
    PathSurfaceSoftware surface{make_desc(4, 4)};
    auto stage = surface.staging_span();
    REQUIRE(stage.size() >= 16);
    stage[0] = 0xAA;
    stage[1] = 0xBB;
    stage[2] = 0xCC;
    stage[3] = 0xDD;
    surface.publish_buffered_frame({
        .frame_index = 6,
        .revision = 11,
        .render_ms = 2.0,
    });

    PathWindowView view;
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
        .framebuffer = {},
        .dirty_tiles = {},
        .allow_iosurface_sharing = true,
    };

    auto stats = view.present(surface, {}, request);
    CHECK(stats.presented);
    CHECK_FALSE(stats.buffered_frame_consumed);
    CHECK(stats.used_iosurface);
    REQUIRE(stats.iosurface.has_value());
    auto iosurface = stats.iosurface->retain_for_external_use();
    REQUIRE(iosurface != nullptr);
    REQUIRE(IOSurfaceLock(iosurface, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess);
    auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(iosurface));
    REQUIRE(base != nullptr);
    CHECK(base[0] == 0xAA);
    CHECK(base[1] == 0xBB);
    CHECK(base[2] == 0xCC);
    CHECK(base[3] == 0xDD);
    IOSurfaceUnlock(iosurface, kIOSurfaceLockAvoidSync, nullptr);
    CFRelease(iosurface);
}
#endif

#if defined(__APPLE__)
TEST_CASE("fullscreen iosurface present protects zero-copy perf") {
    constexpr int width = 3840;
    constexpr int height = 2160;

    PathSurfaceSoftware surface{make_desc(width, height)};

    std::array<std::uint8_t, 16> iosurface_pattern{};
    for (std::size_t i = 0; i < iosurface_pattern.size(); ++i) {
        iosurface_pattern[i] = static_cast<std::uint8_t>(0xA0 + static_cast<int>(i));
    }
    {
        auto stage = surface.staging_span();
        REQUIRE(stage.size() >= iosurface_pattern.size());
        std::copy(iosurface_pattern.begin(), iosurface_pattern.end(), stage.begin());
        surface.publish_buffered_frame({
            .frame_index = 12,
            .revision = 5,
            .render_ms = 4.0,
        });
    }

    PathWindowView view;
    PathWindowView::PresentPolicy policy{};
    PathWindowView::PresentRequest iosurface_request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{8},
        .framebuffer = {},
        .dirty_tiles = {},
        .allow_iosurface_sharing = true,
    };

    auto iosurface_stats = view.present(surface, policy, iosurface_request);
    REQUIRE(iosurface_stats.presented);
    CHECK(iosurface_stats.used_iosurface);
    CHECK_FALSE(iosurface_stats.buffered_frame_consumed);
    REQUIRE(iosurface_stats.iosurface.has_value());

    auto iosurface = iosurface_stats.iosurface->retain_for_external_use();
    REQUIRE(iosurface != nullptr);
    REQUIRE(IOSurfaceLock(iosurface, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess);
    auto* iosurface_base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(iosurface));
    REQUIRE(iosurface_base != nullptr);
    for (std::size_t i = 0; i < iosurface_pattern.size(); ++i) {
        CHECK(iosurface_base[i] == iosurface_pattern[i]);
    }
    IOSurfaceUnlock(iosurface, kIOSurfaceLockAvoidSync, nullptr);
    CFRelease(iosurface);

    std::array<std::uint8_t, 16> copy_pattern{};
    for (std::size_t i = 0; i < copy_pattern.size(); ++i) {
        copy_pattern[i] = static_cast<std::uint8_t>(0x40 + static_cast<int>(i));
    }
    {
        auto stage = surface.staging_span();
        REQUIRE(stage.size() >= copy_pattern.size());
        std::copy(copy_pattern.begin(), copy_pattern.end(), stage.begin());
        surface.publish_buffered_frame({
            .frame_index = 13,
            .revision = 6,
            .render_ms = 4.0,
        });
    }

    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    PathWindowView::PresentRequest copy_request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{8},
        .framebuffer = framebuffer,
        .dirty_tiles = {},
        .allow_iosurface_sharing = false,
    };

    auto copy_stats = view.present(surface, policy, copy_request);
    REQUIRE(copy_stats.presented);
    CHECK(copy_stats.buffered_frame_consumed);
    CHECK_FALSE(copy_stats.used_iosurface);

    for (std::size_t i = 0; i < copy_pattern.size(); ++i) {
        CHECK(framebuffer[i] == copy_pattern[i]);
    }

    CHECK(copy_stats.present_ms >= iosurface_stats.present_ms);
    CHECK((copy_stats.present_ms - iosurface_stats.present_ms) >= 0.02);
}
#endif

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
    auto tile_dims = surface.progressive_buffer().tile_dimensions(0);
    auto affected_rows = tile_dims.height;
    auto affected_cols = tile_dims.width;
    for (int row = 0; row < affected_rows; ++row) {
        auto base = static_cast<std::size_t>(row + tile_dims.y) * row_stride;
        for (int col = 0; col < affected_cols; ++col) {
            auto idx = base + static_cast<std::size_t>(col + tile_dims.x) * 4u;
            CHECK(framebuffer[idx + 0] == 10);
            CHECK(framebuffer[idx + 1] == 20);
            CHECK(framebuffer[idx + 2] == 30);
            CHECK(framebuffer[idx + 3] == 255);
        }
    }
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

TEST_CASE("present clamps wait budget when deadline elapsed") {
    PathSurfaceSoftware surface{make_desc(4, 4)};
    auto staging = surface.staging_span();
    std::fill(staging.begin(), staging.end(), std::uint8_t{0xEE});
    surface.publish_buffered_frame({
        .frame_index = 3,
        .revision = 7,
        .render_ms = 1.25,
    });

    PathWindowView view;
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    auto now = std::chrono::steady_clock::now();
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now - std::chrono::milliseconds{2},
        .framebuffer = framebuffer,
        .dirty_tiles = {},
    };

    auto stats = view.present(surface, {}, request);
    CHECK(stats.presented);
    CHECK(stats.wait_budget_ms == doctest::Approx(0.0));
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
    stats.gpu_encode_ms = 1.5;
    stats.gpu_present_ms = 2.5;
    stats.used_metal_texture = true;
    stats.backend_kind = "Metal2D";
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
    CHECK(space.read<double, std::string>(base + "/gpuEncodeMs").value() == doctest::Approx(1.5));
    CHECK(space.read<double, std::string>(base + "/gpuPresentMs").value() == doctest::Approx(2.5));
    CHECK_FALSE(space.read<bool, std::string>(base + "/lastPresentSkipped").value());
    CHECK(space.read<bool, std::string>(base + "/presented").value());
    CHECK(space.read<bool, std::string>(base + "/bufferedFrameConsumed").value());
    CHECK(space.read<bool, std::string>(base + "/usedProgressive").value());
    CHECK(space.read<bool, std::string>(base + "/usedMetalTexture").value());
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
    CHECK(space.read<std::string, std::string>(base + "/backendKind").value() == "Metal2D");
    auto lastError = space.read<std::string, std::string>(base + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "ok");

    auto diag = Builders::Diagnostics::ReadTargetError(
        space,
        ConcretePathStringView{targetPath.getPath()});
    REQUIRE(diag);
    REQUIRE(diag->has_value());
    CHECK(diag->value().message == "ok");
    CHECK(diag->value().code == 3000);
}

TEST_CASE("WriteWindowPresentMetrics mirrors presenter stats to window diagnostics") {
    PathSpace space;

    PathWindowView::PresentStats stats{};
    stats.presented = true;
    stats.skipped = false;
    stats.buffered_frame_consumed = true;
    stats.used_progressive = true;
    stats.used_metal_texture = false;
    stats.wait_budget_ms = 4.5;
    stats.damage_ms = 1.0;
    stats.encode_ms = 2.25;
    stats.progressive_copy_ms = 0.75;
    stats.publish_ms = 0.5;
    stats.present_ms = 2.75;
    stats.gpu_encode_ms = 1.25;
    stats.gpu_present_ms = 1.5;
    stats.frame_age_ms = 3.0;
    stats.frame_age_frames = 2;
    stats.drawable_count = 42;
    stats.progressive_tiles_updated = 7;
    stats.progressive_bytes_copied = 2048;
    stats.progressive_tile_size = 64;
    stats.progressive_workers_used = 4;
    stats.progressive_jobs = 8;
    stats.encode_workers_used = 3;
    stats.encode_jobs = 6;
    stats.progressive_tiles_dirty = 5;
    stats.progressive_tiles_total = 12;
    stats.progressive_tiles_skipped = 2;
    stats.progressive_tile_diagnostics_enabled = true;
    stats.stale = false;
    stats.mode = PathWindowView::PresentMode::PreferLatestCompleteWithBudget;
    stats.progressive_tiles_copied = 5;
    stats.progressive_rects_coalesced = 4;
    stats.progressive_skip_seq_odd = 1;
    stats.progressive_recopy_after_seq_change = 0;
    stats.frame.frame_index = 11;
    stats.frame.revision = 8;
    stats.frame.render_ms = 6.0;
    stats.backend_kind = "Software2D";
    stats.error = "minor hiccup";
#if defined(__APPLE__)
    stats.used_iosurface = true;
#endif

    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowView::PresentMode::PreferLatestCompleteWithBudget;
    policy.staleness_budget = std::chrono::milliseconds{6};
    policy.staleness_budget_ms_value = 6.0;
    policy.frame_timeout = std::chrono::milliseconds{18};
    policy.frame_timeout_ms_value = 18.0;
    policy.max_age_frames = 4;
    policy.auto_render_on_present = true;
    policy.vsync_align = true;

    auto windowPath = ConcretePathString{"/windows/main"};
    auto writeStatus = Builders::Diagnostics::WriteWindowPresentMetrics(space,
                                                                        ConcretePathStringView{windowPath.getPath()},
                                                                        "view",
                                                                        stats,
                                                                        policy);
    REQUIRE(writeStatus);

    const std::string base = std::string(windowPath.getPath()) +
                             "/diagnostics/metrics/live/views/view/present";

    auto frameIndex = space.read<uint64_t>(base + "/frameIndex");
    REQUIRE(frameIndex);
    CHECK(*frameIndex == 11);
    auto revision = space.read<uint64_t>(base + "/revision");
    REQUIRE(revision);
    CHECK(*revision == 8);
    auto renderMs = space.read<double>(base + "/renderMs");
    REQUIRE(renderMs);
    CHECK(*renderMs == doctest::Approx(6.0));
    auto damageMs = space.read<double>(base + "/damageMs");
    REQUIRE(damageMs);
    CHECK(*damageMs == doctest::Approx(1.0));
    auto encodeMs = space.read<double>(base + "/encodeMs");
    REQUIRE(encodeMs);
    CHECK(*encodeMs == doctest::Approx(2.25));
    auto progressiveCopyMs = space.read<double>(base + "/progressiveCopyMs");
    REQUIRE(progressiveCopyMs);
    CHECK(*progressiveCopyMs == doctest::Approx(0.75));
    auto publishMs = space.read<double>(base + "/publishMs");
    REQUIRE(publishMs);
    CHECK(*publishMs == doctest::Approx(0.5));
    auto presentMs = space.read<double>(base + "/presentMs");
    REQUIRE(presentMs);
    CHECK(*presentMs == doctest::Approx(2.75));
    auto waitBudget = space.read<double>(base + "/waitBudgetMs");
    REQUIRE(waitBudget);
    CHECK(*waitBudget == doctest::Approx(4.5));
    auto backend = space.read<std::string, std::string>(base + "/backendKind");
    REQUIRE(backend);
    CHECK(*backend == "Software2D");
    auto progressiveTiles = space.read<uint64_t>(base + "/progressiveTilesCopied");
    REQUIRE(progressiveTiles);
    CHECK(*progressiveTiles == 5);
    auto progressiveUpdated = space.read<uint64_t>(base + "/progressiveTilesUpdated");
    REQUIRE(progressiveUpdated);
    CHECK(*progressiveUpdated == 7);
    auto progressiveBytes = space.read<uint64_t>(base + "/progressiveBytesCopied");
    REQUIRE(progressiveBytes);
    CHECK(*progressiveBytes == 2048);
    auto progressiveTileSize = space.read<uint64_t>(base + "/progressiveTileSize");
    REQUIRE(progressiveTileSize);
    CHECK(*progressiveTileSize == 64);
    auto progressiveWorkers = space.read<uint64_t>(base + "/progressiveWorkersUsed");
    REQUIRE(progressiveWorkers);
    CHECK(*progressiveWorkers == 4);
    auto progressiveJobs = space.read<uint64_t>(base + "/progressiveJobs");
    REQUIRE(progressiveJobs);
    CHECK(*progressiveJobs == 8);
    auto encodeWorkers = space.read<uint64_t>(base + "/encodeWorkersUsed");
    REQUIRE(encodeWorkers);
    CHECK(*encodeWorkers == 3);
    auto encodeJobs = space.read<uint64_t>(base + "/encodeJobs");
    REQUIRE(encodeJobs);
    CHECK(*encodeJobs == 6);
    auto progressiveDiagnosticsEnabled = space.read<bool>(base + "/progressiveTileDiagnosticsEnabled");
    REQUIRE(progressiveDiagnosticsEnabled);
    CHECK(*progressiveDiagnosticsEnabled);
    auto progressiveDirty = space.read<uint64_t>(base + "/progressiveTilesDirty");
    REQUIRE(progressiveDirty);
    CHECK(*progressiveDirty == 5);
    auto progressiveTotal = space.read<uint64_t>(base + "/progressiveTilesTotal");
    REQUIRE(progressiveTotal);
    CHECK(*progressiveTotal == 12);
    auto progressiveSkipped = space.read<uint64_t>(base + "/progressiveTilesSkipped");
    REQUIRE(progressiveSkipped);
    CHECK(*progressiveSkipped == 2);
    auto drawableCount = space.read<uint64_t>(base + "/drawableCount");
    REQUIRE(drawableCount);
    CHECK(*drawableCount == 42);
    auto lastError = space.read<std::string, std::string>(base + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "minor hiccup");
    auto viewName = space.read<std::string, std::string>(base + "/viewName");
    REQUIRE(viewName);
    CHECK(*viewName == "view");
    auto timestamp = space.read<std::uint64_t>(base + "/timestampNs");
    REQUIRE(timestamp);
    CHECK(*timestamp > 0);
#if defined(__APPLE__)
    auto usedIosurface = space.read<bool>(base + "/usedIOSurface");
    REQUIRE(usedIosurface);
    CHECK(*usedIosurface);
#endif
}

} // TEST_SUITE
