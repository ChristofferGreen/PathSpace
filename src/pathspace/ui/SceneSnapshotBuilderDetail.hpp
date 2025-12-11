#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include "alpaca/alpaca.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Scene {

constexpr std::string_view kBucketSummary = "/bucket/summary";

inline constexpr std::array<std::uint8_t, 4> kBucketMagic{ 'D', 'B', 'K', 'T' };
inline constexpr std::uint32_t               kBucketBinaryVersion = 1;
inline constexpr std::size_t                 kBucketHeaderSize = 32;

enum class BucketEndianness : std::uint8_t {
    Little = 0,
    Big = 1,
};

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

inline auto make_error(std::string message,
                       Error::Code code = Error::Code::UnknownError) -> Error;

template <typename T>
inline auto to_bytes(T const& obj) -> Expected<std::vector<std::uint8_t>>;

template <typename T>
inline auto from_bytes(std::vector<std::uint8_t> const& buffer) -> Expected<T>;

inline auto encode_bucket_envelope(std::vector<std::uint8_t> const& payload)
    -> Expected<std::vector<std::uint8_t>>;

template <typename T>
inline auto encode_bucket_envelope(T const& obj) -> Expected<std::vector<std::uint8_t>>;

inline auto decode_bucket_envelope(std::vector<std::uint8_t> const& buffer)
    -> Expected<std::vector<std::uint8_t>>;

template <typename T>
inline auto decode_bucket_envelope_as(std::vector<std::uint8_t> const& buffer) -> Expected<T>;

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

struct FontAssetReferenceBinaryV1 {
    std::uint64_t drawable_id = 0;
    std::string   resource_root;
    std::uint64_t revision = 0;
    std::uint64_t fingerprint = 0;
};

struct BucketFontAssetsBinaryV1 {
    std::vector<FontAssetReferenceBinaryV1> font_assets;
};

struct FontAssetReferenceBinaryV2 {
    std::uint64_t drawable_id = 0;
    std::string   resource_root;
    std::uint64_t revision = 0;
    std::uint64_t fingerprint = 0;
    std::uint8_t  kind = static_cast<std::uint8_t>(FontAssetKind::Alpha);
};

struct BucketFontAssetsBinaryV2 {
    std::uint32_t                            version = 2;
    std::vector<FontAssetReferenceBinaryV2> font_assets;
};

inline constexpr std::uint32_t kBucketFontAssetsBinaryVersion = 2;

inline auto encode_font_assets(std::vector<FontAssetReference> const& assets)
    -> Expected<std::vector<std::uint8_t>> {
    BucketFontAssetsBinaryV2 binary{};
    binary.version = kBucketFontAssetsBinaryVersion;
    binary.font_assets.reserve(assets.size());
    for (auto const& asset : assets) {
        FontAssetReferenceBinaryV2 entry{};
        entry.drawable_id = asset.drawable_id;
        entry.resource_root = asset.resource_root;
        entry.revision = asset.revision;
        entry.fingerprint = asset.fingerprint;
        entry.kind = static_cast<std::uint8_t>(asset.kind);
        binary.font_assets.push_back(std::move(entry));
    }
    auto payload = to_bytes(binary);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_bucket_envelope(*payload);
}

inline auto decode_font_assets(std::vector<std::uint8_t> const& bytes)
    -> Expected<std::vector<FontAssetReference>> {
    std::vector<std::uint8_t> payload = bytes;
    if (auto enveloped = decode_bucket_envelope(bytes)) {
        payload = std::move(*enveloped);
    } else if (enveloped.error().code != Error::Code::UnserializableType) {
        return std::unexpected(enveloped.error());
    }

    auto decoded_v2 = from_bytes<BucketFontAssetsBinaryV2>(payload);
    if (decoded_v2) {
        if (decoded_v2->version != kBucketFontAssetsBinaryVersion) {
            return std::unexpected(make_error("unsupported font asset binary version",
                                              Error::Code::UnserializableType));
        }
        std::vector<FontAssetReference> assets;
        assets.reserve(decoded_v2->font_assets.size());
        for (auto const& entry : decoded_v2->font_assets) {
            FontAssetReference asset{};
            asset.drawable_id = entry.drawable_id;
            asset.resource_root = entry.resource_root;
            asset.revision = entry.revision;
            asset.fingerprint = entry.fingerprint;
            asset.kind = static_cast<FontAssetKind>(entry.kind);
            assets.push_back(std::move(asset));
        }
        return assets;
    }

    auto const& v2_error = decoded_v2.error();
    if (v2_error.code != Error::Code::UnserializableType) {
        return std::unexpected(v2_error);
    }

    auto decoded_v1 = from_bytes<BucketFontAssetsBinaryV1>(payload);
    if (!decoded_v1) {
        return std::unexpected(decoded_v1.error());
    }
    std::vector<FontAssetReference> assets;
    assets.reserve(decoded_v1->font_assets.size());
    for (auto const& entry : decoded_v1->font_assets) {
        FontAssetReference asset{};
        asset.drawable_id = entry.drawable_id;
        asset.resource_root = entry.resource_root;
        asset.revision = entry.revision;
        asset.fingerprint = entry.fingerprint;
        asset.kind = FontAssetKind::Alpha;
        assets.push_back(std::move(asset));
    }
    return assets;
}

struct BucketGlyphVerticesBinary {
    std::vector<TextGlyphVertex> glyph_vertices;
};

struct SnapshotSummary {
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
    std::vector<std::uint32_t> layer_ids;
    std::uint64_t fingerprint_count = 0;
};

inline auto make_error(std::string message,
                       Error::Code code) -> Error {
    return Error{code, std::move(message)};
}

template <typename T>
inline auto to_bytes(T const& obj) -> Expected<std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> buffer;
    try {
        std::size_t byte_index = 0;
        alpaca::serialize<alpaca::options::fixed_length_encoding>(obj, buffer, byte_index);
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
    auto decoded = alpaca::deserialize<alpaca::options::fixed_length_encoding, T>(buffer, ec);
    if (ec) {
        return std::unexpected(make_error("deserialization failed: " + ec.message(),
                                          Error::Code::UnserializableType));
    }
    return decoded;
}

inline void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

inline void append_le64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_le32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    append_le32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFull));
}

inline auto read_le32(std::span<std::uint8_t const> bytes, std::size_t offset)
    -> Expected<std::uint32_t> {
    if (offset + 4 > bytes.size()) {
        return std::unexpected(make_error("bucket header truncated",
                                          Error::Code::InvalidType));
    }
    auto v0 = static_cast<std::uint32_t>(bytes[offset]);
    auto v1 = static_cast<std::uint32_t>(bytes[offset + 1]) << 8u;
    auto v2 = static_cast<std::uint32_t>(bytes[offset + 2]) << 16u;
    auto v3 = static_cast<std::uint32_t>(bytes[offset + 3]) << 24u;
    return v0 | v1 | v2 | v3;
}

inline auto read_le64(std::span<std::uint8_t const> bytes, std::size_t offset)
    -> Expected<std::uint64_t> {
    if (offset + 8 > bytes.size()) {
        return std::unexpected(make_error("bucket header truncated",
                                          Error::Code::InvalidType));
    }
    auto lower = read_le32(bytes, offset);
    if (!lower) {
        return std::unexpected(lower.error());
    }
    auto upper = read_le32(bytes, offset + 4);
    if (!upper) {
        return std::unexpected(upper.error());
    }
    return (static_cast<std::uint64_t>(*upper) << 32u) | static_cast<std::uint64_t>(*lower);
}

inline auto fnv1a64(std::span<std::uint8_t const> bytes) -> std::uint64_t {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    std::uint64_t hash = kOffsetBasis;
    for (auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kPrime;
    }
    return hash;
}

inline auto encode_bucket_envelope(std::vector<std::uint8_t> const& payload)
    -> Expected<std::vector<std::uint8_t>> {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_error("bucket payload too large",
                                          Error::Code::CapacityExceeded));
    }

    auto payload_size = static_cast<std::uint32_t>(payload.size());
    auto padding = static_cast<std::size_t>((8u - (payload_size % 8u)) % 8u);
    auto checksum = fnv1a64(payload);

    std::vector<std::uint8_t> output;
    output.reserve(kBucketHeaderSize + payload.size() + padding);

    output.insert(output.end(), kBucketMagic.begin(), kBucketMagic.end());
    append_le32(output, kBucketBinaryVersion);
    output.push_back(static_cast<std::uint8_t>(BucketEndianness::Little));
    output.insert(output.end(), 3, 0); // reserved/padding for alignment
    append_le32(output, payload_size);
    append_le64(output, checksum);
    append_le64(output, 0);
    output.insert(output.end(), payload.begin(), payload.end());
    output.insert(output.end(), padding, static_cast<std::uint8_t>(0));
    return output;
}

template <typename T>
inline auto encode_bucket_envelope(T const& obj) -> Expected<std::vector<std::uint8_t>> {
    auto payload = to_bytes(obj);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_bucket_envelope(*payload);
}

inline auto decode_bucket_envelope(std::vector<std::uint8_t> const& buffer)
    -> Expected<std::vector<std::uint8_t>> {
    if (buffer.size() < kBucketHeaderSize) {
        return std::unexpected(make_error("bucket buffer too small for header",
                                          Error::Code::InvalidType));
    }

    if (!std::equal(kBucketMagic.begin(), kBucketMagic.end(), buffer.begin())) {
        return std::unexpected(make_error("bucket buffer missing magic",
                                          Error::Code::UnserializableType));
    }

    std::span<std::uint8_t const> bytes{buffer};

    auto version = read_le32(bytes, 4);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != kBucketBinaryVersion) {
        return std::unexpected(make_error("unsupported bucket binary version",
                                          Error::Code::InvalidType));
    }

    auto endianness = bytes[8];
    if (endianness != static_cast<std::uint8_t>(BucketEndianness::Little)) {
        return std::unexpected(make_error("unsupported bucket endianness",
                                          Error::Code::InvalidType));
    }

    auto payload_size = read_le32(bytes, 12);
    if (!payload_size) {
        return std::unexpected(payload_size.error());
    }

    auto checksum = read_le64(bytes, 16);
    if (!checksum) {
        return std::unexpected(checksum.error());
    }

    auto reserved = read_le64(bytes, 24);
    if (!reserved) {
        return std::unexpected(reserved.error());
    }
    if (*reserved != 0) {
        return std::unexpected(make_error("bucket header reserved bits set",
                                          Error::Code::InvalidType));
    }

    auto available = buffer.size() - kBucketHeaderSize;
    if (*payload_size > available) {
        return std::unexpected(make_error("bucket payload truncated",
                                          Error::Code::InvalidType));
    }

    auto padding = available - *payload_size;
    if (padding > 7) {
        return std::unexpected(make_error("bucket padding exceeds alignment",
                                          Error::Code::InvalidType));
    }

    auto* payload_ptr = buffer.data() + static_cast<std::ptrdiff_t>(kBucketHeaderSize);
    std::span<std::uint8_t const> payload_span{payload_ptr, static_cast<std::size_t>(*payload_size)};

    auto computed_checksum = fnv1a64(payload_span);
    if (computed_checksum != *checksum) {
        return std::unexpected(make_error("bucket payload checksum mismatch",
                                          Error::Code::InvalidType));
    }

    if (padding > 0) {
        auto* padding_begin = payload_ptr + *payload_size;
        auto* padding_end = padding_begin + padding;
        if (std::any_of(padding_begin, padding_end, [](std::uint8_t byte) { return byte != 0; })) {
            return std::unexpected(make_error("bucket padding is not zeroed",
                                              Error::Code::InvalidType));
        }
    }

    return std::vector<std::uint8_t>{payload_span.begin(), payload_span.end()};
}

template <typename T>
inline auto decode_bucket_envelope_as(std::vector<std::uint8_t> const& buffer) -> Expected<T> {
    auto payload = decode_bucket_envelope(buffer);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return from_bytes<T>(*payload);
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
