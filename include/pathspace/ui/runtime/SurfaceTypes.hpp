#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SP::UI::Runtime {

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

enum class MetalStorageMode {
    Private,
    Shared,
    Managed,
    Memoryless,
};

enum class MetalTextureUsage : std::uint8_t {
    ShaderRead   = 1u << 0,
    ShaderWrite  = 1u << 1,
    RenderTarget = 1u << 2,
    Blit         = 1u << 3,
};

constexpr inline auto operator|(MetalTextureUsage lhs, MetalTextureUsage rhs) -> MetalTextureUsage {
    return static_cast<MetalTextureUsage>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr inline auto operator&(MetalTextureUsage lhs, MetalTextureUsage rhs) -> std::uint8_t {
    return static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs);
}

constexpr inline auto metal_usage_contains(std::uint8_t usage, MetalTextureUsage flag) -> bool {
    return (usage & static_cast<std::uint8_t>(flag)) != 0;
}

struct MetalSurfaceOptions {
    MetalStorageMode storage_mode = MetalStorageMode::Private;
    std::uint8_t texture_usage = static_cast<std::uint8_t>(MetalTextureUsage::ShaderRead)
                                 | static_cast<std::uint8_t>(MetalTextureUsage::ShaderWrite)
                                 | static_cast<std::uint8_t>(MetalTextureUsage::RenderTarget);
    bool iosurface_backing = true;
};

struct HtmlTargetDesc {
    std::size_t max_dom_nodes = 10'000;
    bool prefer_dom = true;
    bool allow_canvas_fallback = true;
};

struct SurfaceDesc {
    struct SizePx {
        int width = 0;
        int height = 0;
    } size_px;
    PixelFormat pixel_format = PixelFormat::BGRA8Unorm; // default to BGRA to match window presenters
    ColorSpace color_space = ColorSpace::sRGB;
    bool premultiplied_alpha = true;
    int progressive_tile_size_px = 64;
    MetalSurfaceOptions metal{};
};

struct SoftwareFramebuffer {
    int width = 0;
    int height = 0;
    std::uint32_t row_stride_bytes = 0;
    PixelFormat pixel_format = PixelFormat::BGRA8Unorm;
    ColorSpace color_space = ColorSpace::sRGB;
    bool premultiplied_alpha = true;
    std::vector<std::uint8_t> pixels;
};

} // namespace SP::UI::Runtime
