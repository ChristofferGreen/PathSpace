#include "PathRenderer2DDetail.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace SP::UI::PathRenderer2DDetail {
namespace {

using Runtime::ColorSpace;
using Runtime::PixelFormat;
using Runtime::SurfaceDesc;

auto srgb_to_linear(float value) -> float {
    value = clamp_unit(value);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto linear_to_srgb(float value) -> float {
    value = std::max(0.0f, value);
    value = std::min(1.0f, value);
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

} // namespace

auto make_linear_straight(std::array<float, 4> const& rgba) -> LinearStraightColor {
    auto alpha = clamp_unit(rgba[3]);
    auto r = srgb_to_linear(rgba[0]);
    auto g = srgb_to_linear(rgba[1]);
    auto b = srgb_to_linear(rgba[2]);
    return LinearStraightColor{
        .r = clamp_unit(r),
        .g = clamp_unit(g),
        .b = clamp_unit(b),
        .a = alpha,
    };
}

auto premultiply(LinearStraightColor const& straight) -> LinearPremulColor {
    auto alpha = clamp_unit(straight.a);
    return LinearPremulColor{
        .r = clamp_unit(straight.r) * alpha,
        .g = clamp_unit(straight.g) * alpha,
        .b = clamp_unit(straight.b) * alpha,
        .a = alpha,
    };
}

auto make_linear_color(std::array<float, 4> const& rgba) -> LinearPremulColor {
    return premultiply(make_linear_straight(rgba));
}

auto to_array(LinearPremulColor const& color) -> std::array<float, 4> {
    return {color.r, color.g, color.b, color.a};
}

auto to_array(LinearStraightColor const& color) -> std::array<float, 4> {
    return {color.r, color.g, color.b, color.a};
}

auto needs_srgb_encode(SurfaceDesc const& desc) -> bool {
    switch (desc.pixel_format) {
    case PixelFormat::RGBA8Unorm_sRGB:
    case PixelFormat::BGRA8Unorm_sRGB:
        return true;
    default:
        break;
    }
    return desc.color_space == ColorSpace::sRGB;
}

auto encode_pixel(float const* linear_premul,
                  SurfaceDesc const& desc,
                  bool encode_srgb) -> std::array<std::uint8_t, 4> {
    auto alpha = clamp_unit(linear_premul[3]);

    std::array<float, 3> premul_linear{
        clamp_unit(linear_premul[0]),
        clamp_unit(linear_premul[1]),
        clamp_unit(linear_premul[2]),
    };

    std::array<float, 3> straight_linear{0.0f, 0.0f, 0.0f};
    if (alpha > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            straight_linear[i] = std::clamp(premul_linear[i] / alpha, 0.0f, 1.0f);
        }
    }

    std::array<float, 3> encoded{};
    if (encode_srgb) {
        for (int i = 0; i < 3; ++i) {
            auto value = linear_to_srgb(straight_linear[i]);
            if (desc.premultiplied_alpha) {
                value *= alpha;
            }
            encoded[i] = clamp_unit(value);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            auto value = desc.premultiplied_alpha ? premul_linear[i] : straight_linear[i];
            encoded[i] = clamp_unit(value);
        }
    }

    return {
        to_byte(encoded[0]),
        to_byte(encoded[1]),
        to_byte(encoded[2]),
        to_byte(alpha)
    };
}

auto encode_linear_color_to_output(LinearPremulColor const& color,
                                   SurfaceDesc const& desc) -> std::array<float, 4> {
    std::array<float, 4> premul{
        clamp_unit(color.r),
        clamp_unit(color.g),
        clamp_unit(color.b),
        clamp_unit(color.a),
    };
    auto encoded = encode_pixel(premul.data(), desc, needs_srgb_encode(desc));
    return {
        static_cast<float>(encoded[0]) / 255.0f,
        static_cast<float>(encoded[1]) / 255.0f,
        static_cast<float>(encoded[2]) / 255.0f,
        static_cast<float>(encoded[3]) / 255.0f,
    };
}

} // namespace SP::UI::PathRenderer2DDetail
