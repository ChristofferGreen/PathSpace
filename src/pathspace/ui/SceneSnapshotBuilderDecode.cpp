#include "SceneSnapshotBuilderDetail.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

namespace SP::UI::Scene {

namespace {

template <typename UInt>
auto read_varuint(std::vector<std::uint8_t> const& bytes,
                  std::size_t& index,
                  std::size_t end) -> Expected<UInt> {
    constexpr auto max_bits = std::numeric_limits<UInt>::digits;
    UInt value = 0;
    unsigned int shift = 0;
    while (index < end) {
        auto byte = bytes[index++];
        auto chunk = static_cast<UInt>(byte & 0x7Fu);
        if (shift >= max_bits && chunk != 0) {
            return std::unexpected(make_error("varuint overflow while decoding bucket",
                                              Error::Code::UnserializableType));
        }
        value |= static_cast<UInt>(chunk << shift);
        if ((byte & 0x80u) == 0) {
            return value;
        }
        shift += 7;
        if (shift > max_bits + 7) {
            return std::unexpected(make_error("varuint exceeds target width",
                                              Error::Code::UnserializableType));
        }
    }
    return std::unexpected(make_error("unexpected end of data while decoding bucket",
                                      Error::Code::UnserializableType));
}

template <typename UInt>
auto decode_varint_vector(std::vector<std::uint8_t> const& bytes,
                          std::size_t& index,
                          std::size_t end,
                          std::vector<UInt>& output) -> Expected<void> {
    auto length = read_varuint<std::size_t>(bytes, index, end);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (index > end) {
        return std::unexpected(make_error("bucket buffer truncated",
                                          Error::Code::UnserializableType));
    }
    output.clear();
    output.reserve(*length);
    for (std::size_t i = 0; i < *length; ++i) {
        auto value = read_varuint<UInt>(bytes, index, end);
        if (!value) {
            return std::unexpected(value.error());
        }
        output.push_back(*value);
    }
    return {};
}

auto decode_drawables_binary_varint(std::vector<std::uint8_t> const& bytes)
    -> Expected<BucketDrawablesBinary> {
    BucketDrawablesBinary decoded{};
    std::size_t index = 0;
    auto end = bytes.size();

    if (auto status = decode_varint_vector<std::uint64_t>(bytes, index, end, decoded.drawable_ids); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = decode_varint_vector<std::uint32_t>(bytes, index, end, decoded.command_offsets); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = decode_varint_vector<std::uint32_t>(bytes, index, end, decoded.command_counts); !status) {
        return std::unexpected(status.error());
    }
    if (index != end) {
        return std::unexpected(make_error("unexpected trailing data in drawables bucket",
                                          Error::Code::UnserializableType));
    }
    return decoded;
}

template <typename T>
auto decode_bucket_section(std::vector<std::uint8_t> const& bytes) -> Expected<T> {
    if (auto enveloped = decode_bucket_envelope_as<T>(bytes); enveloped) {
        return enveloped;
    } else if (enveloped.error().code != Error::Code::UnserializableType) {
        return std::unexpected(enveloped.error());
    }
    return from_bytes<T>(bytes);
}

} // namespace

auto SceneSnapshotBuilder::decode_bucket(PathSpace const& space,
                                         std::string const& revisionBase) -> Expected<DrawableBucketSnapshot> {
    auto annotate_error = [](SP::Error error, std::string const& path) -> SP::Error {
        if (error.message && !error.message->empty()) {
            error.message = path + ": " + *error.message;
        } else {
            error.message = path;
        }
        return error;
    };

    auto read_bytes = [&](std::string const& path) -> Expected<std::vector<std::uint8_t>> {
        auto value = space.read<std::vector<std::uint8_t>>(path);
        if (!value) {
            return std::unexpected(annotate_error(value.error(), path));
        }
        return *value;
    };

    auto drawablesBytes = read_bytes(revisionBase + "/bucket/drawables.bin");
    if (!drawablesBytes) return std::unexpected(drawablesBytes.error());
    BucketDrawablesBinary drawablesBinary{};
    auto decode_drawables = [&](std::vector<std::uint8_t> const& bytes)
        -> Expected<BucketDrawablesBinary> {
        auto enveloped = decode_bucket_envelope_as<BucketDrawablesBinary>(bytes);
        if (enveloped) {
            return enveloped;
        }
        auto const& envelope_error = enveloped.error();
        if (envelope_error.code != Error::Code::UnserializableType) {
            return std::unexpected(envelope_error);
        }
        if (auto decoded = from_bytes<BucketDrawablesBinary>(bytes); decoded) {
            return decoded;
        } else {
            auto fallback = decode_drawables_binary_varint(bytes);
            if (!fallback) {
                return std::unexpected(fallback.error());
            }
            return fallback;
        }
    };

    if (auto decoded = decode_drawables(*drawablesBytes); decoded) {
        drawablesBinary = std::move(*decoded);
    } else {
        return std::unexpected(annotate_error(decoded.error(),
                                              revisionBase + "/bucket/drawables.bin"));
    }

    std::vector<std::uint64_t> drawable_fingerprints;
    {
        auto fingerprintBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/fingerprints.bin");
        if (fingerprintBytes) {
            auto decoded = decode_bucket_section<BucketFingerprintsBinary>(*fingerprintBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/fingerprints.bin"));
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
    auto transformsDecoded = decode_bucket_section<BucketTransformsBinary>(*transformsBytes);
    if (!transformsDecoded) return std::unexpected(annotate_error(transformsDecoded.error(), revisionBase + "/bucket/transforms.bin"));

    auto boundsBytes = read_bytes(revisionBase + "/bucket/bounds.bin");
    if (!boundsBytes) return std::unexpected(boundsBytes.error());
    auto boundsDecoded = decode_bucket_section<BucketBoundsBinary>(*boundsBytes);
    if (!boundsDecoded) return std::unexpected(annotate_error(boundsDecoded.error(), revisionBase + "/bucket/bounds.bin"));

    auto stateBytes = read_bytes(revisionBase + "/bucket/state.bin");
    if (!stateBytes) return std::unexpected(stateBytes.error());
    auto stateDecoded = decode_bucket_section<BucketStateBinary>(*stateBytes);
    if (!stateDecoded) return std::unexpected(annotate_error(stateDecoded.error(), revisionBase + "/bucket/state.bin"));

    auto cmdBytes = read_bytes(revisionBase + "/bucket/cmd-buffer.bin");
    if (!cmdBytes) return std::unexpected(cmdBytes.error());
    auto cmdDecoded = decode_bucket_section<BucketCommandBufferBinary>(*cmdBytes);
    if (!cmdDecoded) return std::unexpected(annotate_error(cmdDecoded.error(), revisionBase + "/bucket/cmd-buffer.bin"));

    std::vector<StrokePoint> stroke_points;
    {
        auto strokeBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/strokes.bin");
        if (strokeBytes) {
            auto decoded = decode_bucket_section<BucketStrokePointsBinary>(*strokeBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/strokes.bin"));
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

    auto summaryPath = revisionBase + std::string(kBucketSummary);
    auto summary = space.read<SnapshotSummary>(summaryPath);
    if (!summary) return std::unexpected(annotate_error(summary.error(), summaryPath));

    std::vector<std::int32_t> clip_head_indices;
    {
        auto clipHeadsBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/clip-heads.bin");
        if (clipHeadsBytes) {
            auto decoded = decode_bucket_section<BucketClipHeadsBinary>(*clipHeadsBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/clip-heads.bin"));
            }
            clip_head_indices = std::move(decoded->clip_head_indices);
        } else {
            auto const& error = clipHeadsBytes.error();
            if (error.code == Error::Code::NoObjectFound
                || error.code == Error::Code::NoSuchPath) {
                clip_head_indices.assign(drawablesBinary.drawable_ids.size(), -1);
            } else {
                return std::unexpected(error);
            }
        }
    }

    std::vector<ClipNode> clip_nodes;
    {
        auto clipNodesBytes = space.read<std::vector<std::uint8_t>>(revisionBase + "/bucket/clip-nodes.bin");
        if (clipNodesBytes) {
            auto decoded = decode_bucket_section<BucketClipNodesBinary>(*clipNodesBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/clip-nodes.bin"));
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
            auto decoded = decode_bucket_section<BucketAuthoringMapBinary>(*authoringBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/authoring-map.bin"));
            }
            authoring_map = std::move(decoded->authoring_map);
        } else {
            auto const& error = authoringBytes.error();
            if (error.code == Error::Code::NoObjectFound
                || error.code == Error::Code::NoSuchPath) {
                authoring_map.assign(drawablesBinary.drawable_ids.size(), DrawableAuthoringMapEntry{});
                for (std::size_t i = 0; i < authoring_map.size(); ++i) {
                    authoring_map[i].drawable_id = drawablesBinary.drawable_ids[i];
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
            auto decoded = decode_font_assets(*fontBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/font-assets.bin"));
            }
            font_assets = std::move(*decoded);
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
            auto decoded = decode_bucket_section<BucketGlyphVerticesBinary>(*glyphBytes);
            if (!decoded) {
                return std::unexpected(annotate_error(decoded.error(), revisionBase + "/bucket/glyph-vertices.bin"));
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
    bucket.drawable_ids     = std::move(drawablesBinary.drawable_ids);
    bucket.world_transforms = std::move(transformsDecoded->world_transforms);
    bucket.bounds_spheres   = std::move(boundsDecoded->spheres);
    bucket.bounds_boxes     = std::move(boundsDecoded->boxes);
    bucket.bounds_box_valid = std::move(boundsDecoded->box_valid);
    bucket.layers           = std::move(stateDecoded->layers);
    bucket.z_values         = std::move(stateDecoded->z_values);
    bucket.material_ids     = std::move(stateDecoded->material_ids);
    bucket.pipeline_flags   = std::move(stateDecoded->pipeline_flags);
    bucket.visibility       = std::move(stateDecoded->visibility);
    bucket.command_offsets  = std::move(drawablesBinary.command_offsets);
    bucket.command_counts   = std::move(drawablesBinary.command_counts);
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
