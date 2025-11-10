#include "SceneSnapshotBuilderDetail.hpp"

#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SP::UI::Scene {

namespace {

struct Fnv1a64 {
    std::uint64_t value = 1469598103934665603ull;

    void mix_bytes(void const* data, std::size_t size) {
        auto const* bytes = static_cast<unsigned char const*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value ^= static_cast<std::uint64_t>(bytes[i]);
            value *= 1099511628211ull;
        }
    }

    template <typename T>
    void mix_value(T const& v) {
        mix_bytes(&v, sizeof(v));
    }

    void mix_string(std::string const& s) {
        mix_bytes(s.data(), s.size());
        value ^= static_cast<std::uint64_t>(s.size());
        value *= 1099511628211ull;
    }
};

struct CommandPayloadLayout {
    std::vector<std::size_t> offsets;
    bool truncated = false;
};

auto compute_command_payload_layout(std::vector<std::uint32_t> const& kinds,
                                    std::vector<std::uint8_t> const& payload) -> CommandPayloadLayout {
    CommandPayloadLayout layout{};
    layout.offsets.reserve(kinds.size());
    std::size_t cursor = 0;
    for (auto kind_value : kinds) {
        auto kind = static_cast<DrawCommandKind>(kind_value);
        layout.offsets.push_back(cursor);
        auto payload_size = payload_size_bytes(kind);
        if (cursor + payload_size > payload.size()) {
            layout.truncated = true;
            cursor = payload.size();
        } else {
            cursor += payload_size;
        }
    }
    if (cursor != payload.size()) {
        layout.truncated = true;
    }
    return layout;
}

} // namespace

auto compute_drawable_fingerprints(DrawableBucketSnapshot const& bucket)
    -> Expected<std::vector<std::uint64_t>> {
    auto const drawable_count = bucket.drawable_ids.size();
    auto layout = compute_command_payload_layout(bucket.command_kinds, bucket.command_payload);
    std::vector<std::uint64_t> fingerprints(drawable_count, 0);

    auto clamp_payload_span = [&](std::size_t offset, std::size_t size)
        -> std::pair<void const*, std::size_t> {
        if (offset >= bucket.command_payload.size()) {
            return {nullptr, 0};
        }
        auto available = std::min(size, bucket.command_payload.size() - offset);
        return {bucket.command_payload.data() + offset, available};
    };

    auto mix_clip_chain = [&](Fnv1a64& h, std::int32_t head_index) {
        auto node_count = bucket.clip_nodes.size();
        std::int32_t index = head_index;
        std::size_t safety = 0;
        while (index >= 0 && static_cast<std::size_t>(index) < node_count && safety < node_count) {
            auto const& node = bucket.clip_nodes[static_cast<std::size_t>(index)];
            h.mix_value(static_cast<std::uint32_t>(node.type));
            h.mix_value(node.next);
            h.mix_value(node.rect.min_x);
            h.mix_value(node.rect.min_y);
            h.mix_value(node.rect.max_x);
            h.mix_value(node.rect.max_y);
            h.mix_value(node.path.command_offset);
            h.mix_value(node.path.command_count);
            index = node.next;
            ++safety;
        }
        if (safety >= node_count && node_count > 0) {
            h.mix_value(static_cast<std::uint32_t>(0xFFFFFFFF));
        }
    };

    auto mix_authoring_entry = [&](Fnv1a64& h, DrawableAuthoringMapEntry const& entry) {
        h.mix_value(entry.drawable_index_within_node);
        h.mix_value(entry.generation);
        if (!entry.authoring_node_id.empty()) {
            h.mix_string(entry.authoring_node_id);
        }
    };

    for (std::size_t i = 0; i < drawable_count; ++i) {
        Fnv1a64 hash{};
        if (i < bucket.world_transforms.size()) {
            auto const& transform = bucket.world_transforms[i];
            for (auto value : transform.elements) {
                hash.mix_value(value);
            }
        }
        if (i < bucket.bounds_spheres.size()) {
            auto const& sphere = bucket.bounds_spheres[i];
            for (auto value : sphere.center) {
                hash.mix_value(value);
            }
            hash.mix_value(sphere.radius);
        }
        if (bucket.bounds_boxes.size() == bucket.drawable_ids.size()) {
            auto const& box = bucket.bounds_boxes[i];
            for (auto value : box.min) {
                hash.mix_value(value);
            }
            for (auto value : box.max) {
                hash.mix_value(value);
            }
        }
        if (i < bucket.bounds_box_valid.size()) {
            hash.mix_value(bucket.bounds_box_valid[i]);
        }
        if (i < bucket.layers.size()) {
            hash.mix_value(bucket.layers[i]);
        }
        if (i < bucket.z_values.size()) {
            hash.mix_value(bucket.z_values[i]);
        }
        if (i < bucket.material_ids.size()) {
            hash.mix_value(bucket.material_ids[i]);
        }
        if (i < bucket.pipeline_flags.size()) {
            hash.mix_value(bucket.pipeline_flags[i]);
        }
        if (i < bucket.visibility.size()) {
            hash.mix_value(bucket.visibility[i]);
        }

        if (i < bucket.command_offsets.size() && i < bucket.command_counts.size()) {
            auto offset = bucket.command_offsets[i];
            auto count = bucket.command_counts[i];
            for (std::uint32_t c = 0; c < count; ++c) {
                auto command_index = static_cast<std::size_t>(offset) + c;
                if (command_index >= bucket.command_kinds.size()) {
                    hash.mix_value(static_cast<std::uint32_t>(0xFFFFFFFF));
                    break;
                }
                auto kind_value = bucket.command_kinds[command_index];
                auto kind = static_cast<DrawCommandKind>(kind_value);
                hash.mix_value(kind_value);
                std::size_t payload_size = payload_size_bytes(kind);
                std::size_t payload_offset = 0;
                if (command_index < layout.offsets.size()) {
                    payload_offset = layout.offsets[command_index];
                } else {
                    layout.truncated = true;
                    payload_offset = bucket.command_payload.size();
                }
                auto [ptr, available] = clamp_payload_span(payload_offset, payload_size);
                if (ptr && available > 0) {
                    hash.mix_bytes(ptr, available);
                    if (kind == DrawCommandKind::Stroke && available >= sizeof(StrokeCommand)) {
                        StrokeCommand stroke{};
                        std::memcpy(&stroke, ptr, sizeof(StrokeCommand));
                        hash.mix_value(stroke.thickness);
                        auto stroke_offset = static_cast<std::size_t>(stroke.point_offset);
                        auto stroke_count = static_cast<std::size_t>(stroke.point_count);
                        if (stroke_offset + stroke_count <= bucket.stroke_points.size()) {
                            auto begin = bucket.stroke_points.data() + stroke_offset;
                            auto end = begin + stroke_count;
                            for (auto it = begin; it != end; ++it) {
                                hash.mix_value(it->x);
                                hash.mix_value(it->y);
                            }
                        } else {
                            hash.mix_value(static_cast<std::uint32_t>(0xDEADBEEF));
                        }
                    }
                    if (kind == DrawCommandKind::TextGlyphs && available >= sizeof(TextGlyphsCommand)) {
                        TextGlyphsCommand glyphs{};
                        std::memcpy(&glyphs, ptr, sizeof(TextGlyphsCommand));
                        hash.mix_value(glyphs.atlas_fingerprint);
                        hash.mix_value(glyphs.flags);
                        auto glyph_offset = static_cast<std::size_t>(glyphs.glyph_offset);
                        auto glyph_count = static_cast<std::size_t>(glyphs.glyph_count);
                        if (glyph_offset + glyph_count <= bucket.glyph_vertices.size()) {
                            auto begin = bucket.glyph_vertices.data() + glyph_offset;
                            auto end = begin + glyph_count;
                            for (auto it = begin; it != end; ++it) {
                                hash.mix_value(it->min_x);
                                hash.mix_value(it->min_y);
                                hash.mix_value(it->max_x);
                                hash.mix_value(it->max_y);
                                hash.mix_value(it->u0);
                                hash.mix_value(it->v0);
                                hash.mix_value(it->u1);
                                hash.mix_value(it->v1);
                            }
                        } else {
                            hash.mix_value(static_cast<std::uint32_t>(0xBADCAFE));
                        }
                    }
                }
                if (available < payload_size) {
                    hash.mix_value(static_cast<std::uint32_t>(payload_size - available));
                }
            }
        }

        if (!bucket.clip_head_indices.empty() && i < bucket.clip_head_indices.size()) {
            hash.mix_value(bucket.clip_head_indices[i]);
            mix_clip_chain(hash, bucket.clip_head_indices[i]);
        }

        if (!bucket.authoring_map.empty() && i < bucket.authoring_map.size()) {
            mix_authoring_entry(hash, bucket.authoring_map[i]);
        }

        if (!bucket.font_assets.empty() && i < bucket.drawable_ids.size()) {
            auto drawable_id = bucket.drawable_ids[i];
            for (auto const& asset : bucket.font_assets) {
                if (asset.drawable_id == drawable_id) {
                    if (!asset.resource_root.empty()) {
                        hash.mix_string(asset.resource_root);
                    }
                    hash.mix_value(asset.revision);
                    hash.mix_value(asset.fingerprint);
                }
            }
        }

        if (layout.truncated) {
            hash.mix_value(static_cast<std::uint32_t>(0xAAAA5555));
        }

        fingerprints[i] = hash.value;
    }

    return fingerprints;
}

} // namespace SP::UI::Scene
