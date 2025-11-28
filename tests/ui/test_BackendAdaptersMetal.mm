#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "third_party/doctest.h"

#include <pathspace/ui/PathRenderer2DMetal.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct ScopedEnv {
    std::string key;
    std::optional<std::string> previous;

    ScopedEnv(char const* name, char const* value) : key(name) {
        if (auto* existing = std::getenv(name)) {
            previous = std::string(existing);
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (previous.has_value()) {
            ::setenv(key.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(key.c_str());
        }
    }
};

} // namespace

TEST_CASE("PathSurfaceMetal integrates with ObjC++ presenter harness") {
    ScopedEnv enable_uploads{"PATHSPACE_ENABLE_METAL_UPLOADS", "1"};
    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        INFO("Failed to enable PATHSPACE_ENABLE_METAL_UPLOADS; skipping Metal harness verification");
        return;
    }

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            INFO("No Metal device available; skipping Metal harness verification");
            return;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            INFO("Failed to create Metal command queue; skipping Metal harness verification");
            return;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.contentsScale = 1.0;
        layer.drawableSize = CGSizeMake(8.0, 8.0);

        SP::UI::PathWindowView::MetalPresenterConfig config{
            .layer = (__bridge void*)layer,
            .device = (__bridge void*)device,
            .command_queue = (__bridge void*)queue,
            .contents_scale = 1.0,
        };
        SP::UI::PathWindowView::ConfigureMetalPresenter(config);

        SP::UI::Runtime::SurfaceDesc desc{};
        desc.size_px.width = 8;
        desc.size_px.height = 8;
        desc.pixel_format = SP::UI::Runtime::PixelFormat::BGRA8Unorm;
        desc.color_space = SP::UI::Runtime::ColorSpace::sRGB;
        desc.premultiplied_alpha = true;
        desc.metal.storage_mode = SP::UI::Runtime::MetalStorageMode::Shared;
        desc.metal.texture_usage = static_cast<std::uint8_t>(SP::UI::Runtime::MetalTextureUsage::ShaderRead)
                                   | static_cast<std::uint8_t>(SP::UI::Runtime::MetalTextureUsage::RenderTarget);
        desc.metal.iosurface_backing = true;

        SP::UI::PathSurfaceMetal metal{desc};
        auto texture_info = metal.acquire_texture();
        if (!texture_info.texture) {
            INFO("Failed to acquire Metal texture; skipping Metal harness verification");
            SP::UI::PathWindowView::ResetMetalPresenter();
            return;
        }

        SP::UI::PathRenderer2DMetal renderer;
        std::array<float, 4> clear = {0.0f, 0.0f, 0.0f, 0.0f};
        REQUIRE(renderer.begin_frame(metal, desc, clear));

        SP::UI::PathRenderer2DMetal::Rect rect{
            .min_x = 1.0f,
            .min_y = 1.0f,
            .max_x = 7.0f,
            .max_y = 7.0f,
        };
        REQUIRE(renderer.draw_rect(rect, {0.1f, 0.6f, 0.3f, 1.0f}));
        REQUIRE(renderer.finish(metal, 1, 42));

        SP::UI::PathSurfaceSoftware software{
            desc,
            SP::UI::PathSurfaceSoftware::Options{
                .enable_progressive = false,
                .enable_buffered = true,
                .progressive_tile_size_px = 32,
            },
        };
        auto staging = software.staging_span();
        std::fill(staging.begin(), staging.end(), 0x10u);
        software.publish_buffered_frame({
            .frame_index = 0,
            .revision = 0,
            .render_ms = 0.5,
        });

        std::vector<std::uint8_t> fallback(desc.size_px.width * desc.size_px.height * 4u, 0u);
        SP::UI::PathWindowView::PresentPolicy policy{};
        SP::UI::PathWindowView::PresentRequest request{};
        request.now = std::chrono::steady_clock::now();
        request.vsync_deadline = request.now + std::chrono::milliseconds{5};
        request.framebuffer = std::span<std::uint8_t>{fallback.data(), fallback.size()};
        request.surface_width_px = desc.size_px.width;
        request.surface_height_px = desc.size_px.height;
        request.has_metal_texture = texture_info.texture != nullptr;
        request.metal_surface = &metal;
        request.metal_texture = texture_info;
        request.allow_iosurface_sharing = true;

        SP::UI::PathWindowView view;
        auto stats = view.present(software, policy, request);

        CHECK(stats.presented);
        CHECK(stats.used_metal_texture);
        CHECK(stats.backend_kind == "Metal2D");
        CHECK(stats.gpu_present_ms >= 0.0);
        CHECK(stats.gpu_encode_ms >= 0.0);

        SP::UI::PathWindowView::ResetMetalPresenter();
    }
}

#else

#include "third_party/doctest.h"

TEST_CASE("PathSurfaceMetal integration harness pending") {
    CHECK(true);
}

#endif
