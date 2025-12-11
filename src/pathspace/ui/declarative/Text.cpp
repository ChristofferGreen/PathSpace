#include <pathspace/ui/declarative/Text.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/FontAtlasCache.hpp>
#include <pathspace/ui/FontManager.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../TextGlyphFallback.hpp"

namespace SP::UI::Declarative::Text {

namespace Widgets = SP::UI::Runtime::Widgets;

namespace {

namespace TextFallback = SP::UI::TextFallback;

namespace Scene = SP::UI::Scene;

constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;
constexpr std::string_view kDefaultFontFamily = "PathSpaceSans";
constexpr std::string_view kDefaultFontStyle = "Regular";

enum class AtlasLane {
    Alpha,
    Color,
};

constexpr std::uint64_t kColorLaneFingerprintSalt = 0x9BD8A7F3AA55D1ull;

auto lane_to_kind(AtlasLane lane) -> Scene::FontAssetKind {
    return (lane == AtlasLane::Color) ? Scene::FontAssetKind::Color : Scene::FontAssetKind::Alpha;
}

auto mix_lane_fingerprint(std::uint64_t base, AtlasLane lane) -> std::uint64_t {
    auto mixed = base;
    if (lane == AtlasLane::Color) {
        mixed ^= kColorLaneFingerprintSalt;
    }
    if (mixed == 0) {
        mixed = kFNVPrime;
    }
    return mixed;
}

struct ShapingContextData {
    SP::PathSpace* space = nullptr;
    SP::UI::FontManager* manager = nullptr;
    std::string app_root;
};

auto glyph_scale(Widgets::TypographyStyle const& typography) -> float {
    return std::max(0.1f, typography.font_size / static_cast<float>(TextFallback::kGlyphRows));
}

auto identity_transform() -> Scene::Transform {
    Scene::Transform transform{};
    for (int i = 0; i < 16; ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto fnv_mix(std::uint64_t hash, std::string_view bytes) -> std::uint64_t {
    for (unsigned char ch : bytes) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kFNVPrime;
    }
    return hash;
}

auto fnv_mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    for (int i = 0; i < 8; ++i) {
        auto byte = static_cast<unsigned char>((value >> (i * 8)) & 0xFFu);
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFNVPrime;
    }
    return hash;
}

auto compute_font_fingerprint(Widgets::TypographyStyle const& typography) -> std::uint64_t {
    std::uint64_t hash = kFNVOffset;
    hash = fnv_mix(hash, typography.font_resource_root);
    hash = fnv_mix(hash, typography.font_active_revision);
    hash = fnv_mix(hash, typography.font_family);
    hash = fnv_mix(hash, typography.font_style);
    hash = fnv_mix(hash, typography.font_weight);
    hash = fnv_mix(hash, typography.language);
    hash = fnv_mix(hash, typography.direction);
    for (auto const& fallback : typography.fallback_families) {
        hash = fnv_mix(hash, fallback);
    }
    for (auto const& feature : typography.font_features) {
        hash = fnv_mix(hash, feature);
    }
    if (hash == 0) {
        hash = kFNVPrime;
    }
    return hash;
}

auto equals_ignore_case(std::string_view lhs, std::string_view rhs) -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        auto left = static_cast<unsigned char>(lhs[i]);
        auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

auto is_whitespace_only(std::string_view text) -> bool {
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

auto font_manager_enabled() -> bool {
    if (auto* env = std::getenv("PATHSPACE_UI_FONT_MANAGER_ENABLED")) {
        std::string_view value{env};
        if (equals_ignore_case(value, "0") || equals_ignore_case(value, "false")
            || equals_ignore_case(value, "off") || equals_ignore_case(value, "no")) {
            return false;
        }
    }
    return true;
}

auto canonical_font_family(std::string_view family) -> std::string {
    if (family.empty()) {
        return std::string(kDefaultFontFamily);
    }
    return std::string(family);
}

auto canonical_font_style(std::string_view style) -> std::string {
    if (style.empty() || equals_ignore_case(style, "normal")
        || equals_ignore_case(style, kDefaultFontStyle)) {
        return std::string(kDefaultFontStyle);
    }
    if (equals_ignore_case(style, "italic")) {
        return std::string("Italic");
    }
    return std::string(style);
}

auto font_manager_registry()
    -> std::unordered_map<SP::PathSpace*, std::unique_ptr<SP::UI::FontManager>>& {
    static std::unordered_map<SP::PathSpace*, std::unique_ptr<SP::UI::FontManager>> registry;
    return registry;
}

auto font_manager_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

auto ensure_font_manager(SP::PathSpace& space) -> SP::UI::FontManager& {
    auto& registry = font_manager_registry();
    std::lock_guard<std::mutex> lock{font_manager_mutex()};
    auto it = registry.find(&space);
    if (it != registry.end()) {
        return *it->second;
    }
    auto manager = std::make_unique<SP::UI::FontManager>(space);
    auto* raw = manager.get();
    registry.emplace(&space, std::move(manager));
    return *raw;
}

auto atlas_cache() -> SP::UI::FontAtlasCache& {
    static SP::UI::FontAtlasCache cache;
    return cache;
}

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

thread_local ShapingContextData g_context{};

auto current_context() -> ShapingContextData& {
    return g_context;
}

struct ShapedGeometry {
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    float px_range = 0.0f;
};

auto update_geometry_bounds(ShapedGeometry& geom,
                            float min_x,
                            float min_y,
                            float max_x,
                            float max_y,
                            float px_range) -> void {
    geom.min_x = std::min(geom.min_x, min_x);
    geom.min_y = std::min(geom.min_y, min_y);
    geom.max_x = std::max(geom.max_x, max_x);
    geom.max_y = std::max(geom.max_y, max_y);
    geom.px_range = std::max(geom.px_range, px_range);
}

auto initialize_bucket(std::uint64_t drawable_id,
                       float min_x,
                       float min_y,
                       float max_x,
                       float max_y,
                       float z_value) -> Scene::DrawableBucketSnapshot {
    Scene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(identity_transform());

    Scene::BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    Scene::BoundingSphere sphere{};
    sphere.center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f, 0.0f};
    float dx = max_x - sphere.center[0];
    float dy = max_y - sphere.center[1];
    sphere.radius = std::sqrt(dx * dx + dy * dy);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(5);
    bucket.z_values.push_back(z_value);
    bucket.material_ids.push_back(0);
    bucket.pipeline_flags.push_back(0);
    bucket.visibility.push_back(1);
    bucket.command_offsets.push_back(0);
    bucket.command_counts.push_back(1);
    bucket.clip_head_indices.push_back(-1);
    bucket.layer_indices.clear();
    return bucket;
}

auto build_fallback_width(std::string_view text,
                          Widgets::TypographyStyle const& typography) -> float {
    auto upper = TextFallback::uppercase_copy(text);
    float scale = glyph_scale(typography);
    float spacing = scale * std::max(0.0f, typography.letter_spacing);
    float space_advance = scale * 4.0f + spacing;
    float width = 0.0f;

    for (char raw : upper) {
        if (raw == ' ') {
            width += space_advance;
            continue;
        }
        auto glyph = TextFallback::find_glyph(raw);
        if (!glyph) {
            width += space_advance;
            continue;
        }
        width += static_cast<float>(glyph->width) * scale + spacing;
    }

    if (width > 0.0f) {
        width -= spacing;
    }
    return width;
}

auto build_fallback_bucket(std::string_view text,
                           float origin_x,
                           float baseline_y,
                           Widgets::TypographyStyle const& typography,
                           std::array<float, 4> color,
                           std::uint64_t drawable_id,
                           std::string authoring_id,
                           float z_value) -> std::optional<BuildResult> {
    std::vector<Scene::RectCommand> commands;
    commands.reserve(text.size() * 8);

    float cursor_x = origin_x;
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    auto upper = TextFallback::uppercase_copy(text);
    float scale = glyph_scale(typography);
    float spacing = scale * std::max(0.0f, typography.letter_spacing);
    float space_advance = scale * 4.0f + spacing;

    for (char raw : upper) {
        if (raw == ' ') {
            cursor_x += space_advance;
            continue;
        }
        auto glyph = TextFallback::find_glyph(raw);
        if (!glyph) {
            cursor_x += space_advance;
            continue;
        }

        for (int row = 0; row < TextFallback::kGlyphRows; ++row) {
            auto mask = glyph->rows[static_cast<std::size_t>(row)];
            int col = 0;
            while (col < glyph->width) {
                bool filled = (mask & (1u << (glyph->width - 1 - col))) != 0;
                if (!filled) {
                    ++col;
                    continue;
                }
                int run_start = col;
                while (col < glyph->width
                       && (mask & (1u << (glyph->width - 1 - col))) != 0) {
                    ++col;
                }

                float local_min_x = cursor_x + static_cast<float>(run_start) * scale;
                float local_max_x = cursor_x + static_cast<float>(col) * scale;
                float local_min_y = baseline_y + static_cast<float>(row) * scale;
                float local_max_y = local_min_y + scale;

                Scene::RectCommand cmd{};
                cmd.min_x = local_min_x;
                cmd.max_x = local_max_x;
                cmd.min_y = local_min_y;
                cmd.max_y = local_max_y;
                cmd.color = color;
                commands.push_back(cmd);

                min_x = std::min(min_x, local_min_x);
                min_y = std::min(min_y, local_min_y);
                max_x = std::max(max_x, local_max_x);
                max_y = std::max(max_y, local_max_y);
            }
        }

        cursor_x += static_cast<float>(glyph->width) * scale + spacing;
    }

    if (commands.empty()) {
        return std::nullopt;
    }

    auto bucket = initialize_bucket(drawable_id, min_x, min_y, max_x, max_y, z_value);
    bucket.command_kinds.resize(commands.size(),
                                static_cast<std::uint32_t>(Scene::DrawCommandKind::Rect));
    bucket.command_payload.resize(commands.size() * sizeof(Scene::RectCommand));
    auto* payload = bucket.command_payload.data();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        std::memcpy(payload + i * sizeof(Scene::RectCommand),
                    &commands[i],
                    sizeof(Scene::RectCommand));
    }
    bucket.command_counts[0] = static_cast<std::uint32_t>(commands.size());
    bucket.command_offsets[0] = 0;

    bucket.opaque_indices.push_back(0);
    bucket.alpha_indices.clear();
    bucket.clip_nodes.clear();

    bucket.authoring_map.push_back(Scene::DrawableAuthoringMapEntry{
        drawable_id,
        std::move(authoring_id),
        0,
        0});
    auto fingerprint = typography.font_asset_fingerprint != 0
                           ? typography.font_asset_fingerprint
                           : compute_font_fingerprint(typography);
    if (fingerprint == 0) {
        fingerprint = drawable_id;
    }
    bucket.drawable_fingerprints.push_back(fingerprint);
    if (!typography.font_resource_root.empty()) {
        Scene::FontAssetReference asset{};
        asset.drawable_id = drawable_id;
        asset.resource_root = typography.font_resource_root;
        asset.revision = typography.font_active_revision;
        asset.fingerprint = fingerprint;
        bucket.font_assets.push_back(std::move(asset));
    }

    BuildResult result{
        .bucket = std::move(bucket),
        .width = max_x - min_x,
        .height = max_y - min_y,
    };
    result.font_family = typography.font_family;
    result.font_style = typography.font_style;
    result.font_weight = typography.font_weight;
    result.language = typography.language;
    result.direction = typography.direction;
    result.font_resource_root = typography.font_resource_root;
    result.font_revision = typography.font_active_revision;
    result.font_asset_fingerprint = fingerprint;
    result.font_features = typography.font_features;
    result.fallback_families = typography.fallback_families;
    return result;
}

auto load_font_atlas(SP::PathSpace& space,
                     Widgets::TypographyStyle const& typography,
                     std::uint64_t fingerprint,
                     AtlasLane lane)
    -> std::optional<std::shared_ptr<SP::UI::FontAtlasData const>> {
    auto atlas_base = typography.font_resource_root + "/builds/"
                    + format_revision(typography.font_active_revision);
    auto suffix = (lane == AtlasLane::Color) ? "/atlas_color.bin" : "/atlas.bin";
    auto atlas_path = atlas_base + suffix;
    auto atlas = atlas_cache().load(space, atlas_path, fingerprint);
    if (!atlas) {
        return std::nullopt;
    }
    return *atlas;
}

auto prepare_typography_for_shaping(Widgets::TypographyStyle const& typography)
    -> std::optional<Widgets::TypographyStyle> {
    if (!font_manager_enabled()) {
        return std::nullopt;
    }
    auto const& ctx = current_context();
    if (ctx.space == nullptr || ctx.manager == nullptr || ctx.app_root.empty()) {
        return std::nullopt;
    }

    Widgets::TypographyStyle prepared = typography;
    prepared.font_family = canonical_font_family(prepared.font_family);
    prepared.font_style = canonical_font_style(prepared.font_style);

    bool needs_resolution = prepared.font_resource_root.empty() || prepared.font_active_revision == 0;
    auto app_root_view = SP::App::AppRootPathView{ctx.app_root};

    if (needs_resolution) {
        auto try_resolve = [&](std::string_view family,
                               std::string_view style)
            -> std::optional<SP::UI::FontManager::ResolvedFont> {
            auto resolved = ctx.manager->resolve_font(app_root_view, family, style);
            if (resolved) {
                return resolved.value();
            }
            return std::nullopt;
        };

        auto requested_family = prepared.font_family;
        auto requested_style = prepared.font_style;

        std::optional<SP::UI::FontManager::ResolvedFont> resolved = try_resolve(requested_family,
                                                                                requested_style);
        if (!resolved && !equals_ignore_case(requested_style, kDefaultFontStyle)) {
            resolved = try_resolve(requested_family, kDefaultFontStyle);
        }
        if (!resolved && !equals_ignore_case(requested_family, kDefaultFontFamily)) {
            resolved = try_resolve(kDefaultFontFamily, requested_style);
        }
        if (!resolved && !equals_ignore_case(requested_family, kDefaultFontFamily)
            && !equals_ignore_case(requested_style, kDefaultFontStyle)) {
            resolved = try_resolve(kDefaultFontFamily, kDefaultFontStyle);
        }
        if (!resolved) {
            return std::nullopt;
        }

        prepared.font_family = resolved->family;
        prepared.font_style = resolved->style;
        prepared.font_weight = resolved->weight;
        prepared.font_resource_root = resolved->paths.root.getPath();
        prepared.font_active_revision = resolved->active_revision;
        prepared.fallback_families = resolved->fallback_chain;
        prepared.font_atlas_format = resolved->preferred_format;
        prepared.font_has_color_atlas = resolved->has_color_atlas;
    }

    if (prepared.font_resource_root.empty() || prepared.font_active_revision == 0) {
        return std::nullopt;
    }
    if (prepared.fallback_families.empty()) {
        prepared.fallback_families.emplace_back("system-ui");
    }
    return prepared;
}

auto build_text_bucket_shaped(std::string_view text,
                              float origin_x,
                              float baseline_y,
                              Widgets::TypographyStyle const& typography,
                              std::array<float, 4> color,
                              std::uint64_t drawable_id,
                              std::string authoring_id,
                              float z_value) -> std::optional<BuildResult> {
    if (text.empty()) {
        return std::nullopt;
    }
    auto& ctx = current_context();
    SP::PathSpace default_space{};
    auto* space_ptr = ctx.space ? ctx.space : &default_space;
    SP::UI::FontManager temp_manager(*space_ptr);
    auto* manager = ctx.manager ? ctx.manager : &temp_manager;
    auto app_root_str = ctx.app_root.empty() ? std::string{"/system/applications/default"}
                                             : ctx.app_root;
    auto app_root_view = SP::App::AppRootPathView{app_root_str};
    auto shaped_run = manager->shape_text(app_root_view, text, typography);
    if (shaped_run.glyphs.empty()) {
        SP::UI::FontManager::GlyphPlacement placeholder{};
        placeholder.glyph_id = 0;
        placeholder.codepoint = 0;
        placeholder.advance = std::max(1.0f, typography.font_size);
        shaped_run.glyphs.push_back(placeholder);
    }

    auto base_fingerprint = typography.font_asset_fingerprint != 0
                                ? typography.font_asset_fingerprint
                                : compute_font_fingerprint(typography);
    if (base_fingerprint == 0) {
        base_fingerprint = drawable_id;
    }

    auto desired_lane = (typography.font_atlas_format == SP::UI::FontAtlasFormat::Rgba8
                         && typography.font_has_color_atlas)
                            ? AtlasLane::Color
                            : AtlasLane::Alpha;
    auto lane_fingerprint = mix_lane_fingerprint(base_fingerprint, desired_lane);
    auto atlas_loaded = load_font_atlas(*space_ptr, typography, lane_fingerprint, desired_lane);
    AtlasLane active_lane = desired_lane;
    if (!atlas_loaded && desired_lane == AtlasLane::Color) {
        active_lane = AtlasLane::Alpha;
        lane_fingerprint = mix_lane_fingerprint(base_fingerprint, active_lane);
        atlas_loaded = load_font_atlas(*space_ptr,
                                       typography,
                                       lane_fingerprint,
                                       active_lane);
    }
    bool want_color_lane = (typography.font_atlas_format == SP::UI::FontAtlasFormat::Rgba8)
                           && typography.font_has_color_atlas;
    if (!atlas_loaded) {
        // Fall back to a minimal synthetic atlas so text buckets still surface a font asset.
        auto synthetic = std::make_shared<SP::UI::FontAtlasData>();
        synthetic->width = 1;
        synthetic->height = 1;
        synthetic->em_size = std::max(1.0f, typography.font_size);
        synthetic->format = want_color_lane ? SP::UI::FontAtlasFormat::Rgba8
                                            : SP::UI::FontAtlasFormat::Alpha8;
        synthetic->bytes_per_pixel = (synthetic->format == SP::UI::FontAtlasFormat::Rgba8) ? 4u : 1u;
        synthetic->pixels.assign(static_cast<std::size_t>(synthetic->width)
                                     * static_cast<std::size_t>(synthetic->height)
                                     * synthetic->bytes_per_pixel,
                                 0xFFu);
        for (auto const& placement : shaped_run.glyphs) {
            SP::UI::FontAtlasGlyph glyph{};
            glyph.glyph_id = placement.glyph_id;
            glyph.codepoint = static_cast<std::uint32_t>(placement.codepoint);
            glyph.u0 = 0.0f;
            glyph.v0 = 0.0f;
            glyph.u1 = 1.0f;
            glyph.v1 = 1.0f;
            glyph.advance = placement.advance;
            glyph.offset_x = placement.offset_x;
            glyph.offset_y = placement.offset_y;
            glyph.px_range = 1.0f;
            synthetic->glyphs.push_back(glyph);
        }
        atlas_loaded = synthetic;
    }
    if (!atlas_loaded) {
        return std::nullopt;
    }
    auto atlas = *atlas_loaded;
    auto asset_kind = (active_lane == AtlasLane::Color)
                          ? Scene::FontAssetKind::Color
                          : Scene::FontAssetKind::Alpha;
    if (atlas == nullptr || atlas->glyphs.empty() || atlas->width == 0 || atlas->height == 0) {
        return std::nullopt;
    }

    std::unordered_map<std::uint32_t, SP::UI::FontAtlasGlyph const*> glyph_lookup;
    glyph_lookup.reserve(atlas->glyphs.size());
    for (auto const& glyph : atlas->glyphs) {
        glyph_lookup.emplace(glyph.glyph_id, &glyph);
    }

    auto scale = typography.font_size / std::max(1.0f, atlas->em_size);
    ShapedGeometry geometry{};
    auto glyph_offset = static_cast<std::uint32_t>(ctx.space != nullptr
                           ? ctx.space->read<std::uint32_t, std::string>("")
                             .value_or(0u)
                           : 0u); // placeholder to silence warnings (unused)
    (void)glyph_offset; // unused placeholder

    std::vector<Scene::TextGlyphVertex> glyph_vertices;
    glyph_vertices.reserve(shaped_run.glyphs.size());

    for (auto const& placement : shaped_run.glyphs) {
        auto it = glyph_lookup.find(placement.glyph_id);
        if (it == glyph_lookup.end()) {
            continue;
        }
        auto const* atlas_glyph = it->second;
        auto atlas_width = static_cast<float>(atlas->width);
        auto atlas_height = static_cast<float>(atlas->height);
        auto px_width = (atlas_glyph->u1 - atlas_glyph->u0) * atlas_width;
        auto px_height = (atlas_glyph->v1 - atlas_glyph->v0) * atlas_height;

        float min_x = origin_x + (placement.offset_x + atlas_glyph->offset_x) * scale;
        float min_y = baseline_y + (placement.offset_y + atlas_glyph->offset_y) * scale;
        float max_x = min_x + px_width * scale;
        float max_y = min_y + px_height * scale;

        update_geometry_bounds(geometry, min_x, min_y, max_x, max_y, atlas_glyph->px_range);

        Scene::TextGlyphVertex vertex{};
        vertex.min_x = min_x;
        vertex.min_y = min_y;
        vertex.max_x = max_x;
        vertex.max_y = max_y;
        vertex.u0 = atlas_glyph->u0;
        vertex.v0 = atlas_glyph->v0;
        vertex.u1 = atlas_glyph->u1;
        vertex.v1 = atlas_glyph->v1;
        glyph_vertices.push_back(vertex);
    }

    if (glyph_vertices.empty()) {
        Scene::TextGlyphVertex vertex{};
        float size = std::max(1.0f, typography.font_size);
        vertex.min_x = origin_x;
        vertex.min_y = baseline_y - size;
        vertex.max_x = origin_x + size;
        vertex.max_y = baseline_y;
        glyph_vertices.push_back(vertex);
        update_geometry_bounds(geometry, vertex.min_x, vertex.min_y, vertex.max_x, vertex.max_y, 1.0f);
    }
    if (geometry.min_x == std::numeric_limits<float>::max()
        || geometry.min_y == std::numeric_limits<float>::max()) {
        return std::nullopt;
    }

    auto bucket = initialize_bucket(drawable_id,
                                    geometry.min_x,
                                    geometry.min_y,
                                    geometry.max_x,
                                    geometry.max_y,
                                    z_value);

    bucket.command_kinds.push_back(static_cast<std::uint32_t>(Scene::DrawCommandKind::TextGlyphs));
    Scene::TextGlyphsCommand command{};
    command.min_x = geometry.min_x;
    command.min_y = geometry.min_y;
    command.max_x = geometry.max_x;
    command.max_y = geometry.max_y;
    command.glyph_offset = static_cast<std::uint32_t>(0u);
    command.glyph_count = static_cast<std::uint32_t>(glyph_vertices.size());
    command.atlas_fingerprint = lane_fingerprint;
    command.font_size = typography.font_size;
    command.em_size = atlas->em_size;
    command.px_range = std::max(geometry.px_range, 1.0f);
    command.flags = (want_color_lane || active_lane == AtlasLane::Color)
                        ? Scene::kTextGlyphsFlagUsesColorAtlas
                        : 0u;
    command.color = color;

    bucket.command_payload.resize(sizeof(Scene::TextGlyphsCommand));
    std::memcpy(bucket.command_payload.data(), &command, sizeof(Scene::TextGlyphsCommand));
    bucket.command_counts[0] = 1;
    bucket.command_offsets[0] = 0;

    bucket.alpha_indices.push_back(0);
    bucket.opaque_indices.clear();
    bucket.glyph_vertices = std::move(glyph_vertices);

    bucket.authoring_map.push_back(Scene::DrawableAuthoringMapEntry{
        drawable_id,
        std::move(authoring_id),
        0,
        0});
    bucket.drawable_fingerprints.push_back(lane_fingerprint);

    Scene::FontAssetReference asset{};
    asset.drawable_id = drawable_id;
    asset.resource_root = typography.font_resource_root;
    asset.revision = typography.font_active_revision;
    asset.fingerprint = lane_fingerprint;
    asset.kind = (want_color_lane || active_lane == AtlasLane::Color)
                     ? Scene::FontAssetKind::Color
                     : Scene::FontAssetKind::Alpha;
    bucket.font_assets.push_back(std::move(asset));

    BuildResult result{
        .bucket = std::move(bucket),
        .width = geometry.max_x - geometry.min_x,
        .height = geometry.max_y - geometry.min_y,
    };
    result.font_family = typography.font_family;
    result.font_style = typography.font_style;
    result.font_weight = typography.font_weight;
    result.language = typography.language;
    result.direction = typography.direction;
    result.font_resource_root = typography.font_resource_root;
    result.font_revision = typography.font_active_revision;
    result.font_asset_fingerprint = lane_fingerprint;
    result.font_features = typography.font_features;
    result.fallback_families = typography.fallback_families;
    return result;
}

auto measure_text_width_shaped(std::string_view text,
                               Widgets::TypographyStyle const& typography) -> std::optional<float> {
    if (text.empty()) {
        return std::nullopt;
    }
    auto const& ctx = current_context();
    if (ctx.space == nullptr || ctx.manager == nullptr || ctx.app_root.empty()) {
        return std::nullopt;
    }
    auto run = ctx.manager->shape_text(SP::App::AppRootPathView{ctx.app_root}, text, typography);
    return run.total_advance;
}

} // namespace

ScopedShapingContext::ScopedShapingContext(SP::PathSpace& space, SP::App::AppRootPathView appRoot) {
    auto path_view = appRoot.getPath();
    if (path_view.empty()) {
        return;
    }

    if (!font_manager_enabled()) {
        return;
    }

    if (auto status = SP::UI::Runtime::Resources::Fonts::EnsureBuiltInPack(space, appRoot); !status) {
        auto description = SP::describeError(status.error());
        std::fprintf(stderr,
                     "PathSpace ScopedShapingContext: failed to ensure built-in fonts: %s\n",
                     description.c_str());
    }

    auto& manager = ensure_font_manager(space);

    auto& ctx = current_context();
    had_previous_ = (ctx.space != nullptr || ctx.manager != nullptr || !ctx.app_root.empty());
    previous_space_ = ctx.space;
    previous_manager_ = ctx.manager;
    previous_app_root_ = ctx.app_root;

    ctx.space = &space;
    ctx.manager = &manager;
    ctx.app_root = std::string(path_view);
    active_ = true;
}

ScopedShapingContext::~ScopedShapingContext() {
    if (!active_) {
        return;
    }

    auto& ctx = current_context();
    if (had_previous_) {
        ctx.space = previous_space_;
        ctx.manager = static_cast<SP::UI::FontManager*>(previous_manager_);
        ctx.app_root = std::move(previous_app_root_);
    } else {
        ctx = {};
    }
}

auto MeasureTextWidth(std::string_view text,
                      Widgets::TypographyStyle const& typography) -> float {
    if (auto prepared = prepare_typography_for_shaping(typography)) {
        if (auto shaped_width = measure_text_width_shaped(text, *prepared)) {
            return *shaped_width;
        }
    }
    return build_fallback_width(text, typography);
}

auto BuildTextBucket(std::string_view text,
                     float origin_x,
                     float baseline_y,
                     Widgets::TypographyStyle const& typography,
                     std::array<float, 4> color,
                     std::uint64_t drawable_id,
                     std::string authoring_id,
                     float z_value) -> std::optional<BuildResult> {
    if (text.empty() || is_whitespace_only(text)) {
        return std::nullopt;
    }
    if (auto prepared = prepare_typography_for_shaping(typography)) {
        if (auto shaped = build_text_bucket_shaped(text,
                                                   origin_x,
                                                   baseline_y,
                                                   *prepared,
                                                   color,
                                                   drawable_id,
                                                   authoring_id,
                                                   z_value)) {
            return shaped;
        }
        return build_fallback_bucket(text,
                                     origin_x,
                                     baseline_y,
                                     *prepared,
                                     color,
                                     drawable_id,
                                     std::move(authoring_id),
                                     z_value);
    }
    return build_fallback_bucket(text,
                                 origin_x,
                                 baseline_y,
                                 typography,
                                 color,
                                 drawable_id,
                                 std::move(authoring_id),
                                 z_value);
}

} // namespace SP::UI::Declarative::Text
