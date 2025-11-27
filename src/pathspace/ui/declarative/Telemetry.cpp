#include <pathspace/ui/declarative/Telemetry.hpp>
#include <pathspace/ui/declarative/Detail.hpp>

#include <chrono>
#include <sstream>
#include <string>

namespace SP::UI::Declarative::Telemetry {
namespace {
namespace Detail = SP::UI::Declarative::Detail;

constexpr std::string_view kSchemaMetricsBase = "/system/widgets/runtime/schema/metrics";
constexpr std::string_view kSchemaLogPath = "/system/widgets/runtime/schema/log/events";
constexpr std::string_view kInputLatencyPath = "/system/widgets/runtime/input/metrics/actions_latency_ns";
constexpr std::string_view kInputBacklogPath = "/system/widgets/runtime/input/metrics/ops_backlog";

auto now_ms() -> std::uint64_t {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

template <typename T>
void assign(PathSpace& space, std::string const& path, T const& value) {
    (void)Detail::replace_single<T>(space, path, value);
}

template <typename T>
void increment(PathSpace& space, std::string const& path, T delta) {
    auto existing = Detail::read_optional<T>(space, path);
    T value = (existing && existing->has_value()) ? **existing : T{};
    value += delta;
    (void)Detail::replace_single<T>(space, path, value);
}

void append_log(PathSpace& space, std::string const& path, std::string const& message) {
    (void)space.insert(path, message);
}

std::string focus_log_path(std::string const& scene_path) {
    std::string path = scene_path;
    path.append("/runtime/focus/log/events");
    return path;
}

std::string focus_metrics_base(std::string const& scene_path) {
    std::string path = scene_path;
    path.append("/runtime/focus/metrics");
    return path;
}

std::string lifecycle_metrics_base(std::string const& scene_path) {
    std::string path = scene_path;
    path.append("/runtime/lifecycle/metrics");
    return path;
}

std::string lifecycle_compare_log(std::string const& scene_path) {
    std::string path = scene_path;
    path.append("/runtime/lifecycle/log/compare");
    return path;
}

std::string widget_focus_metrics(std::string const& widget_path) {
    std::string path = widget_path;
    path.append("/metrics/focus");
    return path;
}

} // namespace

void RecordSchemaSample(PathSpace& space, SchemaSample const& sample) {
    auto base = std::string{kSchemaMetricsBase};
    increment<std::uint64_t>(space, base + "/loads_total", static_cast<std::uint64_t>(1));
    assign<std::uint64_t>(space, base + "/last_load_ns", sample.duration_ns);
    if (!sample.success) {
        increment<std::uint64_t>(space, base + "/failures_total", static_cast<std::uint64_t>(1));
        std::ostringstream oss;
        oss << "widget=" << sample.widget_path << " kind=" << sample.widget_kind << " error=" << sample.error;
        append_log(space, std::string{kSchemaLogPath}, oss.str());
    }
}

void RecordFocusTransition(PathSpace& space, FocusTransitionSample const& sample) {
    auto metrics = focus_metrics_base(sample.scene_path);
    increment<std::uint64_t>(space, metrics + "/transitions_total", static_cast<std::uint64_t>(1));
    if (sample.wrapped) {
        increment<std::uint64_t>(space, metrics + "/wraps_total", static_cast<std::uint64_t>(1));
    }
    assign<std::uint64_t>(space, metrics + "/last_transition_ms", now_ms());

    std::ostringstream oss;
    oss << "window=" << sample.window_component
        << " from=" << sample.previous_widget
        << " to=" << sample.next_widget
        << " wrapped=" << (sample.wrapped ? "true" : "false");
    append_log(space, focus_log_path(sample.scene_path), oss.str());
}

void RecordFocusDisabledSkip(PathSpace& space, std::string const& scene_path) {
    auto metrics = focus_metrics_base(scene_path);
    increment<std::uint64_t>(space, metrics + "/disabled_skips_total", static_cast<std::uint64_t>(1));
}

void IncrementFocusOwnership(PathSpace& space, std::string const& widget_path, bool acquired) {
    auto base = widget_focus_metrics(widget_path);
    if (acquired) {
        increment<std::uint64_t>(space, base + "/acquired_total", static_cast<std::uint64_t>(1));
    } else {
        increment<std::uint64_t>(space, base + "/lost_total", static_cast<std::uint64_t>(1));
    }
}

void RecordInputLatency(PathSpace& space, InputLatencySample const& sample) {
    assign<std::uint64_t>(space, std::string{kInputLatencyPath}, sample.latency_ns);
    assign<std::uint64_t>(space, std::string{kInputBacklogPath}, static_cast<std::uint64_t>(sample.backlog));
}

void AppendWidgetLog(PathSpace& space, std::string const& widget_path, std::string const& message) {
    std::string path = widget_path;
    path.append("/log/events");
    append_log(space, path, message);
}

void RecordRenderDirtySample(PathSpace& space, RenderDirtySample const& sample) {
    auto base = lifecycle_metrics_base(sample.scene_path);
    assign<std::uint64_t>(space, base + "/dirty_batch_ns", sample.duration_ns);
    assign<std::string>(space, base + "/last_dirty_widget", sample.widget_path);
}

void RecordRenderPublishSample(PathSpace& space, RenderPublishSample const& sample) {
    auto base = lifecycle_metrics_base(sample.scene_path);
    assign<std::uint64_t>(space, base + "/publish_ns", sample.duration_ns);
}

void RecordRenderCompareSample(PathSpace& space, RenderCompareSample const& sample) {
    auto base = lifecycle_metrics_base(sample.scene_path);
    assign<bool>(space, base + "/legacy_parity_ok", sample.parity_ok);
    if (sample.diff_percent.has_value()) {
        assign<float>(space, base + "/legacy_diff_percent", *sample.diff_percent);
    }
}

void AppendRenderCompareLog(PathSpace& space,
                            std::string const& scene_path,
                            std::string const& message) {
    append_log(space, lifecycle_compare_log(scene_path), message);
}

} // namespace SP::UI::Declarative::Telemetry
