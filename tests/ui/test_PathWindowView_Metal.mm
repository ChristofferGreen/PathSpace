#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ext/doctest.h"

#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>

#include <chrono>
#include <cstdlib>
#include <vector>

using namespace SP::UI;

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
        PathSurfaceSoftware software{desc};
        PathSurfaceMetal metal{desc};

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

        PathWindowView::ResetMetalPresenter();
    }
}

#endif // defined(__APPLE__) && PATHSPACE_UI_METAL
