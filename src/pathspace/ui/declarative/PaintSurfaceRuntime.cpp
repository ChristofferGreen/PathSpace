#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include "../BuildersDetail.hpp"
#include "widgets/Common.hpp"

#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SP::UI::Declarative::PaintRuntime {

namespace {

namespace BuilderDetail = SP::UI::Builders::Detail;
namespace WidgetDetail = SP::UI::Declarative::Detail;
using DirtyRectHint = SP::UI::Builders::DirtyRectHint;
using SP::UI::Builders::Widgets::Bindings::WidgetOpKind;

constexpr std::string_view kStrokePrefix{"paint_surface/stroke/"};
constexpr std::size_t kMaxPendingDirty = 32;
constexpr int kMaxStrokeReadAttempts = 5;

template <typename T>
auto ensure_value(PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto existing = BuilderDetail::read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return BuilderDetail::replace_single(space, path, value);
}

auto log_gpu_event(PathSpace& space, std::string const& widget_path, std::string_view message) -> void {
    auto path = widget_path + "/render/gpu/log/events";
    (void)space.insert(path, std::string(message));
}

auto ensure_gpu_defaults(PathSpace& space, std::string const& widget_path) -> SP::Expected<void> {
    auto state_path = widget_path + "/render/gpu/state";
    if (auto status = ensure_value(space,
                                   state_path,
                                   std::string(PaintGpuStateToString(PaintGpuState::Idle)));
        !status) {
        return status;
    }
    auto dirty_path = widget_path + "/render/buffer/pendingDirty";
    if (auto status = ensure_value(space, dirty_path, std::vector<DirtyRectHint>{}); !status) {
        return status;
    }
    auto stats_path = widget_path + "/render/gpu/stats";
    if (auto status = ensure_value(space, stats_path, PaintGpuStats{}); !status) {
        return status;
    }
    auto fence_start = widget_path + "/render/gpu/fence/start";
    if (auto status = ensure_value(space, fence_start, std::uint64_t{0}); !status) {
        return status;
    }
    auto fence_end = widget_path + "/render/gpu/fence/end";
    if (auto status = ensure_value(space, fence_end, std::uint64_t{0}); !status) {
        return status;
    }
    return {};
}

auto write_gpu_state(PathSpace& space,
                     std::string const& widget_path,
                     PaintGpuState state) -> void {
    auto path = widget_path + "/render/gpu/state";
    (void)BuilderDetail::replace_single(space,
                                        path,
                                        std::string(PaintGpuStateToString(state)));
}

auto read_gpu_state(PathSpace& space, std::string const& widget_path) -> PaintGpuState {
    auto path = widget_path + "/render/gpu/state";
    auto stored = BuilderDetail::read_optional<std::string>(space, path);
    if (!stored || !stored->has_value()) {
        return PaintGpuState::Idle;
    }
    return PaintGpuStateFromString(**stored);
}

auto make_dirty_hint(PaintStrokePoint const& point,
                     PaintBufferMetrics const& metrics,
                     float brush_size) -> DirtyRectHint {
    auto radius = std::max(brush_size * 0.5f, 1.0f);
    DirtyRectHint hint{};
    hint.min_x = point.x - radius;
    hint.min_y = point.y - radius;
    hint.max_x = point.x + radius;
    hint.max_y = point.y + radius;
    auto width = static_cast<float>(std::max<std::uint32_t>(1u, metrics.width));
    auto height = static_cast<float>(std::max<std::uint32_t>(1u, metrics.height));
    hint.min_x = std::clamp(hint.min_x, 0.0f, width);
    hint.min_y = std::clamp(hint.min_y, 0.0f, height);
    hint.max_x = std::clamp(hint.max_x, 0.0f, width);
    hint.max_y = std::clamp(hint.max_y, 0.0f, height);
    if (hint.max_x <= hint.min_x) {
        hint.max_x = hint.min_x;
    }
    if (hint.max_y <= hint.min_y) {
        hint.max_y = hint.min_y;
    }
    return hint;
}

auto append_pending_dirty(PathSpace& space,
                          std::string const& widget_path,
                          DirtyRectHint const& hint) -> SP::Expected<void> {
    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return {};
    }
    auto pending_path = widget_path + "/render/buffer/pendingDirty";
    auto pending = BuilderDetail::read_optional<std::vector<DirtyRectHint>>(space, pending_path);
    if (!pending) {
        return std::unexpected(pending.error());
    }
    auto values = pending->value_or(std::vector<DirtyRectHint>{});
    values.push_back(hint);
    if (values.size() > kMaxPendingDirty) {
        values.erase(values.begin(), values.end() - kMaxPendingDirty);
    }
    return BuilderDetail::replace_single(space, pending_path, values);
}

auto enqueue_dirty_hint(PathSpace& space,
                        std::string const& widget_path,
                        DirtyRectHint const& hint) -> SP::Expected<void> {
    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return {};
    }
    auto queue_path = widget_path + "/render/gpu/dirtyRects";
    auto inserted = space.insert(queue_path, hint);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return append_pending_dirty(space, widget_path, hint);
}

auto gpu_enabled(PathSpace& space, std::string const& widget_path) -> bool {
    auto path = widget_path + "/render/gpu/enabled";
    auto value = BuilderDetail::read_optional<bool>(space, path);
    if (!value || !value->has_value()) {
        return false;
    }
    return **value;
}

auto increment_revision(PathSpace& space, std::string const& widget_path) -> void {
    auto path = widget_path + "/render/buffer/revision";
    auto current = BuilderDetail::read_optional<std::uint64_t>(space, path);
    if (!current) {
        return;
    }
    auto value = current->value_or(0);
    (void)BuilderDetail::replace_single(space, path, value + 1);
}

auto parse_stroke_id(std::string const& component) -> std::optional<std::uint64_t> {
    if (component.rfind(kStrokePrefix, 0) != 0) {
        return std::nullopt;
    }
    auto suffix = component.substr(kStrokePrefix.size());
    std::uint64_t value = 0;
    auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

auto parse_child_id(std::string const& name) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    auto result = std::from_chars(name.data(), name.data() + name.size(), value);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

auto read_brush_size(PathSpace& space, std::string const& widget_path) -> SP::Expected<float> {
    auto path = widget_path + "/state/brush/size";
    auto value = BuilderDetail::read_optional<float>(space, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return value->value_or(6.0f);
}

auto read_brush_color(PathSpace& space, std::string const& widget_path)
    -> SP::Expected<std::array<float, 4>> {
    auto path = widget_path + "/state/brush/color";
    auto value = BuilderDetail::read_optional<std::array<float, 4>>(space, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return value->value_or(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
}

auto clamp_point(PaintBufferMetrics const& metrics, float x, float y) -> PaintStrokePoint {
    auto width = std::max<std::uint32_t>(1u, metrics.width);
    auto height = std::max<std::uint32_t>(1u, metrics.height);
    PaintStrokePoint point{};
    point.x = std::clamp(x, 0.0f, static_cast<float>(width));
    point.y = std::clamp(y, 0.0f, static_cast<float>(height));
    return point;
}

auto points_path(std::string const& widget_path, std::uint64_t stroke_id) -> std::string {
    std::string path = widget_path;
    path.append("/state/history/");
    path.append(std::to_string(stroke_id));
    path.append("/points");
    return path;
}

auto points_version_path(std::string const& widget_path, std::uint64_t stroke_id) -> std::string {
    std::string path = widget_path;
    path.append("/state/history/");
    path.append(std::to_string(stroke_id));
    path.append("/version");
    return path;
}

auto meta_path(std::string const& widget_path, std::uint64_t stroke_id) -> std::string {
    std::string path = widget_path;
    path.append("/state/history/");
    path.append(std::to_string(stroke_id));
    path.append("/meta");
    return path;
}

auto read_points(PathSpace& space, std::string const& widget_path, std::uint64_t stroke_id)
    -> SP::Expected<std::vector<PaintStrokePoint>> {
    auto points_leaf = points_path(widget_path, stroke_id);
    auto version_leaf = points_version_path(widget_path, stroke_id);
    for (int attempt = 0; attempt < kMaxStrokeReadAttempts; ++attempt) {
        auto version_before = BuilderDetail::read_optional<std::uint64_t>(space, version_leaf);
        if (!version_before) {
            return std::unexpected(version_before.error());
        }
        auto points = BuilderDetail::read_optional<std::vector<PaintStrokePoint>>(space, points_leaf);
        if (!points) {
            return std::unexpected(points.error());
        }
        auto version_after = BuilderDetail::read_optional<std::uint64_t>(space, version_leaf);
        if (!version_after) {
            return std::unexpected(version_after.error());
        }
        auto before_value = version_before->value_or(std::uint64_t{0});
        auto after_value = version_after->value_or(before_value);
        if (before_value == after_value) {
            return points->value_or(std::vector<PaintStrokePoint>{});
        }
    }
    return std::unexpected(BuilderDetail::make_error("paint stroke points mutated during read",
                                                     SP::Error::Code::Timeout));
}

auto read_points_version(PathSpace& space, std::string const& widget_path, std::uint64_t stroke_id)
    -> SP::Expected<std::uint64_t> {
    auto path = points_version_path(widget_path, stroke_id);
    auto value = BuilderDetail::read_optional<std::uint64_t>(space, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return value->value_or(std::uint64_t{0});
}

auto read_meta(PathSpace& space, std::string const& widget_path, std::uint64_t stroke_id)
    -> SP::Expected<std::optional<PaintStrokeMeta>> {
    auto path = meta_path(widget_path, stroke_id);
    auto value = BuilderDetail::read_optional<PaintStrokeMeta>(space, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return *value;
}

auto write_stroke(PathSpace& space,
                  std::string const& widget_path,
                  std::uint64_t stroke_id,
                  PaintStrokeMeta const& meta,
                  std::vector<PaintStrokePoint> const& points) -> SP::Expected<void> {
    auto meta_status = BuilderDetail::replace_single(space, meta_path(widget_path, stroke_id), meta);
    if (!meta_status) {
        return meta_status;
    }
    auto points_status = BuilderDetail::replace_single(space,
                                                       points_path(widget_path, stroke_id),
                                                       points);
    if (!points_status) {
        return points_status;
    }
    auto version = read_points_version(space, widget_path, stroke_id);
    if (!version) {
        return std::unexpected(version.error());
    }
    auto next = *version + 1;
    return BuilderDetail::replace_single(space, points_version_path(widget_path, stroke_id), next);
}

} // namespace

auto EnsureBufferDefaults(PathSpace& space,
                          std::string const& widget_path,
                          PaintBufferMetrics const& defaults) -> SP::Expected<void> {
    auto width_path = widget_path + "/render/buffer/metrics/width";
    auto height_path = widget_path + "/render/buffer/metrics/height";
    auto dpi_path = widget_path + "/render/buffer/metrics/dpi";
    auto viewport_path = widget_path + "/render/buffer/viewport";
    auto revision_path = widget_path + "/render/buffer/revision";

    if (auto status = ensure_value(space, width_path, defaults.width); !status) {
        return status;
    }
    if (auto status = ensure_value(space, height_path, defaults.height); !status) {
        return status;
    }
    if (auto status = ensure_value(space, dpi_path, defaults.dpi); !status) {
        return status;
    }
    PaintBufferViewport viewport{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(defaults.width),
        .max_y = static_cast<float>(defaults.height),
    };
    if (auto status = ensure_value(space, viewport_path, viewport); !status) {
        return status;
    }
    if (auto status = ensure_value(space, revision_path, std::uint64_t{0}); !status) {
        return status;
    }
    return ensure_gpu_defaults(space, widget_path);
}

auto ReadBufferMetrics(PathSpace& space, std::string const& widget_path)
    -> SP::Expected<PaintBufferMetrics> {
    PaintBufferMetrics defaults{};
    if (auto status = EnsureBufferDefaults(space, widget_path, defaults); !status) {
        return std::unexpected(status.error());
    }

    PaintBufferMetrics metrics = defaults;
    auto read_u32 = [&](std::string const& path) -> SP::Expected<std::uint32_t> {
        auto value = space.read<std::uint32_t, std::string>(path);
        if (!value) {
            return std::unexpected(value.error());
        }
        return *value;
    };

    if (auto width = read_u32(widget_path + "/render/buffer/metrics/width")) {
        metrics.width = *width;
    } else {
        return std::unexpected(width.error());
    }
    if (auto height = read_u32(widget_path + "/render/buffer/metrics/height")) {
        metrics.height = *height;
    } else {
        return std::unexpected(height.error());
    }
    auto dpi_value = space.read<float, std::string>(widget_path + "/render/buffer/metrics/dpi");
    if (!dpi_value) {
        return std::unexpected(dpi_value.error());
    }
    metrics.dpi = *dpi_value;
    return metrics;
}

auto ReadStrokePointsConsistent(PathSpace& space,
                                std::string const& widget_path,
                                std::uint64_t stroke_id)
    -> SP::Expected<std::vector<PaintStrokePoint>> {
    return read_points(space, widget_path, stroke_id);
}

auto append_point(PathSpace& space,
                  std::string const& widget_path,
                  std::uint64_t stroke_id,
                  PaintStrokeMeta meta,
                  std::vector<PaintStrokePoint>&& points,
                  std::optional<PaintStrokePoint> const& point,
                  bool commit) -> SP::Expected<bool> {
    bool mutated = false;
    if (point) {
        points.push_back(*point);
        mutated = true;
    }
    if (commit && !meta.committed) {
        meta.committed = true;
        mutated = true;
    }
    if (!mutated) {
        return false;
    }
    auto status = write_stroke(space, widget_path, stroke_id, meta, points);
    if (!status) {
        return std::unexpected(status.error());
    }
    return true;
}

auto HandleAction(PathSpace& space, WidgetAction const& action) -> SP::Expected<bool> {
    switch (action.kind) {
    case WidgetOpKind::PaintStrokeBegin:
    case WidgetOpKind::PaintStrokeUpdate:
    case WidgetOpKind::PaintStrokeCommit:
        break;
    default:
        return false;
    }

    auto stroke_id = parse_stroke_id(action.target_id);
    if (!stroke_id) {
        return false;
    }

    auto metrics = ReadBufferMetrics(space, action.widget_path);
    if (!metrics) {
        return std::unexpected(metrics.error());
    }

    std::optional<PaintStrokePoint> point;
    if (action.pointer.has_local) {
        point = clamp_point(*metrics, action.pointer.local_x, action.pointer.local_y);
    }

    auto brush_size = read_brush_size(space, action.widget_path);
    if (!brush_size) {
        return std::unexpected(brush_size.error());
    }
    auto brush_color = read_brush_color(space, action.widget_path);
    if (!brush_color) {
        return std::unexpected(brush_color.error());
    }
    auto wants_gpu_upload = gpu_enabled(space, action.widget_path);

    auto existing_meta = read_meta(space, action.widget_path, *stroke_id);
    if (!existing_meta) {
        return std::unexpected(existing_meta.error());
    }

    std::vector<PaintStrokePoint> points;
    PaintStrokeMeta meta{
        .brush_size = *brush_size,
        .color = *brush_color,
        .committed = false,
    };

    if (existing_meta->has_value()) {
        meta = existing_meta->value();
        auto loaded = read_points(space, action.widget_path, *stroke_id);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        points = std::move(*loaded);
        if (action.kind == WidgetOpKind::PaintStrokeBegin) {
            points.clear();
            meta.brush_size = *brush_size;
            meta.color = *brush_color;
            meta.committed = false;
        }
    } else if (action.kind == WidgetOpKind::PaintStrokeUpdate
               || action.kind == WidgetOpKind::PaintStrokeCommit) {
        // Ignore updates for unknown strokes until a begin arrives.
        return false;
    }

    auto updated = append_point(space,
                                action.widget_path,
                                *stroke_id,
                                meta,
                                std::move(points),
                                point,
                                action.kind == WidgetOpKind::PaintStrokeCommit);
    if (!updated) {
        return std::unexpected(updated.error());
    }

    if (*updated) {
        (void)BuilderDetail::replace_single(space,
                                            action.widget_path + "/state/history/last_stroke_id",
                                            *stroke_id);
        auto dirty = WidgetDetail::mark_render_dirty(space, action.widget_path);
        if (!dirty) {
            return std::unexpected(dirty.error());
        }
        increment_revision(space, action.widget_path);

        if (point) {
            auto hint = make_dirty_hint(*point, *metrics, *brush_size);
            auto hint_status = enqueue_dirty_hint(space, action.widget_path, hint);
            if (!hint_status) {
                auto message = hint_status.error().message.value_or("failed to enqueue dirty hint");
                log_gpu_event(space, action.widget_path, message);
            } else if (wants_gpu_upload) {
                write_gpu_state(space, action.widget_path, PaintGpuState::DirtyPartial);
            }
        }
    }
    return *updated;
}

auto LoadStrokeRecords(PathSpace& space,
                       std::string const& widget_path)
    -> SP::Expected<std::vector<PaintStrokeRecord>> {
    auto history_root = widget_path + "/state/history";
    auto view = SP::ConcretePathStringView{history_root};
    auto children = space.listChildren(view);
    std::vector<PaintStrokeRecord> records;
    records.reserve(children.size());
    for (auto const& child : children) {
        if (child == "next_id" || child == "last_stroke_id") {
            continue;
        }
        auto id = parse_child_id(child);
        if (!id) {
            continue;
        }
        auto meta = space.read<PaintStrokeMeta, std::string>(history_root + "/" + child + "/meta");
        if (!meta) {
            continue;
        }
        PaintStrokeRecord record{};
        record.id = *id;
        record.meta = *meta;
        auto points = read_points(space, widget_path, *id);
        if (!points) {
            continue;
        }
        record.points = std::move(*points);
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.id < rhs.id;
    });
    return records;
}

} // namespace SP::UI::Declarative::PaintRuntime
