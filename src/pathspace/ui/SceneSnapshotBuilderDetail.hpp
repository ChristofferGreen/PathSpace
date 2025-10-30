#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include "alpaca/alpaca.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Scene {

constexpr std::string_view kBucketSummary = "/bucket/summary";

struct EncodedSnapshotMetadata {
    std::string author;
    std::string tool_version;
    std::int64_t created_at_ms = 0;
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
    std::vector<std::string> fingerprint_digests;
};

struct BucketDrawablesBinary {
    std::vector<std::uint64_t> drawable_ids;
    std::vector<std::uint32_t> command_offsets;
    std::vector<std::uint32_t> command_counts;
};

struct BucketTransformsBinary {
    std::vector<Transform> world_transforms;
};

struct BucketBoundsBinary {
    std::vector<BoundingSphere> spheres;
    std::vector<BoundingBox>    boxes;
    std::vector<std::uint8_t>   box_valid;
};

struct BucketStateBinary {
    std::vector<std::uint32_t> layers;
    std::vector<float>         z_values;
    std::vector<std::uint32_t> material_ids;
    std::vector<std::uint32_t> pipeline_flags;
    std::vector<std::uint8_t>  visibility;
};

struct BucketCommandBufferBinary {
    std::vector<std::uint32_t> command_kinds;
    std::vector<std::uint8_t>  command_payload;
};

struct BucketStrokePointsBinary {
    std::vector<StrokePoint> stroke_points;
};

struct BucketClipHeadsBinary {
    std::vector<std::int32_t> clip_head_indices;
};

struct BucketClipNodesBinary {
    std::vector<ClipNode> clip_nodes;
};

struct BucketAuthoringMapBinary {
    std::vector<DrawableAuthoringMapEntry> authoring_map;
};

struct BucketFingerprintsBinary {
    std::vector<std::uint64_t> drawable_fingerprints;
};

struct BucketFontAssetsBinary {
    std::vector<FontAssetReference> font_assets;
};

struct SnapshotSummary {
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
    std::vector<std::uint32_t> layer_ids;
    std::uint64_t fingerprint_count = 0;
};

inline auto make_error(std::string message,
                       Error::Code code = Error::Code::UnknownError) -> Error {
    return Error{code, std::move(message)};
}

template <typename T>
inline auto to_bytes(T const& obj) -> Expected<std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> buffer;
    try {
        alpaca::serialize(obj, buffer);
    } catch (std::exception const& ex) {
        return std::unexpected(make_error("serialization failed: " + std::string(ex.what()),
                                          Error::Code::SerializationFunctionMissing));
    } catch (...) {
        return std::unexpected(make_error("serialization failed: unknown exception",
                                          Error::Code::SerializationFunctionMissing));
    }
    return buffer;
}

template <typename T>
inline auto from_bytes(std::vector<std::uint8_t> const& buffer) -> Expected<T> {
    std::error_code ec;
    auto            decoded = alpaca::deserialize<T>(buffer, ec);
    if (ec) {
        return std::unexpected(make_error("deserialization failed: " + ec.message(),
                                          Error::Code::UnserializableType));
    }
    return decoded;
}

template <typename T>
inline auto drain_queue(PathSpace& space, std::string const& path) -> Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == Error::Code::NoObjectFound
            || error.code == Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
inline auto replace_single(PathSpace& space,
                           std::string const& path,
                           T const& value) -> Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

auto compute_drawable_fingerprints(DrawableBucketSnapshot const& bucket)
    -> Expected<std::vector<std::uint64_t>>;

} // namespace SP::UI::Scene
