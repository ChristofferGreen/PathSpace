#pragma once

#include <cstdint>
#include <vector>

namespace SP::UI::Builders {

enum class PixelFormat {
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA8Unorm_sRGB,
    BGRA8Unorm_sRGB,
    RGBA16F,
    RGBA32F,
};

enum class ColorSpace {
    sRGB,
    DisplayP3,
    Linear,
};

struct SurfaceDesc {
    struct SizePx {
        int width = 0;
        int height = 0;
    } size_px;
    PixelFormat pixel_format = PixelFormat::RGBA8Unorm;
    ColorSpace color_space = ColorSpace::sRGB;
    bool premultiplied_alpha = true;
    int progressive_tile_size_px = 64;
};

struct SoftwareFramebuffer {
    int width = 0;
    int height = 0;
    std::uint32_t row_stride_bytes = 0;
    PixelFormat pixel_format = PixelFormat::RGBA8Unorm;
    ColorSpace color_space = ColorSpace::sRGB;
    bool premultiplied_alpha = true;
    std::vector<std::uint8_t> pixels;
};

} // namespace SP::UI::Builders
