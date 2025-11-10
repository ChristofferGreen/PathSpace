#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/FontAtlasCache.hpp>
#include <pathspace/ui/FontManager.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "TextGlyphFallback.hpp"

namespace SP::UI::Builders::Text {

namespace {

namespace TextFallback = SP::UI::TextFallback;

namespace Scene = SP::UI::Scene;

constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;

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

auto shaped_context_available(Widgets::TypographyStyle const& typography) -> bool {
    auto const& ctx = current_context();
    if (ctx.space == nullptr || ctx.manager == nullptr) {
        return false;
    }
    if (ctx.app_root.empty()) {
        return false;
    }
    if (typography.font_resource_root.empty() || typography.font_active_revision == 0) {
        return false;
    }
    return true;
}

auto load_font_atlas(SP::PathSpace& space,
                     Widgets::TypographyStyle const& typography,
                     std::uint64_t fingerprint)
    -> std::optional<std::shared_ptr<SP::UI::FontAtlasData const>> {
    auto atlas_path = typography.font_resource_root + "/builds/"
                    + format_revision(typography.font_active_revision) + "/atlas.bin";
    auto atlas = atlas_cache().load(space, atlas_path, fingerprint);
    if (!atlas) {
        return std::nullopt;
    }
    return *atlas;
}

auto build_text_bucket_shaped(std::string_view text,
                              float origin_x,
                              float baseline_y,
                              Widgets::TypographyStyle const& typography,
                              std::array<float, 4> color,
                              std::uint64_t drawable_id,
                              std::string authoring_id,
                              float z_value) -> std::optional<BuildResult> {
    if (!shaped_context_available(typography) || text.empty()) {
        return std::nullopt;
    }

    auto& ctx = current_context();
    auto app_root_view = SP::App::AppRootPathView{ctx.app_root};
    auto shaped_run = ctx.manager->shape_text(app_root_view, text, typography);
    if (shaped_run.glyphs.empty()) {
        return std::nullopt;
    }

    auto fingerprint = typography.font_asset_fingerprint != 0
                           ? typography.font_asset_fingerprint
                           : compute_font_fingerprint(typography);
    if (fingerprint == 0) {
        fingerprint = drawable_id;
    }

    auto atlas_loaded = load_font_atlas(*ctx.space, typography, fingerprint);
    if (!atlas_loaded) {
        return std::nullopt;
    }
    auto atlas = *atlas_loaded;
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

    if (glyph_vertices.empty()
        || geometry.min_x == std::numeric_limits<float>::max()
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
    command.atlas_fingerprint = fingerprint;
    command.font_size = typography.font_size;
    command.em_size = atlas->em_size;
    command.px_range = std::max(geometry.px_range, 1.0f);
    command.flags = (atlas->format == SP::UI::FontAtlasFormat::Rgba8)
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
    bucket.drawable_fingerprints.push_back(fingerprint);

    Scene::FontAssetReference asset{};
    asset.drawable_id = drawable_id;
    asset.resource_root = typography.font_resource_root;
    asset.revision = typography.font_active_revision;
    asset.fingerprint = fingerprint;
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
    result.font_asset_fingerprint = fingerprint;
    result.font_features = typography.font_features;
    result.fallback_families = typography.fallback_families;
    return result;
}

auto measure_text_width_shaped(std::string_view text,
                               Widgets::TypographyStyle const& typography) -> std::optional<float> {
    if (!shaped_context_available(typography) || text.empty()) {
        return std::nullopt;
    }
    auto const& ctx = current_context();
    auto run = ctx.manager->shape_text(SP::App::AppRootPathView{ctx.app_root}, text, typography);
    return run.total_advance;
}

} // namespace

ScopedShapingContext::ScopedShapingContext(SP::PathSpace& space, SP::App::AppRootPathView appRoot) {
    auto path_view = appRoot.getPath();
    if (path_view.empty()) {
        return;
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
    if (auto shaped_width = measure_text_width_shaped(text, typography)) {
        return *shaped_width;
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
    if (auto shaped = build_text_bucket_shaped(text,
                                               origin_x,
                                               baseline_y,
                                               typography,
                                               color,
                                               drawable_id,
                                               authoring_id,
                                               z_value)) {
        return shaped;
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

} // namespace SP::UI::Builders::Text
