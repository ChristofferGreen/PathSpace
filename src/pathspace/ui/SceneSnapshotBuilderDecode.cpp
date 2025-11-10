#include "SceneSnapshotBuilderDetail.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace SP::UI::Scene {

auto SceneSnapshotBuilder::decode_bucket(PathSpace const& space,
                                         std::string const& revisionBase) -> Expected<DrawableBucketSnapshot> {
    auto read_bytes = [&](std::string const& path) -> Expected<std::vector<std::uint8_t>> {
        auto value = space.read<std::vector<std::uint8_t>>(path);
        if (!value) {
            return std::unexpected(value.error());
        }
        return *value;
    };

    auto drawablesBytes = read_bytes(revisionBase + "/bucket/drawables.bin");
    if (!drawablesBytes) return std::unexpected(drawablesBytes.error());
    auto drawablesDecoded = from_bytes<BucketDrawablesBinary>(*drawablesBytes);
    if (!drawablesDecoded) return std::unexpected(drawablesDecoded.error());

    std::vector<std::uint64_t> drawable_fingerprints;
    {
        auto fingerprintBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/fingerprints.bin");
        if (fingerprintBytes) {
            auto decoded = from_bytes<BucketFingerprintsBinary>(*fingerprintBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            drawable_fingerprints = std::move(decoded->drawable_fingerprints);
        } else {
            auto const& error = fingerprintBytes.error();
            if (error.code != Error::Code::NoObjectFound
                && error.code != Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    auto transformsBytes = read_bytes(revisionBase + "/bucket/transforms.bin");
    if (!transformsBytes) return std::unexpected(transformsBytes.error());
    auto transformsDecoded = from_bytes<BucketTransformsBinary>(*transformsBytes);
    if (!transformsDecoded) return std::unexpected(transformsDecoded.error());

    auto boundsBytes = read_bytes(revisionBase + "/bucket/bounds.bin");
    if (!boundsBytes) return std::unexpected(boundsBytes.error());
    auto boundsDecoded = from_bytes<BucketBoundsBinary>(*boundsBytes);
    if (!boundsDecoded) return std::unexpected(boundsDecoded.error());

    auto stateBytes = read_bytes(revisionBase + "/bucket/state.bin");
    if (!stateBytes) return std::unexpected(stateBytes.error());
    auto stateDecoded = from_bytes<BucketStateBinary>(*stateBytes);
    if (!stateDecoded) return std::unexpected(stateDecoded.error());

    auto cmdBytes = read_bytes(revisionBase + "/bucket/cmd-buffer.bin");
    if (!cmdBytes) return std::unexpected(cmdBytes.error());
    auto cmdDecoded = from_bytes<BucketCommandBufferBinary>(*cmdBytes);
    if (!cmdDecoded) return std::unexpected(cmdDecoded.error());

    std::vector<StrokePoint> stroke_points;
    {
        auto strokeBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/strokes.bin");
        if (strokeBytes) {
            auto decoded = from_bytes<BucketStrokePointsBinary>(*strokeBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            stroke_points = std::move(decoded->stroke_points);
        } else {
            auto const& error = strokeBytes.error();
            if (error.code != Error::Code::NoObjectFound
                && error.code != Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    auto opaque = space.read<std::vector<std::uint32_t>>(revisionBase + "/bucket/indices/opaque.bin");
    if (!opaque) return std::unexpected(opaque.error());
    auto alpha = space.read<std::vector<std::uint32_t>>(revisionBase + "/bucket/indices/alpha.bin");
    if (!alpha) return std::unexpected(alpha.error());

    auto summary = space.read<SnapshotSummary>(revisionBase + std::string(kBucketSummary));
    if (!summary) return std::unexpected(summary.error());

    std::vector<std::int32_t> clip_head_indices;
    {
        auto clipHeadsBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/clip-heads.bin");
        if (clipHeadsBytes) {
            auto decoded = from_bytes<BucketClipHeadsBinary>(*clipHeadsBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            clip_head_indices = std::move(decoded->clip_head_indices);
        } else {
            auto const& error = clipHeadsBytes.error();
            if (error.code == Error::Code::NoObjectFound
                || error.code == Error::Code::NoSuchPath) {
                clip_head_indices.assign(drawablesDecoded->drawable_ids.size(), -1);
            } else {
                return std::unexpected(error);
            }
        }
    }

    std::vector<ClipNode> clip_nodes;
    {
        auto clipNodesBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/clip-nodes.bin");
        if (clipNodesBytes) {
            auto decoded = from_bytes<BucketClipNodesBinary>(*clipNodesBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            clip_nodes = std::move(decoded->clip_nodes);
        } else {
            auto const& error = clipNodesBytes.error();
            if (error.code == Error::Code::NoObjectFound
                || error.code == Error::Code::NoSuchPath) {
                clip_nodes.clear();
            } else {
                return std::unexpected(error);
            }
        }
    }

    std::vector<DrawableAuthoringMapEntry> authoring_map;
    {
        auto authoringBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/authoring-map.bin");
        if (authoringBytes) {
            auto decoded = from_bytes<BucketAuthoringMapBinary>(*authoringBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            authoring_map = std::move(decoded->authoring_map);
        } else {
            auto const& error = authoringBytes.error();
            if (error.code == Error::Code::NoObjectFound
                || error.code == Error::Code::NoSuchPath) {
                authoring_map.assign(drawablesDecoded->drawable_ids.size(), DrawableAuthoringMapEntry{});
                for (std::size_t i = 0; i < authoring_map.size(); ++i) {
                    authoring_map[i].drawable_id = drawablesDecoded->drawable_ids[i];
                }
            } else {
                return std::unexpected(error);
            }
        }
    }

    std::vector<FontAssetReference> font_assets;
    {
        auto fontBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/font-assets.bin");
        if (fontBytes) {
            auto decoded = from_bytes<BucketFontAssetsBinary>(*fontBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            font_assets = std::move(decoded->font_assets);
        } else {
            auto const& error = fontBytes.error();
            if (error.code != Error::Code::NoObjectFound
                && error.code != Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    std::vector<TextGlyphVertex> glyph_vertices;
    {
        auto glyphBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/glyph-vertices.bin");
        if (glyphBytes) {
            auto decoded = from_bytes<BucketGlyphVerticesBinary>(*glyphBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            glyph_vertices = std::move(decoded->glyph_vertices);
        } else {
            auto const& error = glyphBytes.error();
            if (error.code != Error::Code::NoObjectFound
                && error.code != Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids     = std::move(drawablesDecoded->drawable_ids);
    bucket.world_transforms = std::move(transformsDecoded->world_transforms);
    bucket.bounds_spheres   = std::move(boundsDecoded->spheres);
    bucket.bounds_boxes     = std::move(boundsDecoded->boxes);
    bucket.bounds_box_valid = std::move(boundsDecoded->box_valid);
    bucket.layers           = std::move(stateDecoded->layers);
    bucket.z_values         = std::move(stateDecoded->z_values);
    bucket.material_ids     = std::move(stateDecoded->material_ids);
    bucket.pipeline_flags   = std::move(stateDecoded->pipeline_flags);
    bucket.visibility       = std::move(stateDecoded->visibility);
    bucket.command_offsets  = std::move(drawablesDecoded->command_offsets);
    bucket.command_counts   = std::move(drawablesDecoded->command_counts);
    bucket.opaque_indices   = *opaque;
    bucket.alpha_indices    = *alpha;
    bucket.command_kinds    = std::move(cmdDecoded->command_kinds);
    bucket.command_payload  = std::move(cmdDecoded->command_payload);
    bucket.stroke_points    = std::move(stroke_points);
    bucket.clip_head_indices = std::move(clip_head_indices);
    bucket.clip_nodes        = std::move(clip_nodes);
    bucket.authoring_map     = std::move(authoring_map);
    bucket.drawable_fingerprints = std::move(drawable_fingerprints);
    bucket.font_assets       = std::move(font_assets);
    bucket.glyph_vertices    = std::move(glyph_vertices);

    bucket.layer_indices.reserve(summary->layer_ids.size());
    for (auto layerId : summary->layer_ids) {
        auto layerPath = revisionBase + "/bucket/indices/layer/" + std::to_string(layerId) + ".bin";
        auto indices = space.read<std::vector<std::uint32_t>>(layerPath);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        bucket.layer_indices.push_back(LayerIndices{
            .layer = layerId,
            .indices = std::move(*indices),
        });
    }

    if (bucket.drawable_fingerprints.empty() && !bucket.drawable_ids.empty()) {
        auto computed = compute_drawable_fingerprints(bucket);
        if (!computed) {
            return std::unexpected(computed.error());
        }
        bucket.drawable_fingerprints = std::move(*computed);
    }

    return bucket;
}

auto SceneSnapshotBuilder::decode_metadata(std::span<std::byte const> bytes)
    -> Expected<SnapshotMetadata> {
    std::vector<std::uint8_t> buffer(bytes.size());
    std::transform(bytes.begin(), bytes.end(), buffer.begin(), [](std::byte b) {
        return static_cast<std::uint8_t>(b);
    });
    auto decoded = from_bytes<EncodedSnapshotMetadata>(buffer);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    SnapshotMetadata meta{};
    meta.author = std::move(decoded->author);
    meta.tool_version = std::move(decoded->tool_version);
    meta.created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{decoded->created_at_ms}};
    meta.drawable_count = static_cast<std::size_t>(decoded->drawable_count);
    meta.command_count = static_cast<std::size_t>(decoded->command_count);
    meta.fingerprint_digests = std::move(decoded->fingerprint_digests);
    return meta;
}

} // namespace SP::UI::Scene
