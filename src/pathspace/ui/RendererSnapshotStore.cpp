#include <pathspace/ui/RendererSnapshotStore.hpp>

#include <pathspace/ui/DebugFlags.hpp>

#include <algorithm>
#include <chrono>

namespace SP::UI::Scene {

namespace {

inline auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

inline auto make_error(std::string message, SP::Error::Code code = SP::Error::Code::NoObjectFound) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

} // namespace

auto RendererSnapshotStore::instance() -> RendererSnapshotStore& {
    static RendererSnapshotStore store;
    return store;
}

auto RendererSnapshotStore::store(std::string const& scene_path,
                                  std::uint64_t revision,
                                  SnapshotMetadata const& metadata,
                                  DrawableBucketSnapshot const& bucket) -> void {
    std::lock_guard<std::mutex> lock{mutex_};
    auto& scene = scenes_[scene_path];
    SnapshotEntry entry{};
    entry.metadata = metadata;
    entry.bucket = bucket;
    scene.snapshots[revision] = std::move(entry);
}

auto RendererSnapshotStore::get_bucket(std::string const& scene_path,
                                       std::uint64_t revision) -> Expected<DrawableBucketSnapshot> {
    std::lock_guard<std::mutex> lock{mutex_};
    auto scene_it = scenes_.find(scene_path);
    if (scene_it == scenes_.end()) {
        return std::unexpected(make_error("scene has no snapshots"));
    }
    auto snap_it = scene_it->second.snapshots.find(revision);
    if (snap_it == scene_it->second.snapshots.end()) {
        return std::unexpected(make_error("snapshot not found for revision"));
    }
    return snap_it->second.bucket;
}

auto RendererSnapshotStore::get_metadata(std::string const& scene_path,
                                         std::uint64_t revision) -> Expected<SnapshotMetadata> {
    std::lock_guard<std::mutex> lock{mutex_};
    auto scene_it = scenes_.find(scene_path);
    if (scene_it == scenes_.end()) {
        return std::unexpected(make_error("scene has no snapshots"));
    }
    auto snap_it = scene_it->second.snapshots.find(revision);
    if (snap_it == scene_it->second.snapshots.end()) {
        return std::unexpected(make_error("snapshot not found for revision"));
    }
    return snap_it->second.metadata;
}

auto RendererSnapshotStore::records(std::string const& scene_path) -> std::vector<SnapshotRecord> {
    std::lock_guard<std::mutex> lock{mutex_};
    std::vector<SnapshotRecord> result;
    auto scene_it = scenes_.find(scene_path);
    if (scene_it == scenes_.end()) {
        return result;
    }
    result.reserve(scene_it->second.snapshots.size());
    for (auto const& [revision, entry] : scene_it->second.snapshots) {
        SnapshotRecord record{};
        record.revision = revision;
        record.created_at_ms = to_epoch_ms(entry.metadata.created_at);
        record.drawable_count = static_cast<std::uint64_t>(
            entry.metadata.drawable_count != 0 ? entry.metadata.drawable_count : entry.bucket.drawable_ids.size());
        record.command_count = static_cast<std::uint64_t>(
            entry.metadata.command_count != 0 ? entry.metadata.command_count : entry.bucket.command_kinds.size());
        record.fingerprint_count = static_cast<std::uint64_t>(entry.bucket.drawable_fingerprints.size());
        result.push_back(record);
    }
    std::sort(result.begin(), result.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.revision < rhs.revision;
    });
    return result;
}

auto RendererSnapshotStore::prune(std::string const& scene_path,
                                  SnapshotRetentionPolicy const& policy,
                                  std::optional<std::uint64_t> current_revision,
                                  SnapshotGcMetrics& metrics) -> void {
    std::lock_guard<std::mutex> lock{mutex_};
    metrics = {};

    auto scene_it = scenes_.find(scene_path);
    if (scene_it == scenes_.end()) {
        return;
    }
    auto& snapshots = scene_it->second.snapshots;
    if (snapshots.empty()) {
        return;
    }

    std::vector<std::uint64_t> revisions;
    revisions.reserve(snapshots.size());
    for (auto const& [rev, _] : snapshots) {
        revisions.push_back(rev);
    }
    std::sort(revisions.begin(), revisions.end(), std::greater<std::uint64_t>());

    std::vector<std::uint64_t> retain;
    retain.reserve(revisions.size());

    auto now = std::chrono::system_clock::now();
    std::size_t count = 0;
    for (auto rev : revisions) {
        auto entry_it = snapshots.find(rev);
        if (entry_it == snapshots.end()) {
            continue;
        }
        bool keep = false;
        if (current_revision && rev == *current_revision) {
            keep = true;
        }
        if (count < policy.min_revisions) {
            keep = true;
        }
        if (policy.min_duration.count() >= 0) {
            auto age = now - entry_it->second.metadata.created_at;
            if (age <= policy.min_duration) {
                keep = true;
            }
        }
        if (keep) {
            retain.push_back(rev);
        }
        ++count;
    }

    auto should_keep = [&](std::uint64_t rev) {
        return std::find(retain.begin(), retain.end(), rev) != retain.end();
    };

    for (auto rev : revisions) {
        if (should_keep(rev)) {
            continue;
        }
        snapshots.erase(rev);
        metrics.evicted += 1;
    }

    metrics.retained = static_cast<std::uint64_t>(snapshots.size());
    metrics.total_fingerprint_count = 0;
    metrics.last_revision = 0;
    for (auto const& [rev, entry] : snapshots) {
        metrics.total_fingerprint_count += static_cast<std::uint64_t>(entry.bucket.drawable_fingerprints.size());
        metrics.last_revision = std::max(metrics.last_revision, rev);
    }
}

auto RendererSnapshotStore::clear_scene(std::string const& scene_path) -> void {
    std::lock_guard<std::mutex> lock{mutex_};
    scenes_.erase(scene_path);
}

} // namespace SP::UI::Scene
