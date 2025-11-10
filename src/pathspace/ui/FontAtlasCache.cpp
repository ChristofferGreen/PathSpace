#include <pathspace/ui/FontAtlasCache.hpp>

#include <pathspace/core/Error.hpp>

#include <bit>
#include <cstddef>
#include <cstring>

namespace {

auto make_decode_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::InvalidType, std::move(message)};
}

auto read_u16(std::uint8_t const* data) -> std::uint16_t {
    return static_cast<std::uint16_t>(data[0])
         | (static_cast<std::uint16_t>(data[1]) << 8);
}

auto read_u32(std::uint8_t const* data) -> std::uint32_t {
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}

auto read_f32(std::uint8_t const* data) -> float {
    auto value = read_u32(data);
    return std::bit_cast<float>(value);
}

constexpr std::size_t kGlyphRecordSize = 40;

} // namespace

namespace SP::UI {

auto FontAtlasCache::load(PathSpace& space,
                          std::string const& atlas_path,
                          std::uint64_t fingerprint)
    -> SP::Expected<std::shared_ptr<FontAtlasData const>> {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(fingerprint);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    auto bytes = space.read<std::vector<std::uint8_t>>(atlas_path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }

    auto decoded = decode(*bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.insert_or_assign(fingerprint, *decoded);
    }
    return *decoded;
}

void FontAtlasCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

auto FontAtlasCache::resident_bytes() const -> std::size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (auto const& entry : cache_) {
        if (entry.second) {
            total += entry.second->pixels.size();
            total += entry.second->glyphs.size() * sizeof(FontAtlasGlyph);
        }
    }
    return total;
}

auto FontAtlasCache::decode(std::vector<std::uint8_t> const& bytes) const
    -> SP::Expected<std::shared_ptr<FontAtlasData const>> {
    if (bytes.size() < FontAtlasBinaryHeaderSize) {
        return std::unexpected(make_decode_error("font atlas payload too small"));
    }

    auto const* base = bytes.data();
    if (std::memcmp(base, kFontAtlasMagic, 4) != 0) {
        return std::unexpected(make_decode_error("font atlas magic mismatch"));
    }

    auto version = read_u16(base + 4);
    if (version != kFontAtlasBinaryVersion) {
        return std::unexpected(make_decode_error("font atlas version unsupported"));
    }

    auto flags = read_u16(base + 6);
    (void)flags;
    auto width = read_u32(base + 8);
    auto height = read_u32(base + 12);
    auto glyph_count = read_u32(base + 16);
    auto format_raw = read_u32(base + 20);
    auto em_size = read_f32(base + 24);
    if (width == 0 || height == 0) {
        return std::unexpected(make_decode_error("font atlas dimensions invalid"));
    }

    auto glyph_table_bytes = static_cast<std::size_t>(glyph_count) * kGlyphRecordSize;
    auto header_size = FontAtlasBinaryHeaderSize;
    if (bytes.size() < header_size + glyph_table_bytes) {
        return std::unexpected(make_decode_error("font atlas glyph table truncated"));
    }

    auto format = static_cast<FontAtlasFormat>(format_raw);
    std::size_t bytes_per_pixel = 1;
    switch (format) {
    case FontAtlasFormat::Alpha8:
        bytes_per_pixel = 1;
        break;
    case FontAtlasFormat::Rgba8:
        bytes_per_pixel = 4;
        break;
    default:
        return std::unexpected(make_decode_error("unsupported font atlas format"));
    }

    auto expected_pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel;
    auto pixel_offset = header_size + glyph_table_bytes;
    auto pixel_bytes = bytes.size() - pixel_offset;
    if (pixel_bytes != expected_pixels) {
        return std::unexpected(make_decode_error("font atlas pixel data size mismatch"));
    }

    auto atlas = std::make_shared<FontAtlasData>();
    atlas->width = width;
    atlas->height = height;
    atlas->format = format;
    atlas->em_size = em_size;
    atlas->bytes_per_pixel = static_cast<std::uint32_t>(bytes_per_pixel);
    atlas->glyphs.reserve(glyph_count);

    for (std::size_t i = 0; i < glyph_count; ++i) {
        auto const* glyph_ptr = base + header_size + i * kGlyphRecordSize;
        FontAtlasGlyph glyph{};
        glyph.glyph_id = read_u32(glyph_ptr + 0);
        glyph.codepoint = read_u32(glyph_ptr + 4);
        glyph.u0 = read_f32(glyph_ptr + 8);
        glyph.v0 = read_f32(glyph_ptr + 12);
        glyph.u1 = read_f32(glyph_ptr + 16);
        glyph.v1 = read_f32(glyph_ptr + 20);
        glyph.advance = read_f32(glyph_ptr + 24);
        glyph.offset_x = read_f32(glyph_ptr + 28);
        glyph.offset_y = read_f32(glyph_ptr + 32);
        glyph.px_range = read_f32(glyph_ptr + 36);
        atlas->glyphs.push_back(glyph);
    }

    atlas->pixels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pixel_offset), bytes.end());
    return atlas;
}

} // namespace SP::UI
