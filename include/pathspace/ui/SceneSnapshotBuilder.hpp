#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SP::UI::Scene {

using AppRootPathView = Builders::AppRootPathView;
using ScenePath = Builders::ScenePath;

struct Transform {
    std::array<float, 16> elements{};
};

struct BoundingSphere {
    std::array<float, 3> center{};
    float                radius = 0.0f;
};

struct BoundingBox {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

struct LayerIndices {
    std::uint32_t             layer = 0;
    std::vector<std::uint32_t> indices;
};

struct DrawableBucketSnapshot {
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

struct SnapshotMetadata {
    std::string author;
    std::string tool_version;
    std::chrono::system_clock::time_point created_at;
    std::size_t drawable_count = 0;
    std::size_t command_count  = 0;
    std::vector<std::string> fingerprint_digests;
};

struct SnapshotPublishOptions {
    std::optional<std::uint64_t> revision;
    SnapshotMetadata              metadata;
};

struct SnapshotRetentionPolicy {
    std::size_t                            min_revisions = 3;
    std::chrono::milliseconds              min_duration  = std::chrono::minutes(2);
};

struct SnapshotRecord {
    std::uint64_t revision = 0;
    std::int64_t  created_at_ms = 0;
    std::uint64_t drawable_count = 0;
    std::uint64_t command_count = 0;
    std::uint64_t fingerprint_count = 0;
};

struct SnapshotGcMetrics {
    std::uint64_t retained = 0;
    std::uint64_t evicted = 0;
    std::uint64_t last_revision = 0;
    std::uint64_t total_fingerprint_count = 0;
};

class SceneSnapshotBuilder {
public:
    SceneSnapshotBuilder(PathSpace& space,
                         AppRootPathView appRoot,
                         ScenePath scenePath,
                         SnapshotRetentionPolicy policy = {});

    auto publish(SnapshotPublishOptions const& options,
                 DrawableBucketSnapshot const& bucket) -> SP::Expected<std::uint64_t>;

    auto prune() -> SP::Expected<void>;

    auto snapshot_records() -> SP::Expected<std::vector<SnapshotRecord>>;

    static auto decode_bucket(std::span<std::byte const> bytes) -> SP::Expected<DrawableBucketSnapshot>;
    static auto decode_metadata(std::span<std::byte const> bytes) -> SP::Expected<SnapshotMetadata>;

private:

    PathSpace&                space_;
    AppRootPathView           app_root_;
    ScenePath                 scene_path_;
    SnapshotRetentionPolicy   policy_;
    std::mutex                mutex_;

    auto next_revision(std::optional<std::uint64_t> requested) -> SP::Expected<std::uint64_t>;
    auto store_bucket(std::uint64_t revision,
                      DrawableBucketSnapshot const& bucket,
                      SnapshotMetadata const& meta) -> SP::Expected<void>;
    auto record_snapshot(std::uint64_t revision, SnapshotMetadata const& meta, std::size_t command_count) -> SP::Expected<void>;
    auto load_index() -> SP::Expected<std::vector<SnapshotRecord>>;
    auto persist_index(std::vector<SnapshotRecord> const& records) -> SP::Expected<void>;
    auto prune_impl(std::vector<SnapshotRecord>& records, SnapshotGcMetrics& metrics) -> SP::Expected<void>;
    auto record_metrics(SnapshotGcMetrics const& metrics) -> SP::Expected<void>;
};

} // namespace SP::UI::Scene
