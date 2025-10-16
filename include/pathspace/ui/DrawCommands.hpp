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

struct ImageCommand {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float uv_min_x = 0.0f;
    float uv_min_y = 0.0f;
    float uv_max_x = 1.0f;
    float uv_max_y = 1.0f;
    std::uint64_t image_fingerprint = 0;
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
};

struct TextGlyphsCommand {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    std::uint32_t glyph_count = 0;
    std::uint32_t atlas_page = 0;
    float px_range = 1.0f;
    float font_size = 12.0f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
};

struct PathCommand {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    std::uint32_t path_offset = 0;
    std::uint32_t path_length = 0;
    std::uint32_t fill_rule = 0;
    float stroke_width = 0.0f;
    std::array<float, 4> fill_color{0.0f, 0.0f, 0.0f, 1.0f};
};

struct MeshCommand {
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

constexpr auto payload_size_bytes(DrawCommandKind kind) -> std::size_t {
    switch (kind) {
    case DrawCommandKind::Rect:
        return sizeof(RectCommand);
    case DrawCommandKind::RoundedRect:
        return sizeof(RoundedRectCommand);
    case DrawCommandKind::Image:
        return sizeof(ImageCommand);
    case DrawCommandKind::TextGlyphs:
        return sizeof(TextGlyphsCommand);
    case DrawCommandKind::Path:
        return sizeof(PathCommand);
    case DrawCommandKind::Mesh:
        return sizeof(MeshCommand);
    }
    return 0;
}

} // namespace SP::UI::Scene
