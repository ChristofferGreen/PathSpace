#if defined(__APPLE__) && PATHSPACE_UI_METAL

#import <Metal/Metal.h>

#include "third_party/doctest.h"

#include <array>
#include <vector>

#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PathRenderer2DMetal.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

using namespace SP::UI;
namespace Runtime = SP::UI::Runtime;

TEST_CASE("PathRenderer2DMetal encodes rounded, text, and image commands") {
    Runtime::SurfaceDesc desc{};
    desc.size_px.width = 8;
    desc.size_px.height = 8;
    desc.pixel_format = Runtime::PixelFormat::BGRA8Unorm;
    desc.color_space = Runtime::ColorSpace::sRGB;
    desc.premultiplied_alpha = true;
    desc.metal.texture_usage = static_cast<std::uint8_t>(Runtime::MetalTextureUsage::RenderTarget)
                               | static_cast<std::uint8_t>(Runtime::MetalTextureUsage::ShaderRead);
    desc.metal.storage_mode = Runtime::MetalStorageMode::Shared;
    desc.metal.iosurface_backing = true;

    PathSurfaceMetal surface(desc);
    if (surface.device() == nullptr) {
        INFO("Metal device unavailable; skipping PathRenderer2DMetal command coverage");
        return;
    }

    PathRenderer2DMetal renderer;
    std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    REQUIRE(renderer.begin_frame(surface, desc, clear));

    PathRenderer2DMetal::Rect rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
    };
    CHECK(renderer.draw_rect(rect, {0.2f, 0.3f, 0.4f, 1.0f}));

    Scene::RoundedRectCommand rounded{};
    rounded.min_x = 1.0f;
    rounded.min_y = 1.0f;
    rounded.max_x = 7.0f;
    rounded.max_y = 7.0f;
    rounded.radius_top_left = 1.0f;
    rounded.radius_top_right = 1.5f;
    rounded.radius_bottom_right = 2.0f;
    rounded.radius_bottom_left = 0.5f;
    rounded.color = {0.6f, 0.4f, 0.2f, 0.9f};
    CHECK(renderer.draw_rounded_rect(rounded, {0.6f * 0.9f, 0.4f * 0.9f, 0.2f * 0.9f, 0.9f}));

    Scene::TextGlyphsCommand glyphs{};
    glyphs.min_x = 2.0f;
    glyphs.min_y = 2.0f;
    glyphs.max_x = 5.0f;
    glyphs.max_y = 3.5f;
    glyphs.color = {0.1f, 0.8f, 0.2f, 0.7f};
    CHECK(renderer.draw_text_quad(glyphs, {0.1f * 0.7f, 0.8f * 0.7f, 0.2f * 0.7f, 0.7f}));

    Scene::ImageCommand image{};
    image.min_x = 0.0f;
    image.min_y = 4.0f;
    image.max_x = 4.0f;
    image.max_y = 8.0f;
    image.uv_min_x = 0.0f;
    image.uv_min_y = 0.0f;
    image.uv_max_x = 1.0f;
    image.uv_max_y = 1.0f;
    image.image_fingerprint = 0x1234'5678u;
    image.tint = {1.0f, 0.9f, 0.8f, 0.5f};

    constexpr std::uint32_t kImageWidth = 4;
    constexpr std::uint32_t kImageHeight = 4;
    std::vector<float> pixels(static_cast<std::size_t>(kImageWidth) * static_cast<std::size_t>(kImageHeight) * 4u, 0.0f);
    for (std::uint32_t y = 0; y < kImageHeight; ++y) {
        for (std::uint32_t x = 0; x < kImageWidth; ++x) {
            auto idx = (static_cast<std::size_t>(y) * kImageWidth + x) * 4u;
            pixels[idx + 0] = static_cast<float>(x) / static_cast<float>(kImageWidth);
            pixels[idx + 1] = static_cast<float>(y) / static_cast<float>(kImageHeight);
            pixels[idx + 2] = 0.5f;
            pixels[idx + 3] = 1.0f;
        }
    }
    CHECK(renderer.draw_image(image,
                              kImageWidth,
                              kImageHeight,
                              pixels.data(),
                              pixels.size(),
                              {1.0f, 0.9f, 0.8f, 0.5f}));

    CHECK(renderer.finish(surface, 1, 1));
}

#endif
