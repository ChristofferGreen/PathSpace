#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "TextGlyphFallback.hpp"

namespace SP::UI::Builders::Text {

namespace {

namespace Scene = SP::UI::Scene;

constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
constexpr std::uint64_t kFNVPrime = 1099511628211ull;

auto identity_transform() -> Scene::Transform {
    Scene::Transform transform{};
    for (int i = 0; i < 16; ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto glyph_scale(Widgets::TypographyStyle const& typography) -> float {
    return std::max(0.1f, typography.font_size / static_cast<float>(TextFallback::kGlyphRows));
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

} // namespace

auto MeasureTextWidth(std::string_view text,
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

auto BuildTextBucket(std::string_view text,
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

    DrawableBucketSnapshot bucket{};
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
    bucket.command_counts.push_back(static_cast<std::uint32_t>(commands.size()));
    bucket.opaque_indices.push_back(0);
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices.push_back(-1);

    bucket.command_kinds.resize(commands.size(),
                                static_cast<std::uint32_t>(Scene::DrawCommandKind::Rect));
    bucket.command_payload.resize(commands.size() * sizeof(Scene::RectCommand));
    auto* payload = bucket.command_payload.data();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        std::memcpy(payload + i * sizeof(Scene::RectCommand),
                    &commands[i],
                    sizeof(Scene::RectCommand));
    }

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

} // namespace SP::UI::Builders::Text
