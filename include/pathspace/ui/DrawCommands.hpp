#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace SP::UI::Scene {

enum class DrawCommandKind : std::uint32_t {
    Rect = 0,
    RoundedRect = 1,
    Image = 2,
    TextGlyphs = 3,
    Path = 4,
    Mesh = 5,
};

struct RectCommand {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
};

struct RoundedRectCommand {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float radius_top_left = 0.0f;
    float radius_top_right = 0.0f;
    float radius_bottom_right = 0.0f;
    float radius_bottom_left = 0.0f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
};

constexpr auto payload_size_bytes(DrawCommandKind kind) -> std::size_t {
    switch (kind) {
    case DrawCommandKind::Rect:
        return sizeof(RectCommand);
    case DrawCommandKind::RoundedRect:
        return sizeof(RoundedRectCommand);
    case DrawCommandKind::Image:
    case DrawCommandKind::TextGlyphs:
    case DrawCommandKind::Path:
    case DrawCommandKind::Mesh:
        return 0;
    }
    return 0;
}

} // namespace SP::UI::Scene
