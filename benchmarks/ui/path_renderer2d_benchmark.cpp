#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/LegacyBuildersDeprecation.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <path/UnvalidatedPath.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ctime>
#include <span>
#include <optional>
#include <vector>

using namespace SP;
using namespace SP::UI;
namespace Runtime = SP::UI::Runtime;
namespace UIScene = SP::UI::Scene;

namespace {

struct Stroke {
    std::uint64_t            drawable_id = 0;
    UIScene::RectCommand     rect{};
    std::string              authoring_id;
};

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (int i = 0; i < 16; ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

template <typename T>
auto replace_value(PathSpace& space, std::string const& path, T const& value) -> void {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& err = taken.error();
        if (err.code == Error::Code::NoObjectFound
            || err.code == Error::Code::NoSuchPath) {
            break;
        }
        auto message = err.message.value_or("unknown error");
        throw std::runtime_error("failed clearing '" + path + "': " + message);
    }
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        auto const& err = inserted.errors.front();
        auto message = err.message.value_or("unknown error");
        throw std::runtime_error("failed writing '" + path + "': " + message);
    }
}

auto build_bucket(std::vector<Stroke> const& strokes) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    auto const count = strokes.size();

    bucket.drawable_ids.reserve(count);
    bucket.world_transforms.reserve(count);
    bucket.bounds_spheres.reserve(count);
    bucket.bounds_boxes.reserve(count);
    bucket.bounds_box_valid.reserve(count);
    bucket.layers.reserve(count);
    bucket.z_values.reserve(count);
    bucket.material_ids.reserve(count);
    bucket.pipeline_flags.reserve(count);
    bucket.visibility.reserve(count);
    bucket.command_offsets.reserve(count);
    bucket.command_counts.reserve(count);
    bucket.authoring_map.reserve(count);
    bucket.clip_head_indices.assign(count, -1);
    bucket.drawable_fingerprints.reserve(count);

    std::hash<std::string> hash_author;

    for (std::size_t i = 0; i < count; ++i) {
        auto const& stroke = strokes[i];
        bucket.drawable_ids.push_back(stroke.drawable_id);
        bucket.world_transforms.push_back(identity_transform());

        UIScene::BoundingBox box{};
        box.min = {stroke.rect.min_x, stroke.rect.min_y, 0.0f};
        box.max = {stroke.rect.max_x, stroke.rect.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto width = std::max(0.0f, stroke.rect.max_x - stroke.rect.min_x);
        auto height = std::max(0.0f, stroke.rect.max_y - stroke.rect.min_y);
        float radius = std::sqrt(width * width + height * height) * 0.5f;
        UIScene::BoundingSphere sphere{};
        sphere.center = {(stroke.rect.min_x + stroke.rect.max_x) * 0.5f,
                         (stroke.rect.min_y + stroke.rect.max_y) * 0.5f,
                         0.0f};
        sphere.radius = radius;
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(i));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);

        auto command_index = static_cast<std::uint32_t>(bucket.command_kinds.size());
        bucket.command_offsets.push_back(command_index);
        bucket.command_counts.push_back(1);
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

        auto payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(UIScene::RectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &stroke.rect,
                    sizeof(UIScene::RectCommand));

        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            stroke.drawable_id,
            stroke.authoring_id,
            0,
            0});

        auto fingerprint = static_cast<std::uint64_t>(hash_author(stroke.authoring_id));
        fingerprint ^= static_cast<std::uint64_t>(stroke.drawable_id) << 32;
        bucket.drawable_fingerprints.push_back(fingerprint);
    }

    bucket.opaque_indices.resize(count);
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0u);
    bucket.alpha_indices.clear();

    return bucket;
}

struct DamageMetrics {
    double coverage = 0.0;
    std::uint64_t rectangles = 0;
    std::uint64_t fingerprint_exact = 0;
    std::uint64_t fingerprint_remap = 0;
    std::uint64_t fingerprint_changed = 0;
    std::uint64_t fingerprint_new = 0;
    std::uint64_t fingerprint_removed = 0;
    std::uint64_t tiles_dirty = 0;
    std::uint64_t tiles_total = 0;
    std::uint64_t tiles_skipped = 0;

    [[nodiscard]] auto dirty_ratio() const -> double {
        return tiles_total > 0 ? static_cast<double>(tiles_dirty) / static_cast<double>(tiles_total) : 0.0;
    }

    [[nodiscard]] auto skipped_ratio() const -> double {
        return tiles_total > 0 ? static_cast<double>(tiles_skipped) / static_cast<double>(tiles_total) : 0.0;
    }
};

struct FrameMetrics {
    double render_ms = 0.0;
   double damage_ms = 0.0;
   double encode_ms = 0.0;
   double progressive_copy_ms = 0.0;
   double publish_ms = 0.0;
   double present_ms = 0.0;
   std::uint64_t tiles = 0;
   std::uint64_t bytes = 0;
   std::optional<DamageMetrics> damage;
};

struct AggregateMetrics {
    std::size_t frame_count = 0;
    double avg_ms = 0.0;
    double fps = 0.0;
    double worst_ms = 0.0;
    double avg_damage_ms = 0.0;
    double avg_encode_ms = 0.0;
    double avg_copy_ms = 0.0;
    double avg_publish_ms = 0.0;
    double avg_present_ms = 0.0;
    double avg_tiles = 0.0;
    double avg_bytes = 0.0;
};

struct DamageSummary {
    double coverage = 0.0;
    double dirty_ratio = 0.0;
    double skipped_ratio = 0.0;
    double rectangles = 0.0;
    double fingerprint_exact = 0.0;
    double fingerprint_remap = 0.0;
    double fingerprint_changed = 0.0;
    double fingerprint_new = 0.0;
    double fingerprint_removed = 0.0;
    double tiles_dirty = 0.0;
    double tiles_total = 0.0;
    double tiles_skipped = 0.0;
    std::size_t samples = 0;
};

auto read_metric(PathSpace const& space, std::string const& base, std::string const& leaf) -> std::uint64_t {
    auto value = space.read<std::uint64_t>(base + "/" + leaf);
    if (value) {
        return *value;
    }
    return 0;
}

auto read_optional_u64(PathSpace const& space, std::string const& base, std::string const& leaf) -> std::optional<std::uint64_t> {
    auto value = space.read<std::uint64_t>(base + "/" + leaf);
    if (value) {
        return *value;
    }
    auto const err = value.error();
    if (err.code == Error::Code::NoObjectFound || err.code == Error::Code::NoSuchPath) {
        return std::nullopt;
    }
    throw std::runtime_error(err.message.value_or("failed to read metric: " + leaf));
}

auto read_optional_double(PathSpace const& space, std::string const& base, std::string const& leaf) -> std::optional<double> {
    auto value = space.read<double>(base + "/" + leaf);
    if (value) {
        return *value;
    }
    auto const err = value.error();
    if (err.code == Error::Code::NoObjectFound || err.code == Error::Code::NoSuchPath) {
        return std::nullopt;
    }
    throw std::runtime_error(err.message.value_or("failed to read metric: " + leaf));
}

auto read_damage_metrics(PathSpace const& space, std::string const& base) -> std::optional<DamageMetrics> {
    auto coverage = read_optional_double(space, base, "damageCoverageRatio");
    if (!coverage) {
        return std::nullopt;
    }

    DamageMetrics metrics{};
    metrics.coverage = *coverage;
    metrics.rectangles = read_optional_u64(space, base, "damageRectangles").value_or(0);
    metrics.fingerprint_exact = read_optional_u64(space, base, "fingerprintMatchesExact").value_or(0);
    metrics.fingerprint_remap = read_optional_u64(space, base, "fingerprintMatchesRemap").value_or(0);
    metrics.fingerprint_changed = read_optional_u64(space, base, "fingerprintChanges").value_or(0);
    metrics.fingerprint_new = read_optional_u64(space, base, "fingerprintNew").value_or(0);
    metrics.fingerprint_removed = read_optional_u64(space, base, "fingerprintRemoved").value_or(0);
    metrics.tiles_dirty = read_optional_u64(space, base, "progressiveTilesDirty").value_or(0);
    metrics.tiles_total = read_optional_u64(space, base, "progressiveTilesTotal").value_or(0);
    metrics.tiles_skipped = read_optional_u64(space, base, "progressiveTilesSkipped").value_or(0);
    return metrics;
}

struct DamageAccumulator {
    std::size_t samples = 0;
    double coverage_sum = 0.0;
    double dirty_ratio_sum = 0.0;
    double skipped_ratio_sum = 0.0;
    double rectangles_sum = 0.0;
    double fingerprint_exact_sum = 0.0;
    double fingerprint_remap_sum = 0.0;
    double fingerprint_changed_sum = 0.0;
    double fingerprint_new_sum = 0.0;
    double fingerprint_removed_sum = 0.0;
    double tiles_dirty_sum = 0.0;
    double tiles_total_sum = 0.0;
    double tiles_skipped_sum = 0.0;

    void add(DamageMetrics const& metrics) {
        ++samples;
        coverage_sum += metrics.coverage;
        dirty_ratio_sum += metrics.dirty_ratio();
        skipped_ratio_sum += metrics.skipped_ratio();
        rectangles_sum += static_cast<double>(metrics.rectangles);
        fingerprint_exact_sum += static_cast<double>(metrics.fingerprint_exact);
        fingerprint_remap_sum += static_cast<double>(metrics.fingerprint_remap);
        fingerprint_changed_sum += static_cast<double>(metrics.fingerprint_changed);
        fingerprint_new_sum += static_cast<double>(metrics.fingerprint_new);
        fingerprint_removed_sum += static_cast<double>(metrics.fingerprint_removed);
        tiles_dirty_sum += static_cast<double>(metrics.tiles_dirty);
        tiles_total_sum += static_cast<double>(metrics.tiles_total);
        tiles_skipped_sum += static_cast<double>(metrics.tiles_skipped);
    }

    [[nodiscard]] auto empty() const -> bool {
        return samples == 0;
    }

    [[nodiscard]] auto aggregate() const -> std::optional<DamageSummary> {
        if (empty()) {
            return std::nullopt;
        }
        auto inv = 1.0 / static_cast<double>(samples);
        DamageSummary summary{};
        summary.coverage = coverage_sum * inv;
        summary.dirty_ratio = dirty_ratio_sum * inv;
        summary.skipped_ratio = skipped_ratio_sum * inv;
        summary.rectangles = rectangles_sum * inv;
        summary.fingerprint_exact = fingerprint_exact_sum * inv;
        summary.fingerprint_remap = fingerprint_remap_sum * inv;
        summary.fingerprint_changed = fingerprint_changed_sum * inv;
        summary.fingerprint_new = fingerprint_new_sum * inv;
        summary.fingerprint_removed = fingerprint_removed_sum * inv;
        summary.tiles_dirty = tiles_dirty_sum * inv;
        summary.tiles_total = tiles_total_sum * inv;
        summary.tiles_skipped = tiles_skipped_sum * inv;
        summary.samples = samples;
        return summary;
    }

    [[nodiscard]] auto summary(std::string_view label) const -> std::string {
        std::ostringstream oss;
        auto aggregated = aggregate();
        if (!aggregated) {
            oss << label << ": metrics unavailable (enable --metrics)";
            return oss.str();
        }
        auto pct = [](double value) {
            return value * 100.0;
        };
        auto const& summary = *aggregated;
        oss << label << ": coverage " << std::fixed << std::setprecision(2) << pct(summary.coverage) << "% avg, "
            << "dirty tiles " << std::setprecision(2) << pct(summary.dirty_ratio) << "%, "
            << "skipped " << std::setprecision(2) << pct(summary.skipped_ratio) << "%; "
            << "rectangles avg " << std::setprecision(2) << summary.rectangles
            << ", fingerprints Δ " << std::setprecision(2) << summary.fingerprint_changed
            << " / remap " << std::setprecision(2) << summary.fingerprint_remap
            << " / new " << std::setprecision(2) << summary.fingerprint_new
            << " / removed " << std::setprecision(2) << summary.fingerprint_removed;
        return oss.str();
    }
};

auto render_frame(PathRenderer2D& renderer,
                  PathSurfaceSoftware& surface,
                  PathSpace& space,
                  Builders::ConcretePathView targetPath,
                  Builders::RenderSettings& settings,
                  std::uint64_t frame_index,
                  bool collect_damage_metrics) -> FrameMetrics {
    using namespace std::chrono_literals;

    settings.time.frame_index = frame_index;
    auto stats = renderer.render({
        .target_path = targetPath,
        .settings = settings,
        .surface = surface,
    });
    if (!stats) {
        auto message = stats.error().message.value_or("render failed");
        throw std::runtime_error(message);
    }

    auto metrics_base = std::string(targetPath.getPath()) + "/output/v1/common";
    FrameMetrics metrics{};
    metrics.render_ms = stats->render_ms;
    metrics.damage_ms = stats->damage_ms;
    metrics.encode_ms = stats->encode_ms;
    metrics.progressive_copy_ms = stats->progressive_copy_ms;
    metrics.publish_ms = stats->publish_ms;
    metrics.tiles = read_metric(space, metrics_base, "progressiveTilesUpdated");
    metrics.bytes = read_metric(space, metrics_base, "progressiveBytesCopied");
    if (collect_damage_metrics) {
        metrics.damage = read_damage_metrics(space, metrics_base);
    }

    static std::vector<std::uint8_t> present_buffer;
    auto const frame_bytes = surface.frame_bytes();
    std::span<std::uint8_t> framebuffer_span{};
    if (frame_bytes > 0) {
        if (present_buffer.size() < frame_bytes) {
            present_buffer.resize(frame_bytes);
        }
        framebuffer_span = std::span<std::uint8_t>{present_buffer.data(), frame_bytes};
    }

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    PathWindowView window_view;
    PathWindowView::PresentPolicy present_policy{};
    present_policy.auto_render_on_present = false;
    auto const now = std::chrono::steady_clock::now();
    auto const vsync_deadline = now + 16ms;

    PathWindowView::PresentRequest present_request{};
    present_request.now = now;
    present_request.vsync_deadline = vsync_deadline;
    present_request.framebuffer = framebuffer_span;
    present_request.dirty_tiles = std::span<std::size_t const>{dirty_tiles.data(), dirty_tiles.size()};
#if defined(__APPLE__)
    present_request.allow_iosurface_sharing = true;
#endif

    auto present_stats = window_view.present(surface, present_policy, present_request);
    metrics.present_ms = present_stats.present_ms;

    return metrics;
}

auto aggregate_frame_metrics(std::vector<FrameMetrics> const& frames) -> AggregateMetrics {
    AggregateMetrics agg{};
    agg.frame_count = frames.size();
    if (frames.empty()) {
        return agg;
    }
    auto count = static_cast<double>(frames.size());
    double sum_ms = 0.0;
    double sum_damage = 0.0;
    double sum_encode = 0.0;
    double sum_progressive = 0.0;
    double sum_publish = 0.0;
    double sum_present = 0.0;
    double sum_tiles = 0.0;
    double sum_bytes = 0.0;
    double worst_ms = 0.0;
    for (auto const& frame : frames) {
        sum_ms += frame.render_ms;
        sum_damage += frame.damage_ms;
        sum_encode += frame.encode_ms;
        sum_progressive += frame.progressive_copy_ms;
        sum_publish += frame.publish_ms;
        sum_present += frame.present_ms;
        sum_tiles += static_cast<double>(frame.tiles);
        sum_bytes += static_cast<double>(frame.bytes);
        worst_ms = std::max(worst_ms, frame.render_ms);
    }
    agg.avg_ms = sum_ms / count;
    agg.avg_damage_ms = sum_damage / count;
    agg.avg_encode_ms = sum_encode / count;
    agg.avg_copy_ms = sum_progressive / count;
    agg.avg_publish_ms = sum_publish / count;
    agg.avg_present_ms = sum_present / count;
    agg.avg_tiles = sum_tiles / count;
    agg.avg_bytes = sum_bytes / count;
    agg.worst_ms = worst_ms;
    agg.fps = agg.avg_ms > 0.0 ? 1000.0 / agg.avg_ms : 0.0;
    return agg;
}

auto json_escape(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size() + 4);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                out += buffer;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

void write_json_report(std::string const& path,
                       int canvas_width,
                       int canvas_height,
                       std::uint32_t progressive_tile_count,
                       std::uint32_t progressive_tile_size,
                       AggregateMetrics const& full_metrics,
                       std::optional<DamageSummary> const& full_damage,
                       AggregateMetrics const& incremental_metrics,
                       std::optional<DamageSummary> const& incremental_damage,
                       bool metrics_enabled,
                       std::vector<std::string> const& argv) {
    auto indent = [](int level) {
        return std::string(static_cast<std::size_t>(level) * 2U, ' ');
    };

    auto write_metrics = [&](std::ostream& os, AggregateMetrics const& metrics, int level) {
        auto pad = indent(level);
        os << pad << "\"frameCount\": " << metrics.frame_count << ",\n";
        os << pad << "\"avgMs\": " << metrics.avg_ms << ",\n";
        os << pad << "\"fps\": " << metrics.fps << ",\n";
        os << pad << "\"worstMs\": " << metrics.worst_ms << ",\n";
        os << pad << "\"avgDamageMs\": " << metrics.avg_damage_ms << ",\n";
        os << pad << "\"avgEncodeMs\": " << metrics.avg_encode_ms << ",\n";
        os << pad << "\"avgCopyMs\": " << metrics.avg_copy_ms << ",\n";
        os << pad << "\"avgPublishMs\": " << metrics.avg_publish_ms << ",\n";
        os << pad << "\"avgPresentMs\": " << metrics.avg_present_ms << ",\n";
        os << pad << "\"avgTiles\": " << metrics.avg_tiles << ",\n";
        os << pad << "\"avgBytes\": " << metrics.avg_bytes << "\n";
    };

    auto write_damage = [&](std::ostream& os, DamageSummary const& damage, int level) {
        auto pad = indent(level);
        os << pad << "\"samples\": " << damage.samples << ",\n";
        os << pad << "\"averageCoverage\": " << damage.coverage << ",\n";
        os << pad << "\"averageDirtyRatio\": " << damage.dirty_ratio << ",\n";
        os << pad << "\"averageSkippedRatio\": " << damage.skipped_ratio << ",\n";
        os << pad << "\"averageRectangles\": " << damage.rectangles << ",\n";
        os << pad << "\"averageFingerprintExact\": " << damage.fingerprint_exact << ",\n";
        os << pad << "\"averageFingerprintRemap\": " << damage.fingerprint_remap << ",\n";
        os << pad << "\"averageFingerprintChanged\": " << damage.fingerprint_changed << ",\n";
        os << pad << "\"averageFingerprintNew\": " << damage.fingerprint_new << ",\n";
        os << pad << "\"averageFingerprintRemoved\": " << damage.fingerprint_removed << ",\n";
        os << pad << "\"averageTilesDirty\": " << damage.tiles_dirty << ",\n";
        os << pad << "\"averageTilesTotal\": " << damage.tiles_total << ",\n";
        os << pad << "\"averageTilesSkipped\": " << damage.tiles_skipped << "\n";
    };

    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
#if defined(_WIN32)
    std::tm utc_tm{};
    gmtime_s(&utc_tm, &now_time);
#else
    std::tm utc_tm{};
    gmtime_r(&now_time, &utc_tm);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc_tm, "%FT%TZ");

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open JSON report for writing: " + path);
    }
    out << std::fixed << std::setprecision(6);

    out << "{\n";
    out << indent(1) << "\"generatedAt\": \"" << timestamp.str() << "\",\n";
    out << indent(1) << "\"canvas\": {\"width\": " << canvas_width << ", \"height\": " << canvas_height << "},\n";
    out << indent(1) << "\"progressive\": {\"tileCount\": " << progressive_tile_count
        << ", \"tileSize\": " << progressive_tile_size << "},\n";
    out << indent(1) << "\"metricsEnabled\": " << (metrics_enabled ? "true" : "false") << ",\n";
    out << indent(1) << "\"command\": {\n";
    out << indent(2) << "\"argv\": [";
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << json_escape(argv[i]) << "\"";
    }
    out << "],\n";
    out << indent(2) << "\"program\": \"" << (argv.empty() ? "" : json_escape(argv.front())) << "\"\n";
    out << indent(1) << "},\n";

    out << indent(1) << "\"frames\": {\n";
    out << indent(2) << "\"fullRepaint\": {\n";
    write_metrics(out, full_metrics, 3);
    if (full_damage) {
        out << ",\n" << indent(3) << "\"damage\": {\n";
        write_damage(out, *full_damage, 4);
        out << indent(3) << "}\n";
    } else {
        out << "\n";
    }
    out << indent(2) << "},\n";

    out << indent(2) << "\"incremental\": {\n";
    write_metrics(out, incremental_metrics, 3);
    if (incremental_damage) {
        out << ",\n" << indent(3) << "\"damage\": {\n";
        write_damage(out, *incremental_damage, 4);
        out << indent(3) << "}\n";
    } else {
        out << "\n";
    }
    out << indent(2) << "}\n";
    out << indent(1) << "}\n";
    out << "}\n";
}

auto format_result(std::vector<FrameMetrics> const& frames) -> std::string {
    if (frames.empty()) {
        return "no frames recorded";
    }
    auto agg = aggregate_frame_metrics(frames);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "frames=" << agg.frame_count
        << " avg_ms=" << agg.avg_ms
        << " fps=" << agg.fps
        << " worst_ms=" << agg.worst_ms
        << " avg_damage_ms=" << agg.avg_damage_ms
        << " avg_encode_ms=" << agg.avg_encode_ms
        << " avg_copy_ms=" << agg.avg_copy_ms
        << " avg_publish_ms=" << agg.avg_publish_ms
        << " avg_present_ms=" << agg.avg_present_ms
        << " avg_tiles=" << agg.avg_tiles
        << " avg_bytes=" << agg.avg_bytes / 1'000'000.0 << "MB";
    return oss.str();
}

auto parse_int(std::string_view value) -> std::optional<int> {
    int result = 0;
    auto const* begin = value.data();
    auto const* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, result);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return result;
}

void print_usage(char const* program) {
    std::cout << "Usage: " << program << " [--canvas=WIDTHxHEIGHT] [--metrics]" << std::endl;
    std::cout << "  --canvas   Set canvas dimensions (default 3840x2160)" << std::endl;
    std::cout << "  --metrics  Enable PATHSPACE_UI_DAMAGE_METRICS and emit damage/fingerprint summaries" << std::endl;
}

void enable_damage_metrics_env() {
#if defined(_WIN32)
    _putenv_s("PATHSPACE_UI_DAMAGE_METRICS", "1");
#else
    ::setenv("PATHSPACE_UI_DAMAGE_METRICS", "1", 1);
#endif
}

} // namespace

int main(int argc, char** argv) try {
    SP::UI::LegacyBuilders::ScopedAllow legacy_allow{};
    int canvas_width = 3840;
    int canvas_height = 2160;
    constexpr int brush_size = 64;
    constexpr int incremental_frames = 48;
    bool enable_metrics = false;
    std::string json_report_path;
    std::vector<std::string> command_args(argv, argv + argc);

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--metrics" || arg == "--enable-metrics") {
            enable_metrics = true;
            continue;
        }
        if (arg.rfind("--canvas=", 0) == 0) {
            auto dims = arg.substr(std::string_view{"--canvas="}.size());
            auto sep = dims.find('x');
            if (sep == std::string_view::npos) {
                std::cerr << "invalid canvas argument (expected WIDTHxHEIGHT)" << std::endl;
                return 1;
            }
            auto width_sv = dims.substr(0, sep);
            auto height_sv = dims.substr(sep + 1);
            auto width = parse_int(width_sv);
            auto height = parse_int(height_sv);
            if (!width || !height || *width <= 0 || *height <= 0) {
                std::cerr << "invalid canvas dimensions: " << dims << std::endl;
                return 1;
            }
            canvas_width = *width;
            canvas_height = *height;
            continue;
        }
        if (arg == "--write-json") {
            if (i + 1 >= argc) {
                std::cerr << "--write-json requires a path argument" << std::endl;
                return 1;
            }
            json_report_path = argv[++i];
            continue;
        }
        if (arg.rfind("--write-json=", 0) == 0) {
            auto path = arg.substr(std::string_view{"--write-json="}.size());
            if (path.empty()) {
                std::cerr << "--write-json requires a non-empty path" << std::endl;
                return 1;
            }
            json_report_path = std::string(path);
            continue;
        }
        std::cerr << "unknown argument: " << arg << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    if (canvas_width <= brush_size || canvas_height <= brush_size) {
        std::cerr << "canvas must be larger than brush size (" << brush_size << "px)" << std::endl;
        return 1;
    }

    if (enable_metrics) {
        enable_damage_metrics_env();
    }

    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/bench_app"};
    auto root_view = SP::App::AppRootPathView{app_root.getPath()};

    // Scene setup
    Builders::SceneParams scene_params{
        .name = "benchmark_scene",
        .description = "Renderer benchmark scene",
    };
    auto scene_path = Builders::Scene::Create(space, root_view, scene_params);
    if (!scene_path) {
        throw std::runtime_error(scene_path.error().message.value_or("failed to create scene"));
    }

    UIScene::SceneSnapshotBuilder snapshot_builder{space, root_view, *scene_path};

    // Renderer + surface setup
    Builders::RendererParams renderer_params{
        .name = "renderer_bench",
        .kind = Builders::RendererKind::Software2D,
        .description = "Benchmark renderer",
    };
    auto renderer_path = Builders::Renderer::Create(space, root_view, renderer_params);
    if (!renderer_path) {
        throw std::runtime_error(renderer_path.error().message.value_or("failed to create renderer"));
    }

    Runtime::SurfaceDesc surface_desc{};
    surface_desc.size_px.width = canvas_width;
    surface_desc.size_px.height = canvas_height;
    surface_desc.pixel_format = Runtime::PixelFormat::BGRA8Unorm;
    surface_desc.color_space = Runtime::ColorSpace::sRGB;
    surface_desc.premultiplied_alpha = true;

    Builders::SurfaceParams surface_params{};
    surface_params.name = "surface_bench";
    surface_params.desc = surface_desc;
    surface_params.renderer = renderer_params.name;
    auto surface_path = Builders::Surface::Create(space, root_view, surface_params);
    if (!surface_path) {
        throw std::runtime_error(surface_path.error().message.value_or("failed to create surface"));
    }

    auto set_scene = Builders::Surface::SetScene(space, *surface_path, *scene_path);
    if (!set_scene) {
        throw std::runtime_error(set_scene.error().message.value_or("failed to bind scene to surface"));
    }

    // Resolve target path
    auto target_rel = space.read<std::string, std::string>(std::string(surface_path->getPath()) + "/target");
    if (!target_rel) {
        throw std::runtime_error(target_rel.error().message.value_or("failed to read surface target"));
    }
    auto target_abs = SP::App::resolve_app_relative(root_view, SP::UnvalidatedPathView{target_rel->c_str()});
    if (!target_abs) {
        throw std::runtime_error(target_abs.error().message.value_or("failed to resolve surface target"));
    }
    auto target_path = *target_abs;
    auto target_path_view = Builders::ConcretePathView{target_path.getPath()};
    auto hints_path = std::string(target_path.getPath()) + "/hints/dirtyRects";

    PathRenderer2D renderer{space};
    PathSurfaceSoftware::Options surface_options{
        .enable_progressive = true,
        .enable_buffered = true,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surface_desc, surface_options};

    Builders::RenderSettings render_settings{};
    render_settings.surface.size_px.width = canvas_width;
    render_settings.surface.size_px.height = canvas_height;
    render_settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    std::vector<Stroke> strokes;
    strokes.reserve(512);
    std::uint64_t next_id = 1;

    auto add_background = [&]() {
        Stroke bg{};
        bg.drawable_id = next_id++;
        bg.rect.min_x = 0.0f;
        bg.rect.min_y = 0.0f;
        bg.rect.max_x = static_cast<float>(canvas_width);
        bg.rect.max_y = static_cast<float>(canvas_height);
        bg.rect.color = {0.1f, 0.1f, 0.12f, 1.0f};
        bg.authoring_id = "background";
        strokes.push_back(bg);
    };

    add_background();

    auto publish_scene = [&]() {
        auto bucket = build_bucket(strokes);
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "path_renderer2d_benchmark";
        opts.metadata.tool_version = "bench";
        opts.metadata.created_at = std::chrono::system_clock::now();
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();
        auto published = snapshot_builder.publish(opts, bucket);
        if (!published) {
            throw std::runtime_error(published.error().message.value_or("failed to publish snapshot"));
        }
    };

    publish_scene();
    replace_value<std::vector<Builders::DirtyRectHint>>(space, hints_path, {});

    std::vector<FrameMetrics> full_frames;
    full_frames.reserve(4);
    std::vector<FrameMetrics> incremental_frames_metrics;
    incremental_frames_metrics.reserve(incremental_frames);
    DamageAccumulator full_damage_acc;
    DamageAccumulator incremental_damage_acc;

    std::uint64_t frame_index = 1;
    // Initial full repaint (background publish).
    {
        auto frame = render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++, enable_metrics);
        if (enable_metrics && frame.damage) {
            full_damage_acc.add(*frame.damage);
        }
        full_frames.push_back(frame);
    }

    // Simulate incremental brush strokes.
    std::mt19937 rng{0xC0FFEE};
    std::uniform_real_distribution<float> dist_x(0.0f, static_cast<float>(canvas_width - brush_size));
    std::uniform_real_distribution<float> dist_y(0.0f, static_cast<float>(canvas_height - brush_size));
    std::uniform_real_distribution<float> dist_color(0.2f, 1.0f);

    for (int i = 0; i < incremental_frames; ++i) {
        float min_x = dist_x(rng);
        float min_y = dist_y(rng);
        Stroke stroke{};
        stroke.drawable_id = next_id++;
        stroke.rect.min_x = min_x;
        stroke.rect.min_y = min_y;
        stroke.rect.max_x = min_x + brush_size;
        stroke.rect.max_y = min_y + brush_size;
        stroke.rect.color = {dist_color(rng), dist_color(rng), dist_color(rng), 1.0f};
        stroke.authoring_id = "stroke/" + std::to_string(stroke.drawable_id);
        strokes.push_back(stroke);

        publish_scene();

        Builders::DirtyRectHint hint{};
        hint.min_x = stroke.rect.min_x - 1.0f;
        hint.min_y = stroke.rect.min_y - 1.0f;
        hint.max_x = stroke.rect.max_x + 1.0f;
        hint.max_y = stroke.rect.max_y + 1.0f;
        std::vector<Builders::DirtyRectHint> hints{hint};
        replace_value(space, hints_path, hints);

        auto frame = render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++, enable_metrics);
        if (enable_metrics && frame.damage) {
            incremental_damage_acc.add(*frame.damage);
        }
        incremental_frames_metrics.push_back(frame);
    }

    // Force a full repaint by clearing hints and changing clear color.
    render_settings.clear_color = {0.02f, 0.02f, 0.02f, 1.0f};
    replace_value<std::vector<Builders::DirtyRectHint>>(space, hints_path, {});
    publish_scene();
    {
        auto frame = render_frame(renderer, surface, space, target_path_view, render_settings, frame_index++, enable_metrics);
        if (enable_metrics && frame.damage) {
            full_damage_acc.add(*frame.damage);
        }
        full_frames.push_back(frame);
    }

    std::cout << "=== PathRenderer2D Benchmark ===" << std::endl;
    std::cout << "Canvas: " << canvas_width << "x" << canvas_height
              << " progressive tiles=" << surface.progressive_tile_count()
              << " initial tile size=" << surface.progressive_tile_size() << "px" << std::endl;
    std::cout << "Full repaint stats: " << format_result(full_frames) << std::endl;
    std::cout << "Incremental stroke stats: " << format_result(incremental_frames_metrics) << std::endl;

    if (enable_metrics) {
        std::cout << full_damage_acc.summary("Full repaint damage metrics") << std::endl;
        std::cout << incremental_damage_acc.summary("Incremental damage metrics") << std::endl;
    }

    auto full_summary = aggregate_frame_metrics(full_frames);
    auto incremental_summary = aggregate_frame_metrics(incremental_frames_metrics);
    auto full_damage_summary = full_damage_acc.aggregate();
    auto incremental_damage_summary = incremental_damage_acc.aggregate();

    if (!json_report_path.empty()) {
        write_json_report(json_report_path,
                          canvas_width,
                          canvas_height,
                          static_cast<std::uint32_t>(surface.progressive_tile_count()),
                          static_cast<std::uint32_t>(surface.progressive_tile_size()),
                          full_summary,
                          full_damage_summary,
                          incremental_summary,
                          incremental_damage_summary,
                          enable_metrics,
                          command_args);
        std::cout << "Wrote JSON metrics to " << json_report_path << std::endl;
    }

    // Small-surface diagnostic matching regression tests
    {
        PathSpace small_space;
        SP::App::AppRootPath small_root{"/system/applications/bench_small"};
        auto small_root_view = SP::App::AppRootPathView{small_root.getPath()};

        Builders::SceneParams sp_params{.name = "small_scene", .description = "Small surface diagnostics"};
        auto sp_scene = Builders::Scene::Create(small_space, small_root_view, sp_params);
        if (!sp_scene) {
            throw std::runtime_error(sp_scene.error().message.value_or("failed to create small scene"));
        }
        UIScene::SceneSnapshotBuilder sp_builder{small_space, small_root_view, *sp_scene};

        auto make_small_bucket = [](float origin_x, float origin_y) {
            UIScene::DrawableBucketSnapshot bucket{};
            bucket.drawable_ids = {0xABCDEFu};
            bucket.world_transforms = {identity_transform()};
            bucket.bounds_boxes = {
                UIScene::BoundingBox{{origin_x, origin_y, 0.0f},
                                     {origin_x + 2.0f, origin_y + 2.0f, 0.0f}},
            };
            bucket.bounds_box_valid = {1};
            bucket.bounds_spheres = {
                UIScene::BoundingSphere{{origin_x + 1.0f, origin_y + 1.0f, 0.0f}, 1.5f},
            };
            bucket.layers = {0};
            bucket.z_values = {0.0f};
            bucket.material_ids = {0};
            bucket.pipeline_flags = {0};
            bucket.visibility = {1};
            bucket.command_offsets = {0};
            bucket.command_counts = {1};
            bucket.opaque_indices = {0};
            bucket.alpha_indices.clear();
            bucket.clip_head_indices = {-1};
            bucket.authoring_map = {
                UIScene::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0},
            };

            UIScene::RectCommand rect{
                .min_x = origin_x,
                .min_y = origin_y,
                .max_x = origin_x + 2.0f,
                .max_y = origin_y + 2.0f,
                .color = {0.4f, 0.2f, 0.9f, 1.0f},
            };
            auto offset = bucket.command_payload.size();
            bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
            std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(UIScene::RectCommand));
            bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));
            return bucket;
        };

        auto small_bucket = make_small_bucket(0.0f, 0.0f);
        UIScene::SnapshotPublishOptions sp_opts{};
        sp_opts.metadata.author = "path_renderer2d_benchmark";
        sp_opts.metadata.tool_version = "bench";
        sp_opts.metadata.drawable_count = 1;
        sp_opts.metadata.command_count = 1;
        if (!sp_builder.publish(sp_opts, small_bucket)) {
            throw std::runtime_error("failed to publish small snapshot");
        }

        Builders::RendererParams sp_renderer_params{
            .name = "small_renderer",
            .kind = Builders::RendererKind::Software2D,
            .description = "",
        };
        auto sp_renderer = Builders::Renderer::Create(small_space, small_root_view, sp_renderer_params);
        if (!sp_renderer) {
            throw std::runtime_error("failed to create small renderer");
        }

        Runtime::SurfaceDesc sp_desc{};
        sp_desc.size_px.width = 8;
        sp_desc.size_px.height = 8;
        sp_desc.pixel_format = Runtime::PixelFormat::RGBA8Unorm_sRGB;
        sp_desc.color_space = Runtime::ColorSpace::sRGB;
        sp_desc.premultiplied_alpha = true;

        Builders::SurfaceParams sp_surface_params{};
        sp_surface_params.name = "small_surface";
        sp_surface_params.desc = sp_desc;
        sp_surface_params.renderer = sp_renderer_params.name;
        auto sp_surface_path = Builders::Surface::Create(small_space, small_root_view, sp_surface_params);
        if (!sp_surface_path) {
            throw std::runtime_error("failed to create small surface");
        }

        if (auto set_scene_result = Builders::Surface::SetScene(small_space, *sp_surface_path, *sp_scene); !set_scene_result) {
            throw std::runtime_error("failed to bind scene for small surface");
        }

        auto sp_target_rel = small_space.read<std::string, std::string>(std::string(sp_surface_path->getPath()) + "/target");
        auto sp_target_abs = SP::App::resolve_app_relative(small_root_view, SP::UnvalidatedPathView{sp_target_rel->c_str()});
        auto sp_target_view = Builders::ConcretePathView{sp_target_abs->getPath()};

        PathRenderer2D sp_renderer_inst{small_space};
        PathSurfaceSoftware::Options sp_opts_surface{.enable_progressive = true, .enable_buffered = false, .progressive_tile_size_px = 2};
        PathSurfaceSoftware sp_surface{sp_desc, sp_opts_surface};

        Builders::RenderSettings sp_settings{};
        sp_settings.surface.size_px.width = sp_desc.size_px.width;
        sp_settings.surface.size_px.height = sp_desc.size_px.height;
        sp_settings.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};

        auto render_small = [&](std::uint64_t frame_index) {
            sp_settings.time.frame_index = frame_index;
            auto stats = sp_renderer_inst.render({
                .target_path = sp_target_view,
                .settings = sp_settings,
                .surface = sp_surface,
            });
            if (!stats) {
                throw std::runtime_error("small surface render failed");
            }
        };

        render_small(1);
        (void)sp_surface.consume_progressive_dirty_tiles();

        auto moved_bucket = make_small_bucket(6.0f, 6.0f);
        if (!sp_builder.publish(sp_opts, moved_bucket)) {
            throw std::runtime_error("failed to publish moved small snapshot");
        }

        render_small(2);
        auto small_tiles = sp_surface.consume_progressive_dirty_tiles();
        std::sort(small_tiles.begin(), small_tiles.end());
        std::cout << "Small-surface tiles: ";
        for (auto tile : small_tiles) {
            std::cout << tile << ' ';
        }
        std::cout << "(count=" << small_tiles.size() << ")" << std::endl;
    }

    return 0;
} catch (std::exception const& ex) {
    std::cerr << "Benchmark failed: " << ex.what() << std::endl;
    return 1;
}
