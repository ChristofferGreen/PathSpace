#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <pathspace/ui/RendererSnapshotStore.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/DebugFlags.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include "SceneSnapshotBuilderDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Scene {

namespace {

constexpr std::string_view kBuildsSegment = "/builds/";

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

auto make_revision_base(ScenePath const& scenePath, std::uint64_t revision) -> std::string {
    return std::string(scenePath.getPath()) + std::string(kBuildsSegment) + format_revision(revision);
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
    if (!bucket.clip_head_indices.empty()) {
        if (auto status = check_size(bucket.clip_head_indices.size(), "clip_head_indices"); !status) return status;
    }
    auto const clip_node_count = bucket.clip_nodes.size();
    for (auto const nodeIndex : bucket.clip_head_indices) {
        if (nodeIndex < -1 || (nodeIndex >= 0 && static_cast<std::size_t>(nodeIndex) >= clip_node_count)) {
            return std::unexpected(make_error("clip_head_indices contains out-of-range index",
                                              Error::Code::InvalidType));
        }
    }
    for (std::size_t i = 0; i < bucket.clip_nodes.size(); ++i) {
        auto const& node = bucket.clip_nodes[i];
        if (node.next < -1 || (node.next >= 0 && static_cast<std::size_t>(node.next) >= clip_node_count)) {
            return std::unexpected(make_error("clip_nodes contains next index out of range",
                                              Error::Code::InvalidType));
        }
        switch (node.type) {
        case ClipNodeType::Rect:
            // No additional validation required yet; fields are directly authored.
            break;
        case ClipNodeType::Path:
            // In path mode, expect a non-zero command count to reference cmd-buffer range.
            if (node.path.command_count == 0) {
                return std::unexpected(make_error("clip_nodes path reference missing command count",
                                                  Error::Code::InvalidType));
            }
            break;
        default:
            return std::unexpected(make_error("clip_nodes contains unknown type",
                                              Error::Code::InvalidType));
        }
    }
    if (!bucket.authoring_map.empty()) {
        if (bucket.authoring_map.size() != drawable_size) {
            return std::unexpected(make_error("authoring_map size mismatch",
                                              Error::Code::InvalidType));
        }
        // Optional sanity: ensure entries follow drawable_ids order
        for (std::size_t i = 0; i < drawable_size; ++i) {
            if (bucket.authoring_map[i].drawable_id != 0
                && bucket.authoring_map[i].drawable_id != bucket.drawable_ids[i]) {
                return std::unexpected(make_error("authoring_map drawable_id mismatch",
                                                  Error::Code::InvalidType));
            }
        }
    }
    if (!bucket.drawable_fingerprints.empty()
        && bucket.drawable_fingerprints.size() != drawable_size) {
        return std::unexpected(make_error("drawable_fingerprints size mismatch",
                                          Error::Code::InvalidType));
    }

    std::size_t payload_cursor = 0;
    for (std::size_t command_index = 0; command_index < bucket.command_kinds.size(); ++command_index) {
        auto kind = static_cast<DrawCommandKind>(bucket.command_kinds[command_index]);
        auto payload_size = payload_size_bytes(kind);
        if (payload_cursor + payload_size > bucket.command_payload.size()) {
            return std::unexpected(make_error("command payload buffer too small for recorded kinds",
                                              Error::Code::InvalidType));
        }
        if (kind == DrawCommandKind::Stroke) {
            StrokeCommand stroke{};
            std::memcpy(&stroke, bucket.command_payload.data() + payload_cursor, sizeof(StrokeCommand));
            auto offset = static_cast<std::size_t>(stroke.point_offset);
            auto count = static_cast<std::size_t>(stroke.point_count);
            if (stroke.thickness < 0.0f) {
                return std::unexpected(make_error("stroke command thickness must be non-negative",
                                                  Error::Code::InvalidType));
            }
            if (offset > bucket.stroke_points.size()
                || count > bucket.stroke_points.size()
                || offset + count > bucket.stroke_points.size()) {
                return std::unexpected(make_error("stroke command references point buffer out of range",
                                                  Error::Code::InvalidType));
            }
        }
        payload_cursor += payload_size;
    }
    if (payload_cursor != bucket.command_payload.size()) {
        return std::unexpected(make_error("command payload contains trailing bytes",
                                          Error::Code::InvalidType));
    }

    return {};
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
    if (!SP::UI::DebugTreeWritesEnabled()) {
        policy_.min_revisions = 1;
        policy_.min_duration = std::chrono::milliseconds{-1};
    }
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

    auto revisionDesc = Runtime::SceneRevisionDesc{};
    revisionDesc.revision     = revision;
    revisionDesc.published_at = meta.created_at;
    revisionDesc.author       = meta.author;

    auto publishStatus = Runtime::Scene::PublishRevision(space_,
                                                         scene_path_,
                                                         revisionDesc,
                                                         std::span<std::byte const>{},
                                                         std::span<std::byte const>{});
    if (!publishStatus) {
        return std::unexpected(publishStatus.error());
    }

    SnapshotGcMetrics gcMetrics{};
    RendererSnapshotStore::instance().prune(scene_path_.getPath(), policy_, revision, gcMetrics);
    gcMetrics.last_revision = revision;
    (void)record_metrics(gcMetrics);
    return revision;
}

auto SceneSnapshotBuilder::prune() -> Expected<void> {
    std::lock_guard<std::mutex> lock{mutex_};
    SnapshotGcMetrics gcMetrics{};
    std::optional<std::uint64_t> current;
    auto currentValue = space_.read<std::uint64_t>(std::string(scene_path_.getPath()) + "/current_revision");
    if (currentValue) {
        current = *currentValue;
    }
    RendererSnapshotStore::instance().prune(scene_path_.getPath(), policy_, current, gcMetrics);
    return record_metrics(gcMetrics);
}

auto SceneSnapshotBuilder::snapshot_records() -> Expected<std::vector<SnapshotRecord>> {
    std::lock_guard<std::mutex> lock{mutex_};
    return RendererSnapshotStore::instance().records(scene_path_.getPath());
}

auto SceneSnapshotBuilder::next_revision(std::optional<std::uint64_t> requested) -> Expected<std::uint64_t> {
    if (requested.has_value()) {
        return *requested;
    }
    if (!SP::UI::DebugTreeWritesEnabled()) {
        return std::uint64_t{1};
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
    DrawableBucketSnapshot stored = bucket;

    if (stored.drawable_fingerprints.size() != stored.drawable_ids.size()) {
        auto computed = compute_drawable_fingerprints(stored);
        if (!computed) {
            return std::unexpected(computed.error());
        }
        stored.drawable_fingerprints = std::move(*computed);
    }

    if (stored.clip_head_indices.empty()) {
        stored.clip_head_indices.assign(stored.drawable_ids.size(), -1);
    }

    if (stored.authoring_map.empty()) {
        stored.authoring_map.resize(stored.drawable_ids.size());
        for (std::size_t i = 0; i < stored.drawable_ids.size(); ++i) {
            stored.authoring_map[i].drawable_id = stored.drawable_ids[i];
        }
    }

    RendererSnapshotStore::instance().store(scene_path_.getPath(), revision, metadata, stored);
    return {};
}

auto SceneSnapshotBuilder::record_snapshot(std::uint64_t revision,
                                           SnapshotMetadata const& meta,
                                           std::size_t command_count) -> Expected<void> {
    (void)revision;
    (void)meta;
    (void)command_count;
    return {};
}

auto SceneSnapshotBuilder::load_index() -> Expected<std::vector<SnapshotRecord>> {
    return RendererSnapshotStore::instance().records(scene_path_.getPath());
}

auto SceneSnapshotBuilder::persist_index(std::vector<SnapshotRecord> const& records) -> Expected<void> {
    (void)records;
    return {};
}

auto SceneSnapshotBuilder::prune_impl(std::vector<SnapshotRecord>& records, SnapshotGcMetrics& metrics) -> Expected<void> {
    std::optional<std::uint64_t> current;
    auto currentRevision = space_.read<std::uint64_t>(std::string(scene_path_.getPath()) + "/current_revision");
    if (currentRevision) {
        current = *currentRevision;
    }
    RendererSnapshotStore::instance().prune(scene_path_.getPath(), policy_, current, metrics);
    records = RendererSnapshotStore::instance().records(scene_path_.getPath());
    return {};
}

auto SceneSnapshotBuilder::record_metrics(SnapshotGcMetrics const& /*metrics*/) -> Expected<void> {
    // Renderer-only: keep GC metrics in the renderer cache; avoid writing
    // snapshot metrics into PathSpace.
    return {};
}

} // namespace SP::UI::Scene
