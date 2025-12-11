#include "RuntimeDetail.hpp"
#include "TextGlyphFallback.hpp"

#include <pathspace/ui/FontAtlas.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace SP::UI::Runtime::Resources::Fonts {

using namespace Detail;

namespace {

namespace Fallback = SP::UI::TextFallback;

constexpr int kAtlasColumns = 8;
constexpr int kAtlasCellWidth = 8;
constexpr int kAtlasCellHeight = 8;

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

auto build_default_atlas() -> SP::UI::FontAtlasData {
    SP::UI::FontAtlasData atlas{};
    atlas.format = SP::UI::FontAtlasFormat::Alpha8;
    atlas.em_size = static_cast<float>(Fallback::kGlyphRows);

    auto const glyph_count = Fallback::kGlyphPatterns.size();
    auto const rows = static_cast<int>((glyph_count + kAtlasColumns - 1) / kAtlasColumns);
    atlas.width = static_cast<std::uint32_t>(kAtlasColumns * kAtlasCellWidth);
    atlas.height = static_cast<std::uint32_t>(rows * kAtlasCellHeight);
    atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * atlas.height, 0u);
    atlas.glyphs.reserve(glyph_count);

    auto const inv_width = 1.0f / static_cast<float>(atlas.width);
    auto const inv_height = 1.0f / static_cast<float>(atlas.height);

    for (std::size_t index = 0; index < glyph_count; ++index) {
        auto const& pattern = Fallback::kGlyphPatterns[index];
        auto const col = static_cast<int>(index % kAtlasColumns);
        auto const row = static_cast<int>(index / kAtlasColumns);
        auto const origin_x = col * kAtlasCellWidth + 1;
        auto const origin_y = row * kAtlasCellHeight + 1;

        for (int r = 0; r < Fallback::kGlyphRows; ++r) {
            auto mask = static_cast<unsigned int>(pattern.rows[static_cast<std::size_t>(r)]);
            for (int c = 0; c < pattern.width; ++c) {
                bool filled = (mask & (1u << (pattern.width - 1 - c))) != 0;
                if (!filled) {
                    continue;
                }
                auto x = origin_x + c;
                auto y = origin_y + r;
                if (x >= 0 && x < static_cast<int>(atlas.width)
                    && y >= 0 && y < static_cast<int>(atlas.height)) {
                    auto idx = static_cast<std::size_t>(y) * atlas.width + static_cast<std::size_t>(x);
                    atlas.pixels[idx] = 255u;
                }
            }
        }

        SP::UI::FontAtlasGlyph glyph{};
        glyph.glyph_id = static_cast<std::uint32_t>(static_cast<unsigned char>(pattern.ch));
        glyph.codepoint = glyph.glyph_id;
        glyph.u0 = static_cast<float>(origin_x) * inv_width;
        glyph.v0 = static_cast<float>(origin_y) * inv_height;
        glyph.u1 = static_cast<float>(origin_x + pattern.width) * inv_width;
        glyph.v1 = static_cast<float>(origin_y + Fallback::kGlyphRows) * inv_height;
        glyph.advance = static_cast<float>(pattern.width);
        glyph.offset_x = 0.0f;
        glyph.offset_y = 0.0f;
        glyph.px_range = 1.0f;
        atlas.glyphs.push_back(glyph);
    }

    return atlas;
}

auto encode_font_atlas(SP::UI::FontAtlasData const& atlas) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes;
    auto header_size = SP::UI::FontAtlasBinaryHeaderSize;
    auto glyph_bytes = atlas.glyphs.size() * 40u;
    bytes.reserve(header_size + glyph_bytes + atlas.pixels.size());

    auto append_u16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    };
    auto append_u32 = [&](std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    };
    auto append_f32 = [&](float value) {
        auto bits = std::bit_cast<std::uint32_t>(value);
        append_u32(bits);
    };

    bytes.insert(bytes.end(), std::begin(SP::UI::kFontAtlasMagic), std::end(SP::UI::kFontAtlasMagic));
    append_u16(static_cast<std::uint16_t>(SP::UI::kFontAtlasBinaryVersion));
    append_u16(0);
    append_u32(atlas.width);
    append_u32(atlas.height);
    append_u32(static_cast<std::uint32_t>(atlas.glyphs.size()));
    append_u32(static_cast<std::uint32_t>(atlas.format));
    append_f32(atlas.em_size);

    for (auto const& glyph : atlas.glyphs) {
        append_u32(glyph.glyph_id);
        append_u32(glyph.codepoint);
        append_f32(glyph.u0);
        append_f32(glyph.v0);
        append_f32(glyph.u1);
        append_f32(glyph.v1);
        append_f32(glyph.advance);
        append_f32(glyph.offset_x);
        append_f32(glyph.offset_y);
        append_f32(glyph.px_range);
    }

    bytes.insert(bytes.end(), atlas.pixels.begin(), atlas.pixels.end());
    return bytes;
}

auto build_color_atlas_from_alpha(SP::UI::FontAtlasData const& source) -> SP::UI::FontAtlasData {
    SP::UI::FontAtlasData color = source;
    color.format = SP::UI::FontAtlasFormat::Rgba8;
    color.bytes_per_pixel = 4;
    auto pixel_count = static_cast<std::size_t>(color.width) * color.height;
    color.pixels.resize(pixel_count * 4u);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto value = (i < source.pixels.size()) ? source.pixels[i] : std::uint8_t{0u};
        auto base_index = i * 4u;
        color.pixels[base_index + 0] = value;
        color.pixels[base_index + 1] = value;
        color.pixels[base_index + 2] = value;
        color.pixels[base_index + 3] = value;
    }
    return color;
}

auto persist_default_atlas(PathSpace& space,
                           FontResourcePaths const& paths,
                           std::uint64_t revision,
                           bool emit_color_atlas,
                           SP::UI::FontAtlasFormat preferred_format) -> SP::Expected<void> {
    auto atlas = build_default_atlas();
    auto encoded = encode_font_atlas(atlas);

    auto base = paths.root.getPath();
    auto revision_dir = base + "/builds/" + format_revision(revision);
    auto atlas_path = revision_dir + "/atlas.bin";
    if (auto status = replace_single<std::vector<std::uint8_t>>(space, atlas_path, encoded); !status) {
        return status;
    }

    if (emit_color_atlas) {
        auto color_atlas = build_color_atlas_from_alpha(atlas);
        auto encoded_color = encode_font_atlas(color_atlas);
        auto color_path = revision_dir + "/atlas_color.bin";
        if (auto status = replace_single<std::vector<std::uint8_t>>(space, color_path, encoded_color); !status) {
            return status;
        }
    }

    std::ostringstream meta;
    meta << "{";
    meta << "\"width\":" << atlas.width << ",";
    meta << "\"height\":" << atlas.height << ",";
    meta << "\"format\":\"alpha8\",";
    meta << "\"emSize\":" << atlas.em_size << ",";
    meta << "\"glyphCount\":" << atlas.glyphs.size() << ",";
    meta << "\"hasColorAtlas\":" << (emit_color_atlas ? "true" : "false") << ",";
    meta << "\"preferredFormat\":\""
         << (preferred_format == SP::UI::FontAtlasFormat::Rgba8 ? "rgba8" : "alpha8") << "\"";
    meta << "}";

    auto meta_path = revision_dir + "/meta";
    if (auto status = replace_single<std::string>(space, meta_path + "/atlas.json", meta.str()); !status) {
        return status;
    }

    return {};
}

auto make_paths(AppRootPathView appRoot,
                std::string_view family,
                std::string_view style) -> SP::Expected<FontResourcePaths> {
    if (auto status = ensure_identifier(family, "font family"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_identifier(style, "font style"); !status) {
        return std::unexpected(status.error());
    }

    auto root = SP::App::resolve_resource(appRoot, {"fonts", family, style});
    if (!root) {
        return std::unexpected(root.error());
    }

    FontResourcePaths paths{};
    paths.root = *root;
    auto const& base = root->getPath();
    paths.meta = ConcretePath{base + "/meta"};
    paths.active_revision = ConcretePath{base + "/meta/active_revision"};
    paths.builds = ConcretePath{base + "/builds"};
    paths.inbox = ConcretePath{base + "/inbox"};
    return paths;
}

} // namespace

auto Resolve(AppRootPathView appRoot,
             std::string_view family,
             std::string_view style) -> SP::Expected<FontResourcePaths> {
    return make_paths(appRoot, family, style);
}

auto Register(PathSpace& space,
              AppRootPathView appRoot,
              RegisterFontParams const& params) -> SP::Expected<FontResourcePaths> {
    auto paths = make_paths(appRoot, params.family, params.style);
    if (!paths) {
        return std::unexpected(paths.error());
    }

    auto const meta_base = std::string(paths->meta.getPath());
    auto family_path = meta_base + "/family";
    if (auto status = replace_single<std::string>(space, family_path, params.family); !status) {
        return std::unexpected(status.error());
    }

    auto style_path = meta_base + "/style";
    if (auto status = replace_single<std::string>(space, style_path, params.style); !status) {
        return std::unexpected(status.error());
    }

    auto weight = params.weight.empty() ? std::string{"400"} : params.weight;
    auto weight_path = meta_base + "/weight";
    if (auto status = replace_single<std::string>(space, weight_path, weight); !status) {
        return std::unexpected(status.error());
    }

    std::vector<std::string> sanitized_fallbacks;
    sanitized_fallbacks.reserve(params.fallback_families.size());
    std::unordered_set<std::string> seen{};
    for (auto const& entry : params.fallback_families) {
        if (entry.empty()) {
            continue;
        }
        if (entry == params.family) {
            continue;
        }
        if (seen.insert(entry).second) {
            sanitized_fallbacks.emplace_back(entry);
        }
    }
    if (sanitized_fallbacks.empty()) {
        sanitized_fallbacks.emplace_back("system-ui");
    }

    auto fallbacks_path = meta_base + "/fallbacks";
    if (auto status = replace_single<std::vector<std::string>>(space, fallbacks_path, sanitized_fallbacks); !status) {
        return std::unexpected(status.error());
    }

    auto atlas_base = meta_base + "/atlas";
    auto soft_path = atlas_base + "/softBytes";
    if (auto status = replace_single<std::uint64_t>(space, soft_path, params.atlas_soft_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto hard_path = atlas_base + "/hardBytes";
    if (auto status = replace_single<std::uint64_t>(space, hard_path, params.atlas_hard_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto run_bytes_path = atlas_base + "/shapedRunApproxBytes";
    if (auto status = replace_single<std::uint64_t>(space, run_bytes_path, params.shaped_run_approx_bytes); !status) {
        return std::unexpected(status.error());
    }

    auto has_color_path = atlas_base + "/hasColor";
    if (auto status = replace_single<std::uint64_t>(space,
                                                    has_color_path,
                                                    params.emit_color_atlas ? 1ull : 0ull);
        !status) {
        return std::unexpected(status.error());
    }

    auto preferred_format_value =
        (params.preferred_atlas_format == SP::UI::FontAtlasFormat::Rgba8) ? "rgba8" : "alpha8";
    auto preferred_format_path = atlas_base + "/preferredFormat";
    if (auto status = replace_single<std::string>(space, preferred_format_path, preferred_format_value); !status) {
        return std::unexpected(status.error());
    }

    auto active_path = std::string(paths->active_revision.getPath());
    if (auto status = replace_single<std::uint64_t>(space, active_path, params.initial_revision); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = persist_default_atlas(space,
                                           *paths,
                                           params.initial_revision,
                                           params.emit_color_atlas,
                                           params.preferred_atlas_format);
        !status) {
        return std::unexpected(status.error());
    }

    return *paths;
}

namespace {

struct BuiltInFontSpec {
    std::string_view family;
    std::string_view style;
    std::string_view weight;
    std::array<std::string_view, 3> fallbacks;
    std::uint64_t revision = 1;
};

constexpr std::array<BuiltInFontSpec, 2> kBuiltInFonts = {{
    {.family = "PathSpaceSans",
     .style = "Regular",
     .weight = "400",
     .fallbacks = {"system-ui", "sans-serif", "Helvetica"},
     .revision = 1},
    {.family = "PathSpaceSans",
     .style = "Italic",
     .weight = "400",
     .fallbacks = {"system-ui", "sans-serif", "Helvetica"},
     .revision = 1},
}};

auto ensure_builtin_font(PathSpace& space,
                         AppRootPathView appRoot,
                         BuiltInFontSpec const& spec) -> SP::Expected<void> {
    auto paths = make_paths(appRoot, spec.family, spec.style);
    if (!paths) {
        return std::unexpected(paths.error());
    }

    auto active = space.read<std::uint64_t, std::string>(paths->active_revision.getPath());
    if (active) {
        return {};
    }

    auto const code = active.error().code;
    if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
        return std::unexpected(active.error());
    }

    RegisterFontParams params{};
    params.family = std::string(spec.family);
    params.style = std::string(spec.style);
    params.weight = std::string(spec.weight);
    params.initial_revision = spec.revision;
    params.fallback_families.reserve(spec.fallbacks.size());
    for (auto fallback : spec.fallbacks) {
        if (!fallback.empty()) {
            params.fallback_families.emplace_back(fallback);
        }
    }

    auto registered = Register(space, appRoot, params);
    if (!registered) {
        return std::unexpected(registered.error());
    }

    return {};
}

} // namespace

auto EnsureBuiltInPack(PathSpace& space,
                       AppRootPathView appRoot) -> SP::Expected<void> {
    for (auto const& spec : kBuiltInFonts) {
        if (auto status = ensure_builtin_font(space, appRoot, spec); !status) {
            return status;
        }
    }
    return {};
}

} // namespace SP::UI::Runtime::Resources::Fonts
