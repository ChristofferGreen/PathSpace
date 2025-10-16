#pragma once

#include <cstdint>

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
};

} // namespace SP::UI::Builders

