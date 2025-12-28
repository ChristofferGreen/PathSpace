#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SP::UI::Scene {

// Renderer-owned snapshot registry that keeps drawable buckets and metadata
// out of PathSpace while remaining accessible to renderers and tests.
class RendererSnapshotStore {
public:
    static RendererSnapshotStore& instance();

    auto store(std::string const& scene_path,
               std::uint64_t revision,
               SnapshotMetadata const& metadata,
               DrawableBucketSnapshot const& bucket) -> void;

    auto get_bucket(std::string const& scene_path,
                    std::uint64_t revision) -> Expected<DrawableBucketSnapshot>;

    auto get_metadata(std::string const& scene_path,
                      std::uint64_t revision) -> Expected<SnapshotMetadata>;

    auto records(std::string const& scene_path) -> std::vector<SnapshotRecord>;

    auto prune(std::string const& scene_path,
               SnapshotRetentionPolicy const& policy,
               std::optional<std::uint64_t> current_revision,
               SnapshotGcMetrics& metrics) -> void;

    auto clear_scene(std::string const& scene_path) -> void;

private:
    RendererSnapshotStore() = default;

    struct SnapshotEntry {
        SnapshotMetadata        metadata;
        DrawableBucketSnapshot  bucket;
    };

    struct SceneStore {
        std::unordered_map<std::uint64_t, SnapshotEntry> snapshots;
    };

    std::mutex                                                  mutex_;
    std::unordered_map<std::string, SceneStore>                 scenes_;
};

} // namespace SP::UI::Scene
