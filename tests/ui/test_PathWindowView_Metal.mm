#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

using namespace SP::UI;
namespace UIScene = SP::UI::Scene;
using SP::UI::Builders::Diagnostics::PathSpaceError;

namespace {

auto make_surface_desc() -> Builders::SurfaceDesc {
    Builders::SurfaceDesc desc{};
    desc.size_px.width = 4;
    desc.size_px.height = 4;
    desc.pixel_format = Builders::PixelFormat::BGRA8Unorm;
    desc.color_space = Builders::ColorSpace::sRGB;
    desc.premultiplied_alpha = true;
    return desc;
}

} // namespace

TEST_CASE("PathWindowView presents Metal texture when uploads enabled") {
    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        INFO("PATHSPACE_ENABLE_METAL_UPLOADS is not set; skipping Metal presenter verification");
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal presenter verification");
            return;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        REQUIRE(static_cast<bool>(queue));

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(4.0, 4.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = (__bridge void*)queue,
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        auto desc = make_surface_desc();
        desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                   | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);
        PathSurfaceSoftware software{desc};
        PathSurfaceMetal metal{desc};

        std::vector<MaterialDescriptor> materials;
        MaterialDescriptor descriptor{};
        descriptor.material_id = 5;
        descriptor.pipeline_flags = 0x10;
        descriptor.primary_draw_kind = static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect);
        descriptor.drawable_count = 2;
        descriptor.command_count = 3;
        descriptor.tint_rgba = {1.0f, 1.0f, 1.0f, 1.0f};
        descriptor.color_rgba = {0.2f, 0.4f, 0.6f, 0.8f};
        descriptor.uses_image = false;
        materials.push_back(descriptor);
        metal.update_material_descriptors(materials);

        std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0x7Fu);
        metal.update_from_rgba8(pixels, 4u * 4u, 5, 9);
        auto texture = metal.acquire_texture();

        PathWindowView::PresentRequest request{
            .now = std::chrono::steady_clock::now(),
            .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
            .framebuffer = {},
            .dirty_tiles = {},
            .surface_width_px = desc.size_px.width,
            .surface_height_px = desc.size_px.height,
            .has_metal_texture = texture.texture != nullptr,
            .metal_texture = texture,
            .allow_iosurface_sharing = false,
        };

        PathWindowView view;
        auto stats = view.present(software, {}, request);

        CHECK(stats.presented);
        CHECK(stats.used_metal_texture);
        CHECK(stats.gpu_encode_ms >= 0.0);
        CHECK(stats.gpu_present_ms >= 0.0);
        CHECK(stats.present_ms >= 0.0);
        CHECK_FALSE(stats.skipped);

        id<MTLTexture> tex = (__bridge id<MTLTexture>)texture.texture;
        bool hasTexture = (tex != nil);
        CHECK(hasTexture);
        CHECK(tex.storageMode == MTLStorageModeShared);
        CHECK((tex.usage & MTLTextureUsageShaderRead) != 0);
        CHECK((tex.usage & MTLTextureUsageRenderTarget) != 0);

        auto material_span = metal.material_descriptors();
        CHECK(material_span.size() == materials.size());
        CHECK(material_span.front().material_id == materials.front().material_id);

        PathWindowView::ResetMetalPresenter();
    }
}

TEST_CASE("PathWindowView surfaces Metal presenter errors and falls back") {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal presenter error verification");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(4.0, 4.0);

        PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = nullptr, // intentionally missing to trigger presenter error
            .contents_scale = 1.0,
        };
        PathWindowView::ConfigureMetalPresenter(config);

        auto desc = make_surface_desc();
        PathSurfaceSoftware software{desc};

        // Seed the buffered frame so the CPU fallback can succeed.
        auto staging = software.staging_span();
        std::fill(staging.begin(), staging.end(), 0x3Fu);
        software.publish_buffered_frame({
            .frame_index = 12,
            .revision = 34,
            .render_ms = 1.0,
        });

        PathSurfaceMetal metal{desc};
        auto texture = metal.acquire_texture();

        std::vector<std::uint8_t> framebuffer(software.frame_bytes(), 0);
        PathWindowView::PresentRequest request{
            .now = std::chrono::steady_clock::now(),
            .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
            .framebuffer = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()},
            .dirty_tiles = {},
            .surface_width_px = desc.size_px.width,
            .surface_height_px = desc.size_px.height,
            .has_metal_texture = texture.texture != nullptr,
            .metal_texture = texture,
            .allow_iosurface_sharing = false,
        };

        PathWindowView view;
        auto stats = view.present(software, {}, request);

        CHECK(stats.presented);
        CHECK(stats.buffered_frame_consumed);
        CHECK_FALSE(stats.used_metal_texture);
        CHECK(stats.error == "Metal command queue unavailable");
        CHECK(stats.gpu_encode_ms == doctest::Approx(0.0));
        CHECK(stats.gpu_present_ms == doctest::Approx(0.0));

        // Surface the error via diagnostics so GPU failures are visible to callers.
        SP::PathSpace space;
        SP::ConcretePathString targetPath{"/system/applications/test/renderers/2d/targets/surfaces/canvas"};
        auto targetView = SP::ConcretePathStringView{targetPath.getPath().c_str()};

        stats.backend_kind = "Metal2D";
        stats.present_ms = 1.0;
        stats.frame.render_ms = 0.5;

        PathWindowPresentPolicy policy{};
        policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
        policy.frame_timeout = std::chrono::milliseconds{5};
        policy.frame_timeout_ms_value = 5.0;
        policy.staleness_budget = std::chrono::milliseconds{2};
        policy.staleness_budget_ms_value = 2.0;

        auto writeMetrics = Builders::Diagnostics::WritePresentMetrics(space,
                                                                       targetView,
                                                                       stats,
                                                                       policy);
        REQUIRE(writeMetrics);

        auto metrics = Builders::Diagnostics::ReadTargetMetrics(space, targetView);
        REQUIRE(metrics);
        CHECK(metrics->backend_kind == "Metal2D");
        CHECK_FALSE(metrics->used_metal_texture);
        CHECK(metrics->last_error == "Metal command queue unavailable");
        CHECK(metrics->last_error_code == 3000);
        CHECK(metrics->last_error_severity == PathSpaceError::Severity::Recoverable);
        CHECK(metrics->last_error_timestamp_ns > 0);

        PathWindowView::ResetMetalPresenter();
    }
}

#endif // defined(__APPLE__) && PATHSPACE_UI_METAL
