#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include "SceneSnapshotBuilderDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>

namespace SP::UI::Scene {

namespace {

constexpr std::string_view kBuildsSegment = "/builds/";
constexpr std::string_view kSnapshotsIndex = "/meta/snapshots/index";

struct SceneRevisionRecord {
    std::uint64_t revision = 0;
    std::int64_t  published_at_ms = 0;
    std::string   author;
};

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

auto to_view(std::vector<std::uint8_t> const& bytes) -> std::span<std::byte const> {
    return std::span<std::byte const>{reinterpret_cast<std::byte const*>(bytes.data()), bytes.size()};
}

auto json_escape(std::string_view input) -> std::string {
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '\"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                escaped += buffer;
            } else {
                escaped += ch;
            }
        }
    }
    return escaped;
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
    auto revisionDesc = Runtime::SceneRevisionDesc{};
    revisionDesc.revision     = revision;
    revisionDesc.published_at = metadata.created_at;
    revisionDesc.author       = metadata.author;

    struct BucketManifest {
        std::uint32_t version = 1;
        std::uint64_t drawable_count = 0;
        std::uint64_t command_count = 0;
        std::vector<std::uint32_t> layer_ids;
    } manifest;
    manifest.drawable_count = static_cast<std::uint64_t>(bucket.drawable_ids.size());
    manifest.command_count  = static_cast<std::uint64_t>(bucket.command_kinds.size());
    manifest.layer_ids.reserve(bucket.layer_indices.size());
    for (auto const& layer : bucket.layer_indices) {
        manifest.layer_ids.push_back(layer.layer);
    }

    auto encodedManifest = to_bytes(manifest);
    if (!encodedManifest) {
        return std::unexpected(encodedManifest.error());
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

    auto revisionBase = make_revision_base(scene_path_, revision);
    auto store_enveloped = [&](std::string const& path, auto const& obj) -> Expected<void> {
        auto encoded = encode_bucket_envelope(obj);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return replace_single<std::vector<std::uint8_t>>(space_, path, *encoded);
    };

    std::vector<std::uint64_t> fingerprints = bucket.drawable_fingerprints;
    if (fingerprints.size() != bucket.drawable_ids.size()) {
        auto computed = compute_drawable_fingerprints(bucket);
        if (!computed) {
            return std::unexpected(computed.error());
        }
        fingerprints = std::move(*computed);
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/drawables.bin",
                                      BucketDrawablesBinary{
                                          .drawable_ids    = bucket.drawable_ids,
                                          .command_offsets = bucket.command_offsets,
                                          .command_counts  = bucket.command_counts,
                                      });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/fingerprints.bin",
                                      BucketFingerprintsBinary{ .drawable_fingerprints = fingerprints });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/transforms.bin",
                                      BucketTransformsBinary{ .world_transforms = bucket.world_transforms });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/bounds.bin",
                                      BucketBoundsBinary{
                                          .spheres   = bucket.bounds_spheres,
                                          .boxes     = bucket.bounds_boxes,
                                          .box_valid = bucket.bounds_box_valid,
                                      });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/state.bin",
                                      BucketStateBinary{
                                          .layers         = bucket.layers,
                                          .z_values       = bucket.z_values,
                                          .material_ids   = bucket.material_ids,
                                          .pipeline_flags = bucket.pipeline_flags,
                                          .visibility     = bucket.visibility,
                                      });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/cmd-buffer.bin",
                                      BucketCommandBufferBinary{
                                          .command_kinds   = bucket.command_kinds,
                                          .command_payload = bucket.command_payload,
                                      });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/strokes.bin",
                                      BucketStrokePointsBinary{ .stroke_points = bucket.stroke_points });
        !status) {
        return status;
    }

    std::vector<std::int32_t> clipHeads = bucket.clip_head_indices;
    if (clipHeads.empty()) {
        clipHeads.assign(bucket.drawable_ids.size(), -1);
    }
    if (auto status = store_enveloped(revisionBase + "/bucket/clip-heads.bin",
                                      BucketClipHeadsBinary{ .clip_head_indices = clipHeads });
        !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/clip-nodes.bin",
                                      BucketClipNodesBinary{ .clip_nodes = bucket.clip_nodes });
        !status) {
        return status;
    }

    std::vector<DrawableAuthoringMapEntry> authoringMap = bucket.authoring_map;
    if (authoringMap.empty()) {
        authoringMap.resize(bucket.drawable_ids.size());
        for (std::size_t i = 0; i < bucket.drawable_ids.size(); ++i) {
            authoringMap[i].drawable_id = bucket.drawable_ids[i];
        }
    }
    if (auto status = store_enveloped(revisionBase + "/bucket/authoring-map.bin",
                                      BucketAuthoringMapBinary{ .authoring_map = authoringMap });
        !status) {
        return status;
    }

    auto fontAssetsBytes = encode_font_assets(bucket.font_assets);
    if (!fontAssetsBytes) {
        return std::unexpected(fontAssetsBytes.error());
    }
    if (auto status = replace_single<std::vector<std::uint8_t>>(space_, revisionBase + "/bucket/font-assets.bin", *fontAssetsBytes); !status) {
        return status;
    }

    if (auto status = store_enveloped(revisionBase + "/bucket/glyph-vertices.bin",
                                      BucketGlyphVerticesBinary{ .glyph_vertices = bucket.glyph_vertices });
        !status) {
        return status;
    }

    std::unordered_set<std::string> unique_authoring_nodes;
    unique_authoring_nodes.reserve(authoringMap.size());
    for (auto const& entry : authoringMap) {
        if (!entry.authoring_node_id.empty()) {
            unique_authoring_nodes.insert(entry.authoring_node_id);
        }
    }
    std::string meta_json;
    meta_json.reserve(256);
    meta_json += "{";
    meta_json += "\"revision\":" + std::to_string(revision) + ",";
    meta_json += "\"created_at_ms\":" + std::to_string(to_epoch_ms(metadata.created_at)) + ",";
    meta_json += "\"author\":\"" + json_escape(metadata.author) + "\",";
    meta_json += "\"tool_version\":\"" + json_escape(metadata.tool_version) + "\",";
    meta_json += "\"drawable_count\":" + std::to_string(metadata.drawable_count) + ",";
    meta_json += "\"command_count\":" + std::to_string(metadata.command_count) + ",";
    meta_json += "\"fingerprint_count\":" + std::to_string(metadata.fingerprint_digests.size()) + ",";
    meta_json += "\"font_asset_count\":" + std::to_string(bucket.font_assets.size()) + ",";
    meta_json += "\"authoring_map_entries\":" + std::to_string(authoringMap.size()) + ",";
    meta_json += "\"unique_authoring_nodes\":" + std::to_string(unique_authoring_nodes.size());
    meta_json += "}";
    if (auto status = replace_single<std::string>(space_, revisionBase + "/bucket/meta.json", meta_json); !status) {
        return status;
    }

    if (auto status = replace_single<std::vector<std::uint32_t>>(space_, revisionBase + "/bucket/indices/opaque.bin", bucket.opaque_indices); !status) {
        return status;
    }
    if (auto status = replace_single<std::vector<std::uint32_t>>(space_, revisionBase + "/bucket/indices/alpha.bin", bucket.alpha_indices); !status) {
        return status;
    }

    std::vector<std::uint32_t> layer_ids = manifest.layer_ids;
    for (auto const& layer : bucket.layer_indices) {
        auto layerPath = revisionBase + "/bucket/indices/layer/" + std::to_string(layer.layer) + ".bin";
        if (auto status = replace_single<std::vector<std::uint32_t>>(space_, layerPath, layer.indices); !status) {
            return status;
        }
    }

    SnapshotSummary summary{};
    summary.drawable_count     = static_cast<std::uint64_t>(metadata.drawable_count);
    summary.command_count      = static_cast<std::uint64_t>(metadata.command_count);
    summary.layer_ids          = std::move(layer_ids);
    summary.fingerprint_count  = static_cast<std::uint64_t>(metadata.fingerprint_digests.size());

    if (auto status = replace_single<SnapshotSummary>(space_,
                                                      revisionBase + std::string(kBucketSummary),
                                                      summary);
        !status) {
        return status;
    }

    auto publish = Runtime::Scene::PublishRevision(space_,
                                                    scene_path_,
                                                    revisionDesc,
                                                    to_view(*encodedManifest),
                                                    to_view(*encodedMetadata));
    if (!publish) {
        return std::unexpected(publish.error());
    }
    return {};
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
        std::vector<std::uint32_t> layer_ids;
        if (auto summaryValue = space_.read<SnapshotSummary>(base + std::string(kBucketSummary)); summaryValue) {
            layer_ids = summaryValue->layer_ids;
            (void)space_.take<SnapshotSummary>(base + std::string(kBucketSummary));
        }
        (void)space_.take<SceneRevisionRecord>(base + "/desc");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/drawable_bucket");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/metadata");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/drawables.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/transforms.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/bounds.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/state.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/cmd-buffer.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/strokes.bin");
        (void)space_.take<std::vector<std::uint32_t>>(base + "/bucket/indices/opaque.bin");
        (void)space_.take<std::vector<std::uint32_t>>(base + "/bucket/indices/alpha.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/font-assets.bin");
        (void)space_.take<std::vector<std::uint8_t>>(base + "/bucket/glyph-vertices.bin");
        for (auto layerId : layer_ids) {
            auto layerPath = base + "/bucket/indices/layer/" + std::to_string(layerId) + ".bin";
            (void)space_.take<std::vector<std::uint32_t>>(layerPath);
        }
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
