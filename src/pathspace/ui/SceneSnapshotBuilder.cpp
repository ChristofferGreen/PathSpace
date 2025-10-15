#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <pathspace/ui/Builders.hpp>

#include "alpaca/alpaca.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Scene {

namespace {

constexpr std::string_view kBuildsSegment = "/builds/";
constexpr std::string_view kSnapshotsIndex = "/meta/snapshots/index";
constexpr std::string_view kBucketSummary = "/bucket/summary";

struct SceneRevisionRecord {
    std::uint64_t revision = 0;
    std::int64_t  published_at_ms = 0;
    std::string   author;
};

struct EncodedSnapshotMetadata {
    std::string author;
    std::string tool_version;
    std::int64_t created_at_ms = 0;
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
    std::vector<std::string> fingerprint_digests;
};

struct BucketBinary {
    std::vector<std::uint64_t> drawable_ids;
    std::vector<Transform>     world_transforms;
    std::vector<BoundingSphere> bounds_spheres;
    std::vector<BoundingBox>     bounds_boxes;
    std::vector<std::uint8_t>    bounds_box_valid;
    std::vector<std::uint32_t> layers;
    std::vector<float>         z_values;
    std::vector<std::uint32_t> material_ids;
    std::vector<std::uint32_t> pipeline_flags;
    std::vector<std::uint8_t>  visibility;
    std::vector<std::uint32_t> command_offsets;
    std::vector<std::uint32_t> command_counts;
    std::vector<std::uint32_t> opaque_indices;
    std::vector<std::uint32_t> alpha_indices;
    std::vector<LayerIndices>  layer_indices;
    std::vector<std::uint32_t> command_kinds;
    std::vector<std::uint8_t>  command_payload;
};

struct SnapshotSummary {
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
};

auto make_error(std::string message, Error::Code code = Error::Code::UnknownError) -> Error {
    return Error{code, std::move(message)};
}

template <typename T>
auto to_bytes(T const& obj) -> Expected<std::vector<std::byte>> {
    std::vector<std::uint8_t> buffer;
    std::error_code           ec;
    alpaca::serialize(obj, buffer, ec);
    if (ec) {
        return std::unexpected(make_error("serialization failed: " + ec.message(),
                                          Error::Code::SerializationFunctionMissing));
    }
    std::vector<std::byte> bytes(buffer.size());
    std::transform(buffer.begin(), buffer.end(), bytes.begin(), [](std::uint8_t value) {
        return static_cast<std::byte>(value);
    });
    return bytes;
}

template <typename T>
auto from_bytes(std::span<std::byte const> bytes) -> Expected<T> {
    std::vector<std::uint8_t> buffer(bytes.size());
    std::transform(bytes.begin(), bytes.end(), buffer.begin(), [](std::byte b) {
        return static_cast<std::uint8_t>(b);
    });
    std::error_code ec;
    auto            decoded = alpaca::deserialize<T>(buffer, ec);
    if (ec) {
        return std::unexpected(make_error("deserialization failed: " + ec.message(),
                                          Error::Code::UnserializableType));
    }
    return decoded;
}

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> Expected<void> {
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
auto replace_single(PathSpace& space, std::string const& path, T const& value) -> Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

auto make_revision_base(ScenePath const& scenePath, std::uint64_t revision) -> std::string {
    return std::string(scenePath.getPath()) + std::string(kBuildsSegment) + format_revision(revision);
}

auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

auto ensure_valid_bucket(DrawableBucketSnapshot const& bucket) -> Expected<void> {
    auto const drawable_size = bucket.drawable_ids.size();
    auto check_size = [&](std::size_t size, std::string_view name) -> Expected<void> {
        if (size != drawable_size) {
            return std::unexpected(make_error(std::string(name) + " size mismatch",
                                              Error::Code::InvalidType));
        }
        return {};
    };

    if (auto status = check_size(bucket.world_transforms.size(), "world_transforms"); !status) return status;
    if (auto status = check_size(bucket.bounds_spheres.size(), "bounds_spheres"); !status) return status;
    if (auto status = check_size(bucket.bounds_box_valid.size(), "bounds_box_valid"); !status) return status;
    if (!bucket.bounds_boxes.empty() && bucket.bounds_boxes.size() != drawable_size) {
        return std::unexpected(make_error("bounds_boxes size mismatch",
                                          Error::Code::InvalidType));
    }
    if (auto status = check_size(bucket.layers.size(), "layers"); !status) return status;
    if (auto status = check_size(bucket.z_values.size(), "z_values"); !status) return status;
    if (auto status = check_size(bucket.material_ids.size(), "material_ids"); !status) return status;
    if (auto status = check_size(bucket.pipeline_flags.size(), "pipeline_flags"); !status) return status;
    if (auto status = check_size(bucket.visibility.size(), "visibility"); !status) return status;
    if (auto status = check_size(bucket.command_offsets.size(), "command_offsets"); !status) return status;
    if (auto status = check_size(bucket.command_counts.size(), "command_counts"); !status) return status;
    return {};
}

auto to_view(std::vector<std::byte> const& bytes) -> std::span<std::byte const> {
    return std::span<std::byte const>{bytes.data(), bytes.size()};
}

} // namespace

SceneSnapshotBuilder::SceneSnapshotBuilder(PathSpace& space,
                                           AppRootPathView appRoot,
                                           ScenePath scenePath,
                                           SnapshotRetentionPolicy policy)
    : space_(space)
    , app_root_(appRoot)
    , scene_path_(std::move(scenePath))
    , policy_(policy) {
}

auto SceneSnapshotBuilder::publish(SnapshotPublishOptions const& options,
                                   DrawableBucketSnapshot const& bucket) -> Expected<std::uint64_t> {
    std::lock_guard<std::mutex> lock{mutex_};
    if (auto status = ensure_valid_bucket(bucket); !status) {
        return std::unexpected(status.error());
    }

    auto revisionExpected = next_revision(options.revision);
    if (!revisionExpected) {
        return std::unexpected(revisionExpected.error());
    }
    auto const revision = *revisionExpected;

    auto meta = options.metadata;
    meta.drawable_count = bucket.drawable_ids.size();
    meta.command_count  = bucket.command_kinds.size();
    if (!options.metadata.fingerprint_digests.empty()) {
        meta.fingerprint_digests = options.metadata.fingerprint_digests;
    }

    if (auto stored = store_bucket(revision, bucket, meta); !stored) {
        return std::unexpected(stored.error());
    }
    if (auto recorded = record_snapshot(revision, meta, bucket.command_kinds.size()); !recorded) {
        return std::unexpected(recorded.error());
    }

    auto records = load_index();
    if (!records) {
        return std::unexpected(records.error());
    }
    SnapshotGcMetrics gcMetrics{};
    if (auto pruneStatus = prune_impl(*records, gcMetrics); !pruneStatus) {
        return std::unexpected(pruneStatus.error());
    }
    if (auto persistStatus = persist_index(*records); !persistStatus) {
        return std::unexpected(persistStatus.error());
    }
    gcMetrics.last_revision = revision;
    (void)record_metrics(gcMetrics);
    return revision;
}

auto SceneSnapshotBuilder::prune() -> Expected<void> {
    std::lock_guard<std::mutex> lock{mutex_};
    auto records = load_index();
    if (!records) {
        return std::unexpected(records.error());
    }
    SnapshotGcMetrics gcMetrics{};
    if (auto pruneStatus = prune_impl(*records, gcMetrics); !pruneStatus) {
        return pruneStatus;
    }
    if (auto persistStatus = persist_index(*records); !persistStatus) {
        return persistStatus;
    }
    if (!records->empty()) {
        gcMetrics.last_revision = records->back().revision;
    }
    return record_metrics(gcMetrics);
}

auto SceneSnapshotBuilder::snapshot_records() -> Expected<std::vector<SnapshotRecord>> {
    std::lock_guard<std::mutex> lock{mutex_};
    return load_index();
}

auto SceneSnapshotBuilder::decode_bucket(std::span<std::byte const> bytes) -> Expected<DrawableBucketSnapshot> {
    auto decoded = from_bytes<BucketBinary>(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids     = std::move(decoded->drawable_ids);
    bucket.world_transforms = std::move(decoded->world_transforms);
    bucket.bounds_spheres   = std::move(decoded->bounds_spheres);
    bucket.bounds_boxes     = std::move(decoded->bounds_boxes);
    bucket.bounds_box_valid = std::move(decoded->bounds_box_valid);
    bucket.layers           = std::move(decoded->layers);
    bucket.z_values         = std::move(decoded->z_values);
    bucket.material_ids     = std::move(decoded->material_ids);
    bucket.pipeline_flags   = std::move(decoded->pipeline_flags);
    bucket.visibility       = std::move(decoded->visibility);
    bucket.command_offsets  = std::move(decoded->command_offsets);
    bucket.command_counts   = std::move(decoded->command_counts);
    bucket.opaque_indices   = std::move(decoded->opaque_indices);
    bucket.alpha_indices    = std::move(decoded->alpha_indices);
    bucket.layer_indices    = std::move(decoded->layer_indices);
    bucket.command_kinds    = std::move(decoded->command_kinds);
    bucket.command_payload  = std::move(decoded->command_payload);
    return bucket;
}

auto SceneSnapshotBuilder::decode_metadata(std::span<std::byte const> bytes) -> Expected<SnapshotMetadata> {
    auto decoded = from_bytes<EncodedSnapshotMetadata>(bytes);
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

auto SceneSnapshotBuilder::next_revision(std::optional<std::uint64_t> requested) -> Expected<std::uint64_t> {
    if (requested.has_value()) {
        return *requested;
    }
    auto current = space_.read<std::uint64_t>(std::string(scene_path_.getPath()) + "/current_revision");
    if (!current) {
        if (current.error().code == Error::Code::NoObjectFound
            || current.error().code == Error::Code::NoSuchPath) {
            return std::uint64_t{1};
        }
        return std::unexpected(current.error());
    }
    return *current + 1;
}

auto SceneSnapshotBuilder::store_bucket(std::uint64_t revision,
                                        DrawableBucketSnapshot const& bucket,
                                        SnapshotMetadata const& metadata) -> Expected<void> {
    auto revisionDesc = Builders::SceneRevisionDesc{};
    revisionDesc.revision     = revision;
    revisionDesc.published_at = metadata.created_at;
    revisionDesc.author       = metadata.author;

    BucketBinary binary{};
    binary.drawable_ids    = bucket.drawable_ids;
    binary.world_transforms = bucket.world_transforms;
    binary.bounds_spheres   = bucket.bounds_spheres;
    binary.bounds_boxes     = bucket.bounds_boxes;
    binary.bounds_box_valid = bucket.bounds_box_valid;
    binary.layers           = bucket.layers;
    binary.z_values         = bucket.z_values;
    binary.material_ids     = bucket.material_ids;
    binary.pipeline_flags   = bucket.pipeline_flags;
    binary.visibility       = bucket.visibility;
    binary.command_offsets  = bucket.command_offsets;
    binary.command_counts   = bucket.command_counts;
    binary.opaque_indices   = bucket.opaque_indices;
    binary.alpha_indices    = bucket.alpha_indices;
    binary.layer_indices    = bucket.layer_indices;
    binary.command_kinds    = bucket.command_kinds;
    binary.command_payload  = bucket.command_payload;

    auto encodedBucket = to_bytes(binary);
    if (!encodedBucket) {
        return std::unexpected(encodedBucket.error());
    }

    EncodedSnapshotMetadata encodedMeta{};
    encodedMeta.author         = metadata.author;
    encodedMeta.tool_version   = metadata.tool_version;
    encodedMeta.created_at_ms  = to_epoch_ms(metadata.created_at);
    encodedMeta.drawable_count = static_cast<std::uint64_t>(metadata.drawable_count);
    encodedMeta.command_count  = static_cast<std::uint64_t>(metadata.command_count);
    encodedMeta.fingerprint_digests = metadata.fingerprint_digests;

    auto encodedMetadata = to_bytes(encodedMeta);
    if (!encodedMetadata) {
        return std::unexpected(encodedMetadata.error());
    }

    auto publish = Builders::Scene::PublishRevision(space_,
                                                    scene_path_,
                                                    revisionDesc,
                                                    to_view(*encodedBucket),
                                                    to_view(*encodedMetadata));
    if (!publish) {
        return std::unexpected(publish.error());
    }

    auto revisionBase = make_revision_base(scene_path_, revision);
    struct SnapshotSummary {
        std::uint64_t drawable_count = 0;
        std::uint64_t command_count = 0;
    } summary{
        .drawable_count = static_cast<std::uint64_t>(metadata.drawable_count),
        .command_count  = static_cast<std::uint64_t>(metadata.command_count),
    };

    return replace_single<SnapshotSummary>(space_, revisionBase + std::string(kBucketSummary), summary);
}

auto SceneSnapshotBuilder::record_snapshot(std::uint64_t revision,
                                           SnapshotMetadata const& meta,
                                           std::size_t command_count) -> Expected<void> {
    auto records = load_index();
    if (!records) {
        return std::unexpected(records.error());
    }
    SnapshotRecord record{};
    record.revision       = revision;
    record.created_at_ms  = to_epoch_ms(meta.created_at);
    record.drawable_count = static_cast<std::uint64_t>(meta.drawable_count);
    record.command_count  = static_cast<std::uint64_t>(command_count);
    record.fingerprint_count = static_cast<std::uint64_t>(meta.fingerprint_digests.size());
    records->push_back(record);
    std::sort(records->begin(), records->end(), [](auto const& lhs, auto const& rhs) {
        return lhs.revision < rhs.revision;
    });
    return persist_index(*records);
}

auto SceneSnapshotBuilder::load_index() -> Expected<std::vector<SnapshotRecord>> {
    auto path = std::string(scene_path_.getPath()) + std::string(kSnapshotsIndex);
    auto read = space_.read<std::vector<SnapshotRecord>>(path);
    if (read) {
        return *read;
    }
    if (read.error().code == Error::Code::NoObjectFound
        || read.error().code == Error::Code::NoSuchPath) {
        return std::vector<SnapshotRecord>{};
    }
    return std::unexpected(read.error());
}

auto SceneSnapshotBuilder::persist_index(std::vector<SnapshotRecord> const& records) -> Expected<void> {
    auto path = std::string(scene_path_.getPath()) + std::string(kSnapshotsIndex);
    return replace_single<std::vector<SnapshotRecord>>(space_, path, records);
}

auto SceneSnapshotBuilder::prune_impl(std::vector<SnapshotRecord>& records, SnapshotGcMetrics& metrics) -> Expected<void> {
    if (records.empty()) {
        return {};
    }
    auto currentRevision = space_.read<std::uint64_t>(std::string(scene_path_.getPath()) + "/current_revision");
    std::uint64_t current = 0;
    if (currentRevision) {
        current = *currentRevision;
    }

    auto now_ms = to_epoch_ms(std::chrono::system_clock::now());
    std::sort(records.begin(), records.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.revision > rhs.revision;
    });

    std::vector<std::uint64_t> retain;
    retain.reserve(records.size());

    std::size_t count = 0;
    for (auto const& record : records) {
        bool keep = false;
        if (record.revision == current) {
            keep = true;
        }
        if (count < policy_.min_revisions) {
            keep = true;
        }
        auto age = std::chrono::milliseconds{now_ms - record.created_at_ms};
        if (age <= policy_.min_duration) {
            keep = true;
        }
        if (keep && std::find(retain.begin(), retain.end(), record.revision) == retain.end()) {
            retain.push_back(record.revision);
        }
        ++count;
    }

    auto should_keep = [retain = std::move(retain)](std::uint64_t revision) {
        return std::find(retain.begin(), retain.end(), revision) != retain.end();
    };

    std::vector<SnapshotRecord> filtered;
    filtered.reserve(records.size());
    std::uint64_t evicted = 0;
    for (auto const& record : records) {
        if (should_keep(record.revision)) {
            filtered.push_back(record);
            continue;
        }
        if (record.revision == current) {
            filtered.push_back(record);
            continue;
        }

        auto base = make_revision_base(scene_path_, record.revision);
        (void)space_.take<SceneRevisionRecord>(base + "/desc");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/drawable_bucket");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/metadata");
        (void)space_.take<SnapshotSummary>(base + std::string(kBucketSummary));
        ++evicted;
    }

    std::sort(filtered.begin(), filtered.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.revision < rhs.revision;
    });
    records = std::move(filtered);
    metrics.evicted += evicted;
    metrics.retained = static_cast<std::uint64_t>(records.size());
    for (auto const& record : records) {
        metrics.total_fingerprint_count += record.fingerprint_count;
    }
    return {};
}

auto SceneSnapshotBuilder::record_metrics(SnapshotGcMetrics const& metrics) -> Expected<void> {
    auto path = std::string(scene_path_.getPath()) + "/metrics/snapshots/state";
    return replace_single<SnapshotGcMetrics>(space_, path, metrics);
}

} // namespace SP::UI::Scene
