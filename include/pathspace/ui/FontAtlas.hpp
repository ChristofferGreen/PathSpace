#pragma once

#include <cstdint>
#include <vector>

namespace SP::UI {

enum class FontAtlasFormat : std::uint32_t {
    Alpha8 = 0,
};

struct FontAtlasGlyph {
    std::uint32_t glyph_id = 0;
    std::uint32_t codepoint = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float px_range = 1.0f;
};

struct FontAtlasData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FontAtlasFormat format = FontAtlasFormat::Alpha8;
    float em_size = 16.0f;
    std::vector<FontAtlasGlyph> glyphs;
    std::vector<std::uint8_t> pixels;
};

inline constexpr char kFontAtlasMagic[4] = {'P', 'S', 'A', 'T'};
inline constexpr std::uint32_t kFontAtlasBinaryVersion = 1;
inline constexpr std::size_t FontAtlasBinaryHeaderSize = 28;

} // namespace SP::UI
