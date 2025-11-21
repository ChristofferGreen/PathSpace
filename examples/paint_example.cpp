#include "declarative_example_shared.hpp"

#include <pathspace/examples/paint/PaintControls.hpp>

#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/HistoryTelemetry.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <charconv>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>

#include <third_party/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

using namespace PathSpaceExamples;

namespace {

namespace PaintControlsNS = SP::Examples::PaintControls;
using PaintControlsNS::BrushState;
using PaintLayoutMetrics = PaintControlsNS::PaintLayoutMetrics;

constexpr int kRequiredBaselineManifestRevision = 1;

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool headless = false;
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot_compare_path;
    std::optional<std::filesystem::path> screenshot_diff_path;
    std::optional<std::filesystem::path> screenshot_metrics_path;
    double screenshot_max_mean_error = 0.0015;
    bool screenshot_require_present = false;
    bool gpu_smoke = false;
    std::optional<std::filesystem::path> gpu_texture_path;
};

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    auto assign_threshold = [&](std::string_view token) {
        if (token.empty()) {
            std::cerr << "paint_example: --screenshot-max-mean-error requires a value\n";
            return;
        }
        double candidate = opts.screenshot_max_mean_error;
        std::string owned{token};
        std::stringstream stream(owned);
        stream >> candidate;
        if (stream.fail()) {
            std::cerr << "paint_example: invalid --screenshot-max-mean-error value '" << owned << "'\n";
            return;
        }
        if (candidate < 0.0) {
            candidate = 0.0;
        }
        opts.screenshot_max_mean_error = candidate;
    };
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--headless") {
            opts.headless = true;
            continue;
        }
        auto parse_dimension = [&](std::string_view prefix, int& target) {
            if (!arg.starts_with(prefix)) {
                return false;
            }
            auto value_view = arg.substr(prefix.size());
            if (value_view.empty()) {
                return true;
            }
            int value = target;
            auto result = std::from_chars(value_view.begin(), value_view.end(), value);
            if (result.ec == std::errc{}) {
                target = value;
            }
            return true;
        };
        if (parse_dimension("--width=", opts.width)) {
            continue;
        }
        if (parse_dimension("--height=", opts.height)) {
            continue;
        }
        constexpr std::string_view kScreenshotMetricsPrefix = "--screenshot-metrics-json=";
        if (arg == "--screenshot-metrics-json") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot-metrics-json requires a path\n";
                continue;
            }
            ++i;
            opts.screenshot_metrics_path = std::filesystem::path{argv[i]};
            continue;
        }
        if (arg.rfind(kScreenshotMetricsPrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotMetricsPrefix.size());
            if (value.empty()) {
                std::cerr << "paint_example: --screenshot-metrics-json requires a path\n";
                continue;
            }
            opts.screenshot_metrics_path = std::filesystem::path{std::string{value}};
            continue;
        }
        constexpr std::string_view kScreenshotPrefix = "--screenshot=";
        if (arg == "--screenshot") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot requires a path\n";
                continue;
            }
            ++i;
            opts.screenshot_path = std::filesystem::path{argv[i]};
            opts.headless = true;
            continue;
        }
        if (arg.rfind(kScreenshotPrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotPrefix.size());
            if (value.empty()) {
                std::cerr << "paint_example: --screenshot requires a path\n";
                continue;
            }
            opts.screenshot_path = std::filesystem::path{std::string{value}};
            opts.headless = true;
            continue;
        }
        constexpr std::string_view kScreenshotComparePrefix = "--screenshot-compare=";
        if (arg == "--screenshot-compare") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot-compare requires a path\n";
                continue;
            }
            ++i;
            opts.screenshot_compare_path = std::filesystem::path{argv[i]};
            continue;
        }
        if (arg.rfind(kScreenshotComparePrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotComparePrefix.size());
            if (value.empty()) {
                std::cerr << "paint_example: --screenshot-compare requires a path\n";
                continue;
            }
            opts.screenshot_compare_path = std::filesystem::path{std::string{value}};
            continue;
        }
        constexpr std::string_view kScreenshotDiffPrefix = "--screenshot-diff=";
        if (arg == "--screenshot-diff") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot-diff requires a path\n";
                continue;
            }
            ++i;
            opts.screenshot_diff_path = std::filesystem::path{argv[i]};
            continue;
        }
        if (arg.rfind(kScreenshotDiffPrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotDiffPrefix.size());
            if (value.empty()) {
                std::cerr << "paint_example: --screenshot-diff requires a path\n";
                continue;
            }
            opts.screenshot_diff_path = std::filesystem::path{std::string{value}};
            continue;
        }
        constexpr std::string_view kScreenshotMeanErrorPrefix = "--screenshot-max-mean-error=";
        if (arg == "--screenshot-max-mean-error") {
            if (i + 1 >= argc) {
                std::cerr << "paint_example: --screenshot-max-mean-error requires a value\n";
                continue;
            }
            ++i;
            assign_threshold(std::string_view{argv[i]});
            continue;
        }
        if (arg.rfind(kScreenshotMeanErrorPrefix, 0) == 0) {
            auto value = arg.substr(kScreenshotMeanErrorPrefix.size());
            assign_threshold(value);
            continue;
        }
        if (arg == "--screenshot-require-present") {
            opts.screenshot_require_present = true;
            continue;
        }
        constexpr std::string_view kGpuSmokePrefix = "--gpu-smoke=";
        if (arg == "--gpu-smoke") {
            opts.gpu_smoke = true;
            opts.headless = true;
            continue;
        }
        if (arg.rfind(kGpuSmokePrefix, 0) == 0) {
            auto value = arg.substr(kGpuSmokePrefix.size());
            opts.gpu_smoke = true;
            opts.headless = true;
            if (!value.empty()) {
                opts.gpu_texture_path = std::filesystem::path{std::string{value}};
            }
            continue;
        }
        std::cerr << "paint_example: ignoring unknown argument '" << arg << "'\n";
    }
    opts.width = std::max(800, opts.width);
    opts.height = std::max(600, opts.height);
    return opts;
}

struct BaselineTelemetryInputs {
    std::optional<int> manifest_revision;
    std::optional<std::string> tag;
    std::optional<std::string> sha256;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> renderer;
    std::optional<std::string> captured_at;
    std::optional<std::string> commit;
    std::optional<std::string> notes;
    std::optional<double> tolerance;
};

struct ScreenshotRunMetrics {
    std::string status;
    std::uint64_t timestamp_ns = 0;
    bool hardware_capture = false;
    bool require_present = false;
    std::optional<double> mean_error;
    std::optional<std::uint32_t> max_channel_delta;
    std::optional<std::string> screenshot_path;
    std::optional<std::string> diff_path;
};

auto escape_json_string(std::string_view value) -> std::string {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '\"':
            escaped.append("\\\"");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        case '\r':
            escaped.append("\\r");
            break;
        case '\t':
            escaped.append("\\t");
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}


auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "paint_example: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

auto read_env_string(char const* key) -> std::optional<std::string> {
    auto value = std::getenv(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

auto parse_env_int(std::string const& text) -> std::optional<int> {
    int result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{}) {
        return std::nullopt;
    }
    return result;
}

auto parse_env_double(std::string const& text) -> std::optional<double> {
    double value = 0.0;
    std::stringstream stream(text);
    stream >> value;
    if (stream.fail()) {
        return std::nullopt;
    }
    return value;
}

template <typename T>
auto replace_value(SP::PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto publish_baseline_metrics(SP::PathSpace& space, BaselineTelemetryInputs const& telemetry) -> void {
    auto make_leaf_path = [](std::string const& root, std::string_view leaf) {
        std::string path = root;
        path.push_back('/');
        path.append(leaf.begin(), leaf.end());
        return path;
    };
    auto root = std::string{"/diagnostics/ui/paint_example/screenshot_baseline"};
    replace_value(space,
                  make_leaf_path(root, "manifest_revision"),
                  static_cast<std::int64_t>(telemetry.manifest_revision.value_or(0)));
    replace_value(space, make_leaf_path(root, "tag"), telemetry.tag.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "sha256"), telemetry.sha256.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "width"), static_cast<std::int64_t>(telemetry.width.value_or(0)));
    replace_value(space,
                  make_leaf_path(root, "height"),
                  static_cast<std::int64_t>(telemetry.height.value_or(0)));
    replace_value(space, make_leaf_path(root, "renderer"), telemetry.renderer.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "captured_at"), telemetry.captured_at.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "commit"), telemetry.commit.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "notes"), telemetry.notes.value_or(std::string{}));
    replace_value(space, make_leaf_path(root, "tolerance"), telemetry.tolerance.value_or(0.0));
}

auto record_last_run_metrics(SP::PathSpace& space, ScreenshotRunMetrics const& metrics) -> void {
    auto root = std::string{"/diagnostics/ui/paint_example/screenshot_baseline/last_run"};
    auto make_leaf_path = [&](std::string_view leaf) {
        std::string path = root;
        path.push_back('/');
        path.append(leaf.begin(), leaf.end());
        return path;
    };
    replace_value(space,
                  make_leaf_path("timestamp_ns"),
                  static_cast<std::int64_t>(metrics.timestamp_ns));
    replace_value(space, make_leaf_path("status"), metrics.status);
    replace_value(space, make_leaf_path("hardware_capture"), metrics.hardware_capture);
    replace_value(space, make_leaf_path("require_present"), metrics.require_present);
    replace_value(space, make_leaf_path("mean_error"), metrics.mean_error.value_or(0.0));
    replace_value(space,
                  make_leaf_path("max_channel_delta"),
                  static_cast<std::int64_t>(metrics.max_channel_delta.value_or(0)));
    replace_value(space, make_leaf_path("screenshot_path"), metrics.screenshot_path.value_or(std::string{}));
    replace_value(space, make_leaf_path("diff_path"), metrics.diff_path.value_or(std::string{}));
}

auto write_metrics_snapshot_json(std::filesystem::path const& output_path,
                                 BaselineTelemetryInputs const& telemetry,
                                 ScreenshotRunMetrics const& run) -> void {
    if (output_path.empty()) {
        return;
    }
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "paint_example: failed to create metrics directory '" << parent.string() << "'\n";
            return;
        }
    }
    std::ofstream stream(output_path, std::ios::trunc);
    if (!stream) {
        std::cerr << "paint_example: failed to open metrics file '" << output_path.string() << "'\n";
        return;
    }
    auto format_string = [](std::optional<std::string> const& value) {
        if (!value || value->empty()) {
            return std::string{"null"};
        }
        return std::string{"\""} + escape_json_string(*value) + "\"";
    };
    auto format_int = [](std::optional<int> const& value) {
        if (!value) {
            return std::string{"null"};
        }
        return std::to_string(*value);
    };
    auto format_double = [](std::optional<double> const& value) {
        if (!value) {
            return std::string{"null"};
        }
        std::ostringstream out;
        out << std::setprecision(8) << *value;
        return out.str();
    };
    stream << "{\n";
    stream << "  \"schema_version\": 1,\n";
    std::vector<std::string> manifest_lines;
    manifest_lines.push_back("    \"manifest_revision\": "
                             + std::to_string(telemetry.manifest_revision.value_or(0)));
    manifest_lines.push_back("    \"tag\": " + format_string(telemetry.tag));
    manifest_lines.push_back("    \"sha256\": " + format_string(telemetry.sha256));
    manifest_lines.push_back("    \"width\": " + format_int(telemetry.width));
    manifest_lines.push_back("    \"height\": " + format_int(telemetry.height));
    manifest_lines.push_back("    \"renderer\": " + format_string(telemetry.renderer));
    manifest_lines.push_back("    \"captured_at\": " + format_string(telemetry.captured_at));
    manifest_lines.push_back("    \"commit\": " + format_string(telemetry.commit));
    manifest_lines.push_back("    \"notes\": " + format_string(telemetry.notes));
    manifest_lines.push_back("    \"tolerance\": " + format_double(telemetry.tolerance));
    stream << "  \"manifest\": {\n";
    for (std::size_t i = 0; i < manifest_lines.size(); ++i) {
        stream << manifest_lines[i];
        if (i + 1 < manifest_lines.size()) {
            stream << ",\n";
        }
    }
    stream << "\n  },\n";
    std::vector<std::string> run_lines;
    run_lines.push_back("    \"timestamp_ns\": " + std::to_string(run.timestamp_ns));
    run_lines.push_back("    \"status\": \"" + escape_json_string(run.status) + "\"");
    run_lines.push_back("    \"hardware_capture\": " + std::string(run.hardware_capture ? "true" : "false"));
    run_lines.push_back("    \"require_present\": " + std::string(run.require_present ? "true" : "false"));
    if (run.mean_error) {
        std::ostringstream out;
        out << std::setprecision(8) << *run.mean_error;
        run_lines.push_back("    \"mean_error\": " + out.str());
    } else {
        run_lines.push_back("    \"mean_error\": null");
    }
    if (run.max_channel_delta) {
        run_lines.push_back("    \"max_channel_delta\": "
                            + std::to_string(*run.max_channel_delta));
    } else {
        run_lines.push_back("    \"max_channel_delta\": null");
    }
    run_lines.push_back("    \"screenshot_path\": " + format_string(run.screenshot_path));
    run_lines.push_back("    \"diff_path\": " + format_string(run.diff_path));
    stream << "  \"run\": {\n";
    for (std::size_t i = 0; i < run_lines.size(); ++i) {
        stream << run_lines[i];
        if (i + 1 < run_lines.size()) {
            stream << ",\n";
        }
    }
    stream << "\n  }\n";
    stream << "}\n";
}

auto history_metrics_root(std::string const& widget_path) -> std::string {
    if (widget_path.empty()) {
        return {};
    }
    return widget_path + "/metrics/history_binding";
}

template <typename T>
auto write_history_metric(SP::PathSpace& space,
                          std::string const& metrics_root,
                          std::string_view leaf,
                          T const& value) -> void {
    if (metrics_root.empty()) {
        return;
    }
    std::string path = metrics_root;
    path.push_back('/');
    path.append(leaf.begin(), leaf.end());
    auto status = replace_value(space, path, value);
    if (!status) {
        auto context = std::string{"set_history_metric("};
        context.append(leaf.begin(), leaf.end());
        context.push_back(')');
        log_error(status, context);
    }
}

auto record_history_state(SP::PathSpace& space,
                          std::string const& metrics_root,
                          std::string_view state) -> void {
    write_history_metric(space, metrics_root, "state", std::string{state});
    write_history_metric(space, metrics_root, "state_timestamp_ns", now_timestamp_ns());
}

struct HistoryErrorInfo {
    std::string context;
    std::string message;
    std::string code;
    std::uint64_t timestamp_ns = 0;
};

auto record_history_error(SP::PathSpace& space,
                          std::string const& metrics_root,
                          std::string_view context,
                          SP::Error const* error) -> HistoryErrorInfo {
    HistoryErrorInfo info{};
    info.context.assign(context.begin(), context.end());
    if (metrics_root.empty()) {
        return info;
    }
    if (error != nullptr) {
        info.message = SP::describeError(*error);
        info.code = std::string(SP::errorCodeToString(error->code));
    }
    info.timestamp_ns = now_timestamp_ns();
    write_history_metric(space, metrics_root, "last_error_context", info.context);
    write_history_metric(space, metrics_root, "last_error_message", info.message);
    write_history_metric(space, metrics_root, "last_error_code", info.code);
    write_history_metric(space, metrics_root, "last_error_timestamp_ns", info.timestamp_ns);
    return info;
}

auto initialize_history_metrics(SP::PathSpace& space, std::string const& widget_path) -> void {
    auto metrics_root = history_metrics_root(widget_path);
    if (metrics_root.empty()) {
        return;
    }
    record_history_state(space, metrics_root, "pending");
    write_history_metric(space, metrics_root, "buttons_enabled", false);
    write_history_metric(space, metrics_root, "buttons_enabled_last_change_ns", now_timestamp_ns());
    write_history_metric(space, metrics_root, "undo_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "undo_failures_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "redo_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "redo_failures_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "last_error_context", std::string{});
    write_history_metric(space, metrics_root, "last_error_message", std::string{});
    write_history_metric(space, metrics_root, "last_error_code", std::string{});
    write_history_metric(space, metrics_root, "last_error_timestamp_ns", static_cast<std::uint64_t>(0));
    SP::UI::Declarative::HistoryBindingTelemetryCard card{};
    card.state = "pending";
    card.state_timestamp_ns = now_timestamp_ns();
    card.buttons_enabled = false;
    card.buttons_enabled_last_change_ns = card.state_timestamp_ns;
    write_history_metric(space, metrics_root, "card", card);
}

auto window_view_base(SP::UI::Builders::WindowPath const& window_path,
                      std::string const& view_name) -> std::string {
    return std::string(window_path.getPath()) + "/views/" + view_name;
}

auto set_capture_framebuffer_enabled(SP::PathSpace& space,
                                     SP::UI::Builders::WindowPath const& window_path,
                                     std::string const& view_name,
                                     bool enabled) -> SP::Expected<void> {
    auto base = window_view_base(window_path, view_name);
    return replace_value(space, base + "/present/params/capture_framebuffer", enabled);
}

auto format_brush_state(float size, std::array<float, 4> const& color) -> std::string {
    std::ostringstream stream;
    stream << "Brush size: " << std::clamp(size, 1.0f, 128.0f) << " | Color: rgb("
           << std::clamp(color[0], 0.0f, 1.0f) << ", "
           << std::clamp(color[1], 0.0f, 1.0f) << ", "
           << std::clamp(color[2], 0.0f, 1.0f) << ")";
    return stream.str();
}

auto apply_brush_size(SP::PathSpace& space, std::string const& widget_path, float size) -> SP::Expected<void> {
    return replace_value(space, widget_path + "/state/brush/size", size);
}

auto apply_brush_color(SP::PathSpace& space,
                       std::string const& widget_path,
                       std::array<float, 4> const& color) -> SP::Expected<void> {
    return replace_value(space, widget_path + "/state/brush/color", color);
}

auto log_expected_error(std::string const& context, SP::Error const& error) -> void;

using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind;
using DirtyRectHint = SP::UI::Builders::DirtyRectHint;

auto make_paint_action(std::string const& widget_path,
                       WidgetOpKind kind,
                       std::uint64_t stroke_id,
                       float x,
                       float y) -> WidgetAction {
    WidgetAction action{};
    action.widget_path = widget_path;
    action.kind = kind;
    action.target_id = std::string{"paint_surface/stroke/"} + std::to_string(stroke_id);
    action.pointer.has_local = true;
    action.pointer.local_x = x;
    action.pointer.local_y = y;
    return action;
}

auto scripted_stroke_actions(std::string const& widget_path) -> std::vector<WidgetAction> {
    constexpr std::uint64_t kPrimaryStroke = 1;
    constexpr std::uint64_t kAccentStroke = 2;
    std::vector<WidgetAction> actions;
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeBegin,
                                       kPrimaryStroke,
                                       80.0f,
                                       120.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeUpdate,
                                       kPrimaryStroke,
                                       320.0f,
                                       260.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeCommit,
                                       kPrimaryStroke,
                                       460.0f,
                                       420.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeBegin,
                                       kAccentStroke,
                                       420.0f,
                                       140.0f));
    actions.push_back(make_paint_action(widget_path,
                                       WidgetOpKind::PaintStrokeCommit,
                                       kAccentStroke,
                                       160.0f,
                                       420.0f));
    return actions;
}

struct SoftwareImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

auto make_image(int width, int height, std::array<float, 4> color) -> SoftwareImage {
    SoftwareImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    auto sr = static_cast<std::uint8_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f);
    auto sg = static_cast<std::uint8_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f);
    auto sb = static_cast<std::uint8_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f);
    auto sa = static_cast<std::uint8_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f);
    for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = sr;
        image.pixels[i + 1] = sg;
        image.pixels[i + 2] = sb;
        image.pixels[i + 3] = sa;
    }
    return image;
}

inline auto clamp_to_int(int value, int min_value, int max_value) -> int {
    return std::max(min_value, std::min(value, max_value));
}

auto draw_disc(SoftwareImage& image,
               float cx,
               float cy,
               float radius,
               std::array<float, 4> const& color) -> void {
    auto sr = static_cast<std::uint8_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f);
    auto sg = static_cast<std::uint8_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f);
    auto sb = static_cast<std::uint8_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f);
    auto sa = static_cast<std::uint8_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f);
    int min_y = clamp_to_int(static_cast<int>(std::floor(cy - radius)), 0, image.height - 1);
    int max_y = clamp_to_int(static_cast<int>(std::ceil(cy + radius)), 0, image.height - 1);
    int min_x = clamp_to_int(static_cast<int>(std::floor(cx - radius)), 0, image.width - 1);
    int max_x = clamp_to_int(static_cast<int>(std::ceil(cx + radius)), 0, image.width - 1);
    float radius_sq = radius * radius;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            if ((dx * dx + dy * dy) > radius_sq) {
                continue;
            }
            auto index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
            image.pixels[index + 0] = sr;
            image.pixels[index + 1] = sg;
            image.pixels[index + 2] = sb;
            image.pixels[index + 3] = sa;
        }
    }
}

auto draw_line(SoftwareImage& image,
               float x0,
               float y0,
               float x1,
               float y1,
               float radius,
               std::array<float, 4> const& color) -> void {
    auto length = std::max(1.0f, std::hypot(x1 - x0, y1 - y0));
    int steps = static_cast<int>(length);
    for (int i = 0; i <= steps; ++i) {
        float t = steps == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(steps);
        float x = std::lerp(x0, x1, t);
        float y = std::lerp(y0, y1, t);
        draw_disc(image, x, y, radius, color);
    }
}

auto write_image_png(SoftwareImage const& image, std::filesystem::path const& output_path) -> bool {
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "paint_example: failed to create directory '" << parent.string()
                      << "': " << ec.message() << '\n';
            return false;
        }
    }
    auto stride = static_cast<int>(image.width * 4);
    if (stbi_write_png(output_path.string().c_str(),
                       image.width,
                       image.height,
                       4,
                       image.pixels.data(),
                       stride) == 0) {
        std::cerr << "paint_example: failed to write PNG to '" << output_path.string() << "'\n";
        return false;
    }
    return true;
}

auto render_scripted_strokes_image(int width,
                                   int height,
                                   float brush_radius,
                                   std::array<float, 4> const& brush_color,
                                   PaintLayoutMetrics const& layout) -> SoftwareImage {
    auto background = make_image(width, height, {0.07f, 0.08f, 0.12f, 1.0f});
    std::unordered_map<std::string, std::pair<float, float>> active_strokes;
    auto actions = scripted_stroke_actions("screenshot");
    float offset_x = std::max(0.0f, layout.canvas_offset_x);
    float offset_y = std::max(0.0f, layout.canvas_offset_y);
    for (auto const& action : actions) {
        auto sample_x = action.pointer.local_x + offset_x;
        auto sample_y = action.pointer.local_y + offset_y;
        switch (action.kind) {
        case WidgetOpKind::PaintStrokeBegin:
            active_strokes[action.target_id] = {sample_x, sample_y};
            draw_disc(background, sample_x, sample_y, brush_radius, brush_color);
            break;
        case WidgetOpKind::PaintStrokeUpdate: {
            auto it = active_strokes.find(action.target_id);
            if (it != active_strokes.end()) {
                draw_line(background,
                          it->second.first,
                          it->second.second,
                          sample_x,
                          sample_y,
                          brush_radius,
                          brush_color);
                it->second = {sample_x, sample_y};
            }
            break;
        }
        case WidgetOpKind::PaintStrokeCommit: {
            auto it = active_strokes.find(action.target_id);
            if (it != active_strokes.end()) {
                draw_line(background,
                          it->second.first,
                          it->second.second,
                          sample_x,
                          sample_y,
                          brush_radius,
                          brush_color);
                active_strokes.erase(it);
            }
            break;
        }
        default:
            break;
        }
    }
    return background;
}

auto render_scripted_strokes_png(int width,
                                 int height,
                                 std::filesystem::path const& output_path,
                                 float brush_radius,
                                 std::array<float, 4> const& brush_color,
                                 PaintLayoutMetrics const& layout) -> bool {
    auto image = render_scripted_strokes_image(width, height, brush_radius, brush_color, layout);
    return write_image_png(image, output_path);
}

struct ScreenshotImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

auto load_png_rgba(std::filesystem::path const& path) -> std::optional<ScreenshotImage> {
    auto absolute = path.string();
    std::ifstream file(absolute, std::ios::binary);
    if (!file) {
        std::cerr << "paint_example: failed to open PNG '" << absolute << "'\n";
        return std::nullopt;
    }
    std::vector<std::uint8_t> buffer(std::istreambuf_iterator<char>(file), {});
    if (buffer.empty()) {
        std::cerr << "paint_example: PNG '" << absolute << "' is empty\n";
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    int components = 0;
    auto* data =
        stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &width, &height, &components, 4);
    if (data == nullptr) {
        auto reason = stbi_failure_reason();
        std::cerr << "paint_example: failed to decode PNG '" << absolute << "'";
        if (reason != nullptr) {
            std::cerr << " (" << reason << ")";
        }
        std::cerr << '\n';
        return std::nullopt;
    }
    auto total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    ScreenshotImage image{};
    image.width = width;
    image.height = height;
    image.pixels.assign(data, data + total);
    stbi_image_free(data);
    return image;
}

auto overlay_strokes_onto_screenshot(std::filesystem::path const& screenshot_path,
                                     SoftwareImage const& strokes,
                                     PaintLayoutMetrics const& layout) -> bool {
    auto screenshot = load_png_rgba(screenshot_path);
    if (!screenshot) {
        return false;
    }
    if (screenshot->width != strokes.width || screenshot->height != strokes.height) {
        std::cerr << "paint_example: screenshot size mismatch during overlay\n";
        return false;
    }
    auto left = std::clamp(static_cast<int>(std::round(layout.canvas_offset_x)), 0, screenshot->width);
    auto top = std::clamp(static_cast<int>(std::round(layout.canvas_offset_y)), 0, screenshot->height);
    auto right = std::clamp(static_cast<int>(std::round(layout.canvas_offset_x + layout.canvas_width)), left, screenshot->width);
    auto bottom = std::clamp(static_cast<int>(std::round(layout.canvas_offset_y + layout.canvas_height)), top, screenshot->height);
    if (left >= right || top >= bottom) {
        std::cerr << "paint_example: invalid canvas bounds for overlay\n";
        return false;
    }
    auto row_bytes = static_cast<std::size_t>(screenshot->width) * 4u;
    auto copy_bytes = static_cast<std::size_t>(right - left) * 4u;
    for (int y = top; y < bottom; ++y) {
        auto dst = screenshot->pixels.data() + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
        auto src = strokes.pixels.data() + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
        std::copy(src, src + copy_bytes, dst);
    }
    SoftwareImage composite{};
    composite.width = screenshot->width;
    composite.height = screenshot->height;
    composite.pixels = std::move(screenshot->pixels);
    return write_image_png(composite, screenshot_path);
}

struct ScreenshotDiffStats {
    double mean_error = 0.0;
    std::uint8_t max_channel_delta = 0;
};

auto compare_screenshots(std::filesystem::path const& baseline_path,
                         std::filesystem::path const& capture_path,
                         std::optional<std::filesystem::path> const& diff_path)
    -> std::optional<ScreenshotDiffStats> {
    auto baseline = load_png_rgba(baseline_path);
    if (!baseline) {
        return std::nullopt;
    }
    auto capture = load_png_rgba(capture_path);
    if (!capture) {
        return std::nullopt;
    }
    if (baseline->width != capture->width || baseline->height != capture->height) {
        std::cerr << "paint_example: screenshot baseline dimensions (" << baseline->width << "x"
                  << baseline->height << ") do not match capture (" << capture->width << "x"
                  << capture->height << ")\n";
        return std::nullopt;
    }
    ScreenshotDiffStats stats{};
    auto channel_count = static_cast<std::size_t>(baseline->width)
                         * static_cast<std::size_t>(baseline->height) * 4u;
    double total_error = 0.0;

    SoftwareImage diff_image;
    auto write_diff = diff_path.has_value() && channel_count > 0;
    if (write_diff) {
        diff_image.width = baseline->width;
        diff_image.height = baseline->height;
        diff_image.pixels.resize(channel_count);
        for (std::size_t alpha = 3; alpha < diff_image.pixels.size(); alpha += 4) {
            diff_image.pixels[alpha] = 255;
        }
    }

    for (std::size_t offset = 0; offset < baseline->pixels.size(); offset += 4) {
        std::uint8_t pixel_delta = 0;
        for (int channel = 0; channel < 4; ++channel) {
            auto delta = static_cast<std::uint8_t>(
                std::abs(static_cast<int>(baseline->pixels[offset + channel])
                         - static_cast<int>(capture->pixels[offset + channel])));
            stats.max_channel_delta = std::max(stats.max_channel_delta, delta);
            total_error += static_cast<double>(delta) / 255.0;
            pixel_delta = std::max(pixel_delta, delta);
        }
        if (write_diff) {
            auto scaled = static_cast<std::uint8_t>(std::min(255, static_cast<int>(pixel_delta) * 8));
            diff_image.pixels[offset + 0] = scaled;
            diff_image.pixels[offset + 1] = scaled;
            diff_image.pixels[offset + 2] = scaled;
        }
    }

    if (channel_count == 0) {
        stats.mean_error = 0.0;
    } else {
        stats.mean_error = total_error / static_cast<double>(channel_count);
    }

    if (write_diff) {
        if (stats.max_channel_delta == 0) {
            std::error_code ec;
            std::filesystem::remove(*diff_path, ec);
        } else if (!write_image_png(diff_image, *diff_path)) {
            return std::nullopt;
        }
    }

    return stats;
}

auto write_framebuffer_png(std::vector<std::uint8_t> const& framebuffer,
                           int width,
                           int height,
                           std::filesystem::path const& output_path) -> bool {
    if (width <= 0 || height <= 0) {
        std::cerr << "paint_example: invalid framebuffer dimensions for screenshot\n";
        return false;
    }
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "paint_example: failed to create directory '" << parent.string()
                      << "': " << ec.message() << '\n';
            return false;
        }
    }
    auto rows = static_cast<std::size_t>(height);
    auto row_pixels = static_cast<std::size_t>(width) * 4u;
    auto required_bytes = row_pixels * rows;
    std::span<const std::uint8_t> image_span;
    std::vector<std::uint8_t> packed;
    if (framebuffer.size() == required_bytes) {
        image_span = std::span<const std::uint8_t>(framebuffer.data(), framebuffer.size());
    } else if (framebuffer.size() > required_bytes && rows > 0 && framebuffer.size() % rows == 0) {
        auto row_stride = framebuffer.size() / rows;
        if (row_stride < row_pixels) {
            std::cerr << "paint_example: framebuffer stride smaller than row bytes\n";
            return false;
        }
        packed.resize(required_bytes);
        for (int y = 0; y < height; ++y) {
            auto const* src = framebuffer.data() + static_cast<std::size_t>(y) * row_stride;
            auto* dst = packed.data() + static_cast<std::size_t>(y) * row_pixels;
            std::memcpy(dst, src, row_pixels);
        }
        image_span = std::span<const std::uint8_t>(packed.data(), packed.size());
    } else {
        std::cerr << "paint_example: framebuffer too small for screenshot\n";
        return false;
    }
    if (stbi_write_png(output_path.string().c_str(),
                       width,
                       height,
                       4,
                       image_span.data(),
                       static_cast<int>(row_pixels)) == 0) {
        std::cerr << "paint_example: failed to write PNG to '" << output_path.string() << "'\n";
        return false;
    }
    return true;
}

auto capture_present_frame(SP::PathSpace& space,
                           SP::UI::Builders::WindowPath const& window_path,
                           std::string const& view_name,
                           std::chrono::milliseconds timeout)
    -> std::optional<SP::UI::Builders::Window::WindowPresentResult> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto present = SP::UI::Builders::Window::Present(space, window_path, view_name);
        if (!present) {
            auto const& error = present.error();
            if (error.code == SP::Error::Code::NoObjectFound
                || error.code == SP::Error::Code::NoSuchPath) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            log_expected_error("Window::Present", error);
            return std::nullopt;
        }
        if (present->stats.skipped || present->framebuffer.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        return std::optional<SP::UI::Builders::Window::WindowPresentResult>{std::move(*present)};
    }
    std::cerr << "paint_example: Window::Present did not produce a frame before timeout\n";
    return std::nullopt;
}

auto capture_window_screenshot(SP::PathSpace& space,
                               SP::UI::Builders::WindowPath const& window_path,
                               std::string const& view_name,
                               int width,
                               int height,
                               std::filesystem::path const& output_path,
                               std::chrono::milliseconds timeout) -> bool {
    auto present = capture_present_frame(space, window_path, view_name, timeout);
    if (!present) {
        return false;
    }
    return write_framebuffer_png(present->framebuffer, width, height, output_path);
}

auto playback_scripted_strokes(SP::PathSpace& space, std::string const& widget_path) -> bool {
    auto actions = scripted_stroke_actions(widget_path);
    for (auto& action : actions) {
        auto handled = SP::UI::Declarative::PaintRuntime::HandleAction(space, action);
        if (!handled) {
            log_expected_error("PaintRuntime::HandleAction", handled.error());
            return false;
        }
        if (!*handled) {
            std::cerr << "paint_example: scripted stroke had no effect\n";
            return false;
        }
    }
    return true;
}

struct GpuSmokeConfig {
    std::chrono::milliseconds timeout{std::chrono::milliseconds{2000}};
    std::optional<std::filesystem::path> dump_path;
};

auto compute_texture_digest(std::vector<std::uint8_t> const& pixels) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (auto byte : pixels) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

auto write_texture_png(SP::UI::Declarative::PaintTexturePayload const& texture,
                       std::filesystem::path const& output_path) -> bool {
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "paint_example: failed to create directory '" << parent.string()
                      << "': " << ec.message() << '\n';
            return false;
        }
    }
    if (texture.width == 0 || texture.height == 0 || texture.pixels.empty()) {
        std::cerr << "paint_example: GPU texture payload missing pixels\n";
        return false;
    }
    auto row_bytes = static_cast<std::size_t>(texture.width) * 4u;
    auto stride = texture.stride == 0 ? row_bytes : static_cast<std::size_t>(texture.stride);
    if (stride < row_bytes) {
        std::cerr << "paint_example: GPU texture stride smaller than row bytes\n";
        return false;
    }
    if (texture.pixels.size() < stride * static_cast<std::size_t>(texture.height)) {
        std::cerr << "paint_example: GPU texture payload too small for framebuffer copy\n";
        return false;
    }
    std::vector<std::uint8_t> packed(row_bytes * static_cast<std::size_t>(texture.height));
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        auto* src = texture.pixels.data() + static_cast<std::size_t>(y) * stride;
        auto* dst = packed.data() + static_cast<std::size_t>(y) * row_bytes;
        std::copy_n(src, row_bytes, dst);
    }
    if (stbi_write_png(output_path.string().c_str(),
                       static_cast<int>(texture.width),
                       static_cast<int>(texture.height),
                       4,
                       packed.data(),
                       static_cast<int>(row_bytes)) == 0) {
        std::cerr << "paint_example: failed to write GPU texture PNG to '"
                  << output_path.string() << "'\n";
        return false;
    }
    return true;
}

auto read_gpu_state(SP::PathSpace& space,
                    std::string const& widget_path)
    -> std::optional<SP::UI::Declarative::PaintGpuState> {
    auto state_path = widget_path + "/render/gpu/state";
    auto stored = space.read<std::string, std::string>(state_path);
    if (!stored) {
        auto const& error = stored.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return std::nullopt;
        }
        log_expected_error("read gpu state", error);
        return std::nullopt;
    }
    if (stored->empty()) {
        return std::nullopt;
    }
    return SP::UI::Declarative::PaintGpuStateFromString(*stored);
}

auto wait_for_gpu_state(SP::PathSpace& space,
                       std::string const& widget_path,
                       SP::UI::Declarative::PaintGpuState desired,
                       std::chrono::milliseconds timeout)
    -> std::optional<SP::UI::Declarative::PaintGpuState> {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        auto state = read_gpu_state(space, widget_path);
        if (state && *state == desired) {
            return state;
        }
        if (state && *state == SP::UI::Declarative::PaintGpuState::Error) {
            return state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return read_gpu_state(space, widget_path);
}

auto wait_for_scene_revision(SP::PathSpace& space,
                             SP::UI::Builders::ScenePath const& scene_path,
                             std::chrono::milliseconds timeout,
                             std::optional<std::uint64_t> min_revision = std::nullopt)
    -> std::optional<std::uint64_t> {
    auto revision_path = std::string(scene_path.getPath()) + "/current_revision";
    auto format_revision = [](std::uint64_t revision) {
        std::ostringstream oss;
        oss << std::setw(16) << std::setfill('0') << revision;
        return oss.str();
    };
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<std::uint64_t> ready_revision;
    while (std::chrono::steady_clock::now() < deadline) {
        auto revision = space.read<std::uint64_t, std::string>(revision_path);
        if (revision) {
            if (*revision != 0
                && (!min_revision.has_value() || *revision > *min_revision)) {
                ready_revision = *revision;
                break;
            }
        } else {
            auto const& error = revision.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                log_expected_error("read scene revision", error);
                return std::nullopt;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ready_revision) {
        std::cerr << "paint_example: timed out waiting for scene '"
                  << scene_path.getPath() << "' to publish";
        if (min_revision) {
            std::cerr << " revision > " << *min_revision;
        }
        std::cerr << std::endl;
        return std::nullopt;
    }
    auto revision_str = format_revision(*ready_revision);
    auto bucket_path = std::string(scene_path.getPath()) + "/builds/" + revision_str + "/bucket/drawables.bin";
    auto bucket_deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < bucket_deadline) {
        auto drawables = space.read<std::vector<std::uint8_t>, std::string>(bucket_path);
        if (drawables) {
            return ready_revision;
        }
        auto const& error = drawables.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            log_expected_error("read scene bucket", error);
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "paint_example: timed out waiting for scene bucket '"
              << bucket_path << "'" << std::endl;
    return std::nullopt;
}

auto count_window_widgets(SP::PathSpace& space, std::string const& window_view_path) -> std::size_t {
    auto widgets_root = window_view_path + "/widgets";
    auto children = space.listChildren(SP::ConcretePathStringView{widgets_root});
    return children.size();
}

auto wait_for_widget_buckets(SP::PathSpace& space,
                             SP::UI::Builders::ScenePath const& scene_path,
                             std::size_t expected_widgets,
                             std::chrono::milliseconds timeout) -> bool {
    if (expected_widgets == 0) {
        return true;
    }
    auto metrics_base = std::string(scene_path.getPath()) + "/runtime/lifecycle/metrics";
    auto widgets_path = metrics_base + "/widgets_with_buckets";
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto buckets = space.read<std::uint64_t, std::string>(widgets_path);
        if (buckets && *buckets >= expected_widgets) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::cerr << "paint_example: timed out waiting for declarative widgets to publish ("
              << expected_widgets << " expected buckets)\n";
    return false;
}

auto make_scene_widgets_root(SP::UI::Builders::ScenePath const& scene_path,
                             SP::UI::Builders::WindowPath const& window_path,
                             std::string const& view_name) -> std::string {
    auto window_component = std::string(window_path.getPath());
    auto slash = window_component.find_last_of('/');
    if (slash != std::string::npos) {
        window_component = window_component.substr(slash + 1);
    }
    std::string root = std::string(scene_path.getPath());
    root.append("/structure/widgets/windows/");
    root.append(window_component);
    root.append("/views/");
    root.append(view_name);
    root.append("/widgets");
    return root;
}

auto wait_for_scene_widgets(SP::PathSpace& space,
                            std::string const& scene_widgets_root,
                            std::size_t expected_widgets,
                            std::chrono::milliseconds timeout) -> bool {
    if (expected_widgets == 0) {
        return true;
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto children = space.listChildren(SP::ConcretePathStringView{scene_widgets_root});
        if (children.size() >= expected_widgets) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::cerr << "paint_example: timed out waiting for scene widget structure at '"
              << scene_widgets_root << "' (" << expected_widgets << " expected entries)\n";
    return false;
}

auto wait_for_stack_children(SP::PathSpace& space,
                             std::string const& stack_root,
                             std::span<const std::string_view> required_children,
                             std::chrono::milliseconds timeout,
                             bool verbose) -> bool {
    if (required_children.empty()) {
        return true;
    }
    auto children_root = stack_root + "/children";
    std::vector<std::string_view> last_missing;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto children = space.listChildren(SP::ConcretePathStringView{children_root});
        std::vector<std::string_view> missing;
        missing.reserve(required_children.size());
        for (auto child : required_children) {
            auto it = std::find(children.begin(), children.end(), child);
            if (it == children.end()) {
                missing.push_back(child);
            }
        }
        if (missing.empty()) {
            if (verbose) {
                std::cerr << "paint_example: controls stack ready at '" << children_root << "' with "
                          << children.size() << " children\n";
            }
            return true;
        }
        if (verbose && missing != last_missing) {
            std::cerr << "paint_example: waiting for controls children at '" << children_root << "', missing";
            for (auto child : missing) {
                std::cerr << " " << child;
            }
            std::cerr << "\n";
            last_missing = missing;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::cerr << "paint_example: timed out waiting for controls stack children at '"
              << children_root << "'\n";
    return false;
}

auto wait_for_paint_capture_ready(SP::PathSpace& space,
                                  std::string const& widget_path,
                                  std::chrono::milliseconds timeout) -> bool {
    auto state = wait_for_gpu_state(space,
                                    widget_path,
                                    SP::UI::Declarative::PaintGpuState::Ready,
                                    timeout);
    if (!state) {
        std::cerr << "paint_example: failed to read paint GPU state before capture\n";
        return false;
    }
    if (*state != SP::UI::Declarative::PaintGpuState::Ready) {
        std::cerr << "paint_example: paint GPU state '"
                  << SP::UI::Declarative::PaintGpuStateToString(*state)
                  << "' while waiting for Ready\n";
        return false;
    }

    auto pending_path = widget_path + "/render/buffer/pendingDirty";
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto pending = space.read<std::vector<DirtyRectHint>, std::string>(pending_path);
        if (!pending) {
            log_expected_error("read pending dirty hints", pending.error());
            return false;
        }
        if (pending->empty()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "paint_example: pending dirty hints not drained before capture\n";
    return false;
}

auto run_gpu_smoke(SP::PathSpace& space,
                   std::string const& widget_path,
                   GpuSmokeConfig const& config) -> bool {
    if (!playback_scripted_strokes(space, widget_path)) {
        return false;
    }

    auto state = wait_for_gpu_state(space,
                                    widget_path,
                                    SP::UI::Declarative::PaintGpuState::Ready,
                                    config.timeout);
    if (!state || *state != SP::UI::Declarative::PaintGpuState::Ready) {
        std::cerr << "paint_example: GPU smoke timed out waiting for Ready "
                  << "(state=" << (state ? std::string(SP::UI::Declarative::PaintGpuStateToString(*state))
                                         : std::string("unknown"))
                  << ")\n";
        return false;
    }

    auto texture_path = widget_path + "/assets/texture";
    auto texture = space.read<SP::UI::Declarative::PaintTexturePayload, std::string>(texture_path);
    if (!texture) {
        log_expected_error("read GPU texture", texture.error());
        return false;
    }
    if (texture->pixels.empty()) {
        std::cerr << "paint_example: GPU texture has no pixels\n";
        return false;
    }

    auto metrics = SP::UI::Declarative::PaintRuntime::ReadBufferMetrics(space, widget_path);
    if (!metrics) {
        log_expected_error("read paint buffer metrics", metrics.error());
        return false;
    }

    if (texture->width != metrics->width || texture->height != metrics->height) {
        std::cerr << "paint_example: GPU texture dimensions (" << texture->width << "x"
                  << texture->height << ") differ from buffer metrics (" << metrics->width
                  << "x" << metrics->height << ")\n";
        return false;
    }

    auto stats_path = widget_path + "/render/gpu/stats";
    auto stats = space.read<SP::UI::Declarative::PaintGpuStats, std::string>(stats_path);
    if (!stats) {
        log_expected_error("read GPU stats", stats.error());
        return false;
    }
    if (stats->uploads_total == 0) {
        std::cerr << "paint_example: GPU uploader never staged a texture\n";
        return false;
    }

    auto pending_path = widget_path + "/render/buffer/pendingDirty";
    auto pending_dirty = space.read<std::vector<DirtyRectHint>, std::string>(pending_path);
    if (!pending_dirty) {
        log_expected_error("read pending dirty hints", pending_dirty.error());
        return false;
    }
    if (!pending_dirty->empty()) {
        std::cerr << "paint_example: pending dirty hints not drained after GPU upload\n";
        return false;
    }

    auto digest = compute_texture_digest(texture->pixels);
    std::cout << "paint_example: GPU smoke ready (revision " << texture->revision
              << ", bytes " << texture->pixels.size()
              << ", digest 0x" << std::hex << digest << std::dec << ")\n";

    if (config.dump_path) {
        if (!write_texture_png(*texture, *config.dump_path)) {
            return false;
        }
        std::cout << "paint_example: wrote GPU texture PNG to "
                  << config.dump_path->string() << "\n";
    }
    return true;
}

struct HistoryBinding {
    std::shared_ptr<SP::History::UndoableSpace> undo;
    std::string root;
    std::string metrics_root;
    std::uint64_t undo_total = 0;
    std::uint64_t redo_total = 0;
    std::uint64_t undo_failures = 0;
    std::uint64_t redo_failures = 0;
    bool buttons_enabled = false;
    std::uint64_t buttons_enabled_last_change_ns = 0;
    std::string state = "pending";
    std::uint64_t state_timestamp_ns = 0;
    std::string last_error_context;
    std::string last_error_message;
    std::string last_error_code;
    std::uint64_t last_error_timestamp_ns = 0;
};

auto make_history_card(HistoryBinding const& binding)
    -> SP::UI::Declarative::HistoryBindingTelemetryCard {
    SP::UI::Declarative::HistoryBindingTelemetryCard card{};
    card.state = binding.state;
    card.state_timestamp_ns = binding.state_timestamp_ns;
    card.buttons_enabled = binding.buttons_enabled;
    card.buttons_enabled_last_change_ns = binding.buttons_enabled_last_change_ns;
    card.undo_total = binding.undo_total;
    card.undo_failures_total = binding.undo_failures;
    card.redo_total = binding.redo_total;
    card.redo_failures_total = binding.redo_failures;
    card.last_error_context = binding.last_error_context;
    card.last_error_message = binding.last_error_message;
    card.last_error_code = binding.last_error_code;
    card.last_error_timestamp_ns = binding.last_error_timestamp_ns;
    return card;
}

auto publish_history_binding_card(SP::PathSpace& space, HistoryBinding const& binding) -> void {
    if (binding.metrics_root.empty()) {
        return;
    }
    write_history_metric(space, binding.metrics_root, "card", make_history_card(binding));
}

auto set_binding_state(SP::PathSpace& space,
                       HistoryBinding& binding,
                       std::string_view state) -> void {
    if (binding.metrics_root.empty()) {
        return;
    }
    auto timestamp = now_timestamp_ns();
    binding.state.assign(state.begin(), state.end());
    binding.state_timestamp_ns = timestamp;
    write_history_metric(space, binding.metrics_root, "state", std::string{state});
    write_history_metric(space, binding.metrics_root, "state_timestamp_ns", timestamp);
    publish_history_binding_card(space, binding);
}

auto set_binding_buttons_enabled(SP::PathSpace& space,
                                 HistoryBinding& binding,
                                 bool enabled) -> void {
    if (binding.metrics_root.empty()) {
        return;
    }
    auto timestamp = now_timestamp_ns();
    binding.buttons_enabled = enabled;
    binding.buttons_enabled_last_change_ns = timestamp;
    write_history_metric(space, binding.metrics_root, "buttons_enabled", enabled);
    write_history_metric(space, binding.metrics_root, "buttons_enabled_last_change_ns", timestamp);
    publish_history_binding_card(space, binding);
}

auto make_history_binding(SP::PathSpace& space, std::string root_path) -> SP::Expected<HistoryBinding> {
    auto metrics_root = history_metrics_root(root_path);
    record_history_state(space, metrics_root, "binding");
    auto upstream = std::shared_ptr<SP::PathSpaceBase>(&space, [](SP::PathSpaceBase*) {});
    auto alias = std::make_unique<SP::PathAlias>(upstream, "/");
    SP::History::HistoryOptions defaults{};
    defaults.allowNestedUndo = true;
    defaults.maxEntries = 1024;
    defaults.ramCacheEntries = 64;
    defaults.useMutationJournal = true;
    auto undo_space = std::make_shared<SP::History::UndoableSpace>(std::move(alias), defaults);
    auto enable = undo_space->enableHistory(SP::ConcretePathStringView{root_path});
    if (!enable) {
        record_history_state(space, metrics_root, "error");
        record_history_error(space, metrics_root, "UndoableSpace::enableHistory", &enable.error());
        return std::unexpected(enable.error());
    }
    HistoryBinding binding{
        .undo = std::move(undo_space),
        .root = std::move(root_path),
        .metrics_root = std::move(metrics_root),
    };
    binding.buttons_enabled = false;
    binding.buttons_enabled_last_change_ns = now_timestamp_ns();
    set_binding_state(space, binding, "ready");
    return binding;
}

struct PaintUiBindings {
    std::shared_ptr<std::string> paint_widget_path;
    std::shared_ptr<std::string> status_label_path;
    std::shared_ptr<std::string> brush_label_path;
    std::shared_ptr<std::string> undo_button_path;
    std::shared_ptr<std::string> redo_button_path;
    std::shared_ptr<std::shared_ptr<HistoryBinding>> history_binding;
    std::shared_ptr<BrushState> brush_state;
};

constexpr auto kControlsStackChildren =
    std::to_array<std::string_view>({"status_label", "brush_label", "brush_slider", "palette", "actions"});
constexpr auto kActionsStackChildren = std::to_array<std::string_view>({"undo_button", "redo_button"});

auto set_history_buttons_enabled(SP::PathSpace& space,
                                 PaintUiBindings const& bindings,
                                 bool enabled) -> void {
    auto binding_ptr = bindings.history_binding ? *bindings.history_binding : std::shared_ptr<HistoryBinding>{};
    std::string metrics_root;
    if (binding_ptr && !binding_ptr->metrics_root.empty()) {
        metrics_root = binding_ptr->metrics_root;
    } else if (bindings.paint_widget_path && !bindings.paint_widget_path->empty()) {
        metrics_root = history_metrics_root(*bindings.paint_widget_path);
    }
    if (binding_ptr) {
        if (binding_ptr->buttons_enabled != enabled) {
            set_binding_buttons_enabled(space, *binding_ptr, enabled);
        }
    } else {
        auto timestamp = now_timestamp_ns();
        write_history_metric(space, metrics_root, "buttons_enabled", enabled);
        write_history_metric(space, metrics_root, "buttons_enabled_last_change_ns", timestamp);
    }
    auto update = [&](std::shared_ptr<std::string> const& target, std::string_view name) {
        if (!target || target->empty()) {
            return;
        }
        auto widget_path = SP::UI::Builders::WidgetPath{*target};
        auto status = SP::UI::Declarative::Button::SetEnabled(space, widget_path, enabled);
        if (!status) {
            auto context = std::string{"Button::SetEnabled("};
            context.append(name);
            context.push_back(')');
            log_error(status, context);
        }
    };
    update(bindings.undo_button_path, "undo");
    update(bindings.redo_button_path, "redo");
}

auto build_controls_fragment(PaintUiBindings const& bindings,
                             PaintControlsNS::PaintLayoutMetrics const& layout,
                             SP::UI::Builders::Widgets::WidgetTheme const& theme)
    -> SP::UI::Declarative::WidgetFragment {
    using namespace PaintControlsNS;
    SP::UI::Declarative::Stack::Args controls{};
    controls.style.axis = SP::UI::Builders::Widgets::StackAxis::Vertical;
    controls.style.spacing = std::max(10.0f, layout.controls_spacing * 0.6f);
    controls.style.align_cross = SP::UI::Builders::Widgets::StackAlignCross::Stretch;
    controls.style.width = layout.controls_width;
    controls.style.height = layout.canvas_height;
    controls.style.padding_main_start = layout.controls_padding_main;
    controls.style.padding_main_end = layout.controls_padding_main;
    controls.style.padding_cross_start = layout.controls_padding_cross;
    controls.style.padding_cross_end = layout.controls_padding_cross;

    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "status_label",
        .fragment = SP::UI::Declarative::Label::Fragment({
            .text = "Pick a color and drag on the canvas",
            .typography = MakeTypography(24.0f * layout.controls_scale,
                                         30.0f * layout.controls_scale),
            .color = {0.92f, 0.94f, 0.98f, 1.0f},
        }),
    });

    auto brush_state = bindings.brush_state ? bindings.brush_state : std::make_shared<BrushState>();
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "brush_label",
        .fragment = SP::UI::Declarative::Label::Fragment({
            .text = format_brush_state(brush_state->size, brush_state->color),
            .typography = MakeTypography(20.0f * layout.controls_scale,
                                         26.0f * layout.controls_scale),
            .color = {0.82f, 0.86f, 0.92f, 1.0f},
        }),
    });

    BrushSliderConfig slider_config{
        .layout = layout,
        .brush_state = brush_state,
        .minimum = 1.0f,
        .maximum = 64.0f,
        .step = 1.0f,
        .on_change = [bindings](SP::UI::Declarative::SliderContext& ctx, float value) {
            if (bindings.brush_state) {
                bindings.brush_state->size = value;
            }
            auto paint_root = bindings.paint_widget_path ? *bindings.paint_widget_path : std::string{};
            if (!paint_root.empty() && bindings.brush_state) {
                auto status = apply_brush_size(ctx.space, paint_root, bindings.brush_state->size);
                log_error(status, "apply_brush_size");
            }
            auto brush_label = bindings.brush_label_path ? *bindings.brush_label_path : std::string{};
            if (!brush_label.empty() && bindings.brush_state) {
                auto label_path = SP::UI::Builders::WidgetPath{brush_label};
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              label_path,
                                                              format_brush_state(bindings.brush_state->size,
                                                                                 bindings.brush_state->color)),
                          "Label::SetText");
            }
            auto status_label = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_label.empty()) {
                auto label_path = SP::UI::Builders::WidgetPath{status_label};
                std::ostringstream message;
                message << "Brush size adjusted to " << std::lround(value) << " px";
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              label_path,
                                                              message.str()),
                          "Label::SetText");
            }
        },
    };
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "brush_slider",
        .fragment = BuildBrushSliderFragment(slider_config),
    });

    auto palette_entries = BuildDefaultPaletteEntries(theme);
    PaletteComponentConfig palette_config{
        .layout = layout,
        .theme = theme,
        .entries = std::span<const PaletteEntry>(palette_entries.data(), palette_entries.size()),
        .brush_state = brush_state,
        .on_select = [bindings](SP::UI::Declarative::ButtonContext& ctx, PaletteEntry const& entry) {
            if (bindings.brush_state) {
                bindings.brush_state->color = entry.color;
            }
            auto paint_root = bindings.paint_widget_path ? *bindings.paint_widget_path : std::string{};
            if (!paint_root.empty()) {
                auto status = apply_brush_color(ctx.space, paint_root, entry.color);
                log_error(status, "apply_brush_color");
            }
            auto brush_label = bindings.brush_label_path ? *bindings.brush_label_path : std::string{};
            if (!brush_label.empty() && bindings.brush_state) {
                auto brush_path = SP::UI::Builders::WidgetPath{brush_label};
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              brush_path,
                                                              format_brush_state(bindings.brush_state->size,
                                                                                 bindings.brush_state->color)),
                          "Label::SetText");
            }
            auto status_path = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_path.empty()) {
                auto widget_path = SP::UI::Builders::WidgetPath{status_path};
                std::ostringstream message;
                message << "Selected " << entry.label << " paint";
                log_error(SP::UI::Declarative::Label::SetText(ctx.space, widget_path, message.str()),
                          "Label::SetText");
            }
        },
    };
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "palette",
        .fragment = BuildPaletteFragment(palette_config),
    });

    HistoryActionsConfig actions_config{
        .layout = layout,
        .on_action = [bindings](SP::UI::Declarative::ButtonContext& ctx, HistoryAction action) {
            auto binding_ptr = bindings.history_binding ? *bindings.history_binding : std::shared_ptr<HistoryBinding>{};
            if (!binding_ptr) {
                std::cerr << "paint_example: history binding missing for "
                          << (action == HistoryAction::Undo ? "undo" : "redo")
                          << " button\n";
                auto widget_path = bindings.paint_widget_path ? *bindings.paint_widget_path : std::string{};
                auto metrics_root = history_metrics_root(widget_path);
                record_history_state(ctx.space, metrics_root, "missing");
                SP::Error missing_error{SP::Error::Code::UnknownError, "history_binding_missing"};
                record_history_error(ctx.space,
                                     metrics_root,
                                     action == HistoryAction::Undo ? "UndoableSpace::undo"
                                                                    : "UndoableSpace::redo",
                                     &missing_error);
                return;
            }
            auto root = SP::ConcretePathStringView{binding_ptr->root};
            auto update_action_metrics = [&](bool success) {
                if (action == HistoryAction::Undo) {
                    if (success) {
                        ++binding_ptr->undo_total;
                        write_history_metric(ctx.space,
                                             binding_ptr->metrics_root,
                                             "undo_total",
                                             binding_ptr->undo_total);
                    } else {
                        ++binding_ptr->undo_failures;
                        write_history_metric(ctx.space,
                                             binding_ptr->metrics_root,
                                             "undo_failures_total",
                                             binding_ptr->undo_failures);
                    }
                } else {
                    if (success) {
                        ++binding_ptr->redo_total;
                        write_history_metric(ctx.space,
                                             binding_ptr->metrics_root,
                                             "redo_total",
                                             binding_ptr->redo_total);
                    } else {
                        ++binding_ptr->redo_failures;
                        write_history_metric(ctx.space,
                                             binding_ptr->metrics_root,
                                             "redo_failures_total",
                                             binding_ptr->redo_failures);
                    }
                }
                publish_history_binding_card(ctx.space, *binding_ptr);
            };
            SP::Expected<void> status = action == HistoryAction::Undo ? binding_ptr->undo->undo(root)
                                                                      : binding_ptr->undo->redo(root);
            if (!status) {
                update_action_metrics(false);
                log_error(status, action == HistoryAction::Undo ? "UndoableSpace::undo" : "UndoableSpace::redo");
                set_binding_state(ctx.space, *binding_ptr, "error");

                auto error_info = record_history_error(ctx.space,
                                                       binding_ptr->metrics_root,
                                                       action == HistoryAction::Undo ? "UndoableSpace::undo"
                                                                                      : "UndoableSpace::redo",
                                                       &status.error());
                binding_ptr->last_error_context = error_info.context;
                binding_ptr->last_error_message = error_info.message;
                binding_ptr->last_error_code = error_info.code;
                binding_ptr->last_error_timestamp_ns = error_info.timestamp_ns;
                publish_history_binding_card(ctx.space, *binding_ptr);
                return;
            }
            update_action_metrics(true);
            set_binding_state(ctx.space, *binding_ptr, "ready");
            auto status_label = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_label.empty()) {
                auto status_path = SP::UI::Builders::WidgetPath{status_label};
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              status_path,
                                                              action == HistoryAction::Undo ? "Undo applied"
                                                                                           : "Redo applied"),
                          "Label::SetText");
            }
        },
    };
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "actions",
        .fragment = BuildHistoryActionsFragment(actions_config),
    });

    EnsureActivePanel(controls);
    return SP::UI::Declarative::Stack::Fragment(std::move(controls));
}

auto log_expected_error(std::string const& context, SP::Error const& error) -> void {
    std::cerr << "paint_example: " << context << " error (code=" << static_cast<int>(error.code) << ")";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (options.screenshot_compare_path && !options.screenshot_path) {
        std::cerr << "paint_example: --screenshot-compare requires --screenshot\n";
        return 1;
    }
    if (options.screenshot_diff_path && !options.screenshot_compare_path) {
        std::cerr << "paint_example: --screenshot-diff requires --screenshot-compare\n";
        return 1;
    }

    if (!options.screenshot_metrics_path) {
        if (auto metrics_env = read_env_string("PAINT_EXAMPLE_METRICS_JSON")) {
            options.screenshot_metrics_path = std::filesystem::path{*metrics_env};
        }
    }

    auto absolutize_if_present = [](std::optional<std::filesystem::path>& candidate) {
        if (!candidate) {
            return;
        }
        std::error_code ec;
        auto resolved = std::filesystem::absolute(*candidate, ec);
        if (!ec) {
            candidate = resolved;
        }
    };
    absolutize_if_present(options.screenshot_path);
    absolutize_if_present(options.screenshot_compare_path);
    absolutize_if_present(options.screenshot_diff_path);
    absolutize_if_present(options.screenshot_metrics_path);
    absolutize_if_present(options.gpu_texture_path);

    auto baseline_version_env = read_env_string("PAINT_EXAMPLE_BASELINE_VERSION");
    auto baseline_tag_env = read_env_string("PAINT_EXAMPLE_BASELINE_TAG");
    auto baseline_sha_env = read_env_string("PAINT_EXAMPLE_BASELINE_SHA256");
    BaselineTelemetryInputs baseline_telemetry{};
    baseline_telemetry.tolerance = options.screenshot_max_mean_error;
    std::optional<int> baseline_manifest_revision;
    if (baseline_version_env) {
        auto parsed_revision = parse_env_int(*baseline_version_env);
        if (!parsed_revision) {
            std::cerr << "paint_example: invalid PAINT_EXAMPLE_BASELINE_VERSION='" << *baseline_version_env << "'\n";
            return 1;
        }
        if (*parsed_revision < kRequiredBaselineManifestRevision) {
            std::cerr << "paint_example: baseline manifest revision " << *parsed_revision
                      << " is older than required revision " << kRequiredBaselineManifestRevision << "\n";
            std::cerr << "Re-run scripts/paint_example_capture.py to refresh the baseline manifest.\n";
            return 1;
        }
        baseline_manifest_revision = parsed_revision;
        baseline_telemetry.manifest_revision = parsed_revision;
        std::cout << "paint_example: baseline manifest revision " << *parsed_revision
                  << " (required " << kRequiredBaselineManifestRevision << ")\n";
    }
    if (baseline_tag_env) {
        baseline_telemetry.tag = *baseline_tag_env;
    }
    if (baseline_sha_env) {
        baseline_telemetry.sha256 = *baseline_sha_env;
    }
    if (auto width_env = read_env_string("PAINT_EXAMPLE_BASELINE_WIDTH")) {
        if (auto value = parse_env_int(*width_env)) {
            baseline_telemetry.width = *value;
        }
    }
    if (auto height_env = read_env_string("PAINT_EXAMPLE_BASELINE_HEIGHT")) {
        if (auto value = parse_env_int(*height_env)) {
            baseline_telemetry.height = *value;
        }
    }
    if (auto renderer_env = read_env_string("PAINT_EXAMPLE_BASELINE_RENDERER")) {
        baseline_telemetry.renderer = *renderer_env;
    }
    if (auto captured_env = read_env_string("PAINT_EXAMPLE_BASELINE_CAPTURED_AT")) {
        baseline_telemetry.captured_at = *captured_env;
    }
    if (auto commit_env = read_env_string("PAINT_EXAMPLE_BASELINE_COMMIT")) {
        baseline_telemetry.commit = *commit_env;
    }
    if (auto notes_env = read_env_string("PAINT_EXAMPLE_BASELINE_NOTES")) {
        baseline_telemetry.notes = *notes_env;
    }
    if (auto tolerance_env = read_env_string("PAINT_EXAMPLE_BASELINE_TOLERANCE")) {
        if (auto parsed_tolerance = parse_env_double(*tolerance_env)) {
            baseline_telemetry.tolerance = *parsed_tolerance;
        }
    }

    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "paint_example: failed to launch declarative runtime\n";
        return 1;
    }

    publish_baseline_metrics(space, baseline_telemetry);

    auto app = SP::App::Create(space,
                               "paint_example",
                               {.title = "Declarative Paint"});
    if (!app) {
        std::cerr << "paint_example: failed to create app\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};
    auto theme_selection = SP::UI::Builders::Widgets::LoadTheme(space, app_root_view, "");
    auto active_theme = theme_selection.theme;

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "paint_window";
    window_opts.title = "Declarative Paint Surface";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        std::cerr << "paint_example: failed to create window\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "paint_scene";
    scene_opts.description = "Declarative paint scene";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        std::cerr << "paint_example: failed to create scene\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto scene_result = *scene;

    auto bootstrap = build_bootstrap_from_window(space,
                                                 app_root_view,
                                                 window->path,
                                                 window->view_name);
    if (!bootstrap) {
        log_expected_error("failed to prepare presenter bootstrap", bootstrap.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    if (auto capture_status = set_capture_framebuffer_enabled(space,
                                                              window->path,
                                                              window->view_name,
                                                              true);
        !capture_status) {
        log_expected_error("enable framebuffer capture", capture_status.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    bootstrap->present_policy.capture_framebuffer = true;

    auto bind_scene = SP::UI::Builders::Surface::SetScene(space, (*bootstrap).surface, scene_result.path);
    if (!bind_scene) {
        log_expected_error("Surface::SetScene", bind_scene.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "paint_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "paint_example");
    auto pointer_devices = std::vector<std::string>{std::string{kPointerDevice}};
    auto keyboard_devices = std::vector<std::string>{std::string{kKeyboardDevice}};
    subscribe_window_devices(space,
                             window->path,
                             std::span<const std::string>(pointer_devices),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices));

    auto window_view_path = window_view_base(window->path, window->view_name);
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto brush_state = std::make_shared<BrushState>();

    bool screenshot_mode = options.screenshot_path.has_value();
    bool debug_layout_logging = std::getenv("PAINT_EXAMPLE_DEBUG_LAYOUT") != nullptr;

    auto layout_metrics = PaintControlsNS::ComputeLayoutMetrics(options.width, options.height);

    PaintUiBindings bindings{
        .paint_widget_path = std::make_shared<std::string>(),
        .status_label_path = std::make_shared<std::string>(),
        .brush_label_path = std::make_shared<std::string>(),
        .undo_button_path = std::make_shared<std::string>(),
        .redo_button_path = std::make_shared<std::string>(),
        .history_binding = std::make_shared<std::shared_ptr<HistoryBinding>>(),
        .brush_state = brush_state,
    };

    SP::UI::Declarative::PaintSurface::Args paint_args{};
    paint_args.brush_size = brush_state->size;
    paint_args.brush_color = brush_state->color;
    paint_args.buffer_width = static_cast<std::uint32_t>(std::max(1.0f, layout_metrics.canvas_width));
    paint_args.buffer_height = static_cast<std::uint32_t>(std::max(1.0f, layout_metrics.canvas_height));
    paint_args.gpu_enabled = options.gpu_smoke || screenshot_mode;
    bool const paint_gpu_enabled = paint_args.gpu_enabled;
    paint_args.on_draw = [status_label_path = bindings.status_label_path](SP::UI::Declarative::PaintSurfaceContext& ctx) {
        auto label_path = status_label_path ? *status_label_path : std::string{};
        if (label_path.empty()) {
            return;
        }
        auto widget_path = SP::UI::Builders::WidgetPath{label_path};
        log_error(SP::UI::Declarative::Label::SetText(ctx.space, widget_path, "Stroke recorded"),
                  "Label::SetText");
    };

    auto controls_fragment = build_controls_fragment(bindings, layout_metrics, active_theme);

    SP::UI::Declarative::Stack::Args root_stack{};
    root_stack.active_panel = "canvas_panel";
    root_stack.style.axis = SP::UI::Builders::Widgets::StackAxis::Horizontal;
    root_stack.style.spacing = layout_metrics.controls_spacing;
    root_stack.style.align_cross = SP::UI::Builders::Widgets::StackAlignCross::Start;
    root_stack.style.padding_main_start = layout_metrics.padding_x;
    root_stack.style.padding_main_end = layout_metrics.padding_x;
    root_stack.style.padding_cross_start = layout_metrics.padding_y;
    root_stack.style.padding_cross_end = layout_metrics.padding_y;
    root_stack.style.width = layout_metrics.controls_width + layout_metrics.canvas_width
                             + layout_metrics.controls_spacing + layout_metrics.padding_x * 2.0f;
    root_stack.style.height = layout_metrics.canvas_height + layout_metrics.padding_y * 2.0f;

    root_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "controls_panel",
        .fragment = std::move(controls_fragment),
    });
    root_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "canvas_panel",
        .fragment = SP::UI::Declarative::PaintSurface::Fragment(paint_args),
    });

    auto ui_stack = SP::UI::Declarative::Stack::Create(space,
                                                       window_view,
                                                       "ui_stack",
                                                       std::move(root_stack));
    if (!ui_stack) {
        log_expected_error("create UI stack", ui_stack.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto stack_root = ui_stack->getPath();
    auto controls_root = stack_root + "/children/controls_panel";
    *bindings.status_label_path = controls_root + "/children/status_label";
    *bindings.brush_label_path = controls_root + "/children/brush_label";
    *bindings.paint_widget_path = stack_root + "/children/canvas_panel";
    auto paint_widget_path = *bindings.paint_widget_path;

    initialize_history_metrics(space, paint_widget_path);

    if (!wait_for_stack_children(space,
                                 controls_root,
                                 std::span<const std::string_view>(kControlsStackChildren),
                                 std::chrono::milliseconds(1500),
                                 debug_layout_logging)) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto actions_root = controls_root + "/children/actions";
    if (!wait_for_stack_children(space,
                                 actions_root,
                                 std::span<const std::string_view>(kActionsStackChildren),
                                 std::chrono::milliseconds(1000),
                                 debug_layout_logging)) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    *bindings.undo_button_path = actions_root + "/children/undo_button";
    *bindings.redo_button_path = actions_root + "/children/redo_button";

    auto history_binding_result = make_history_binding(space, paint_widget_path);
    if (!history_binding_result) {
        log_expected_error("failed to enable UndoableSpace history", history_binding_result.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto history_binding = std::make_shared<HistoryBinding>(std::move(*history_binding_result));
    *bindings.history_binding = history_binding;
    set_history_buttons_enabled(space, bindings, true);
    auto paint_widget = SP::UI::Builders::WidgetPath{paint_widget_path};

    auto widget_count = count_window_widgets(space, window_view_path);
    auto scene_widgets_root = make_scene_widgets_root(scene_result.path, window->path, window->view_name);
    if (!wait_for_widget_buckets(space,
                                 scene_result.path,
                                 widget_count,
                                 std::chrono::seconds(5))
        || !wait_for_scene_widgets(space,
                                   scene_widgets_root,
                                   widget_count,
                                   std::chrono::seconds(5))) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto latest_revision = wait_for_scene_revision(space, scene_result.path, std::chrono::seconds(3));
    if (!latest_revision) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    if (options.gpu_smoke) {
        GpuSmokeConfig smoke_config{};
        smoke_config.dump_path = options.gpu_texture_path;
        if (!run_gpu_smoke(space, paint_widget_path, smoke_config)) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        if (!screenshot_mode) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 0;
        }
    }

    if (screenshot_mode) {
        bool debug_logging = std::getenv("PAINT_EXAMPLE_DEBUG") != nullptr;
        auto strokes_preview = render_scripted_strokes_image(options.width,
                                                             options.height,
                                                             brush_state->size * 0.5f,
                                                             brush_state->color,
                                                             layout_metrics);
        auto log_lifecycle_state = [&](std::string_view phase) {
            if (!debug_logging) {
                return;
            }
            auto scene_base = std::string(scene_result.path.getPath());
            auto revision = space.read<std::uint64_t, std::string>(scene_base + "/current_revision");
            auto metrics_base = scene_base + "/runtime/lifecycle/metrics";
            auto processed = space.read<std::uint64_t, std::string>(metrics_base + "/events_processed_total");
            auto widgets_with_buckets =
                space.read<std::uint64_t, std::string>(metrics_base + "/widgets_with_buckets");
            auto last_error = space.read<std::string, std::string>(metrics_base + "/last_error");
            std::cerr << "paint_example: lifecycle[" << phase << "] revision "
                      << revision.value_or(0) << " processed "
                      << processed.value_or(0) << " widgets_with_buckets "
                      << widgets_with_buckets.value_or(0);
            if (last_error && !last_error->empty()) {
                std::cerr << " last_error " << *last_error;
            }
            std::cerr << "\n";
        };
        log_lifecycle_state("before_playback");
        if (!playback_scripted_strokes(space, paint_widget_path)) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        log_lifecycle_state("after_playback");
        if (debug_logging) {
            auto records = SP::UI::Declarative::PaintRuntime::LoadStrokeRecords(space, paint_widget_path);
            if (records) {
                std::cerr << "paint_example: stroke records " << records->size() << "\n";
            } else {
                std::cerr << "paint_example: stroke record load failed\n";
            }
            auto descriptor_debug = SP::UI::Declarative::LoadWidgetDescriptor(space, paint_widget);
            if (descriptor_debug) {
                auto bucket_debug = SP::UI::Declarative::BuildWidgetBucket(*descriptor_debug);
                if (bucket_debug) {
                    std::cerr << "paint_example: bucket debug strokes "
                              << bucket_debug->stroke_points.size()
                              << " commands " << bucket_debug->command_kinds.size() << "\n";
                    std::size_t payload_index = 0;
                    for (std::size_t i = 0; i < bucket_debug->command_kinds.size(); ++i) {
                        auto kind = static_cast<SP::UI::Scene::DrawCommandKind>(bucket_debug->command_kinds[i]);
                        if (kind == SP::UI::Scene::DrawCommandKind::Stroke) {
                            SP::UI::Scene::StrokeCommand cmd{};
                            if (payload_index + sizeof(cmd) <= bucket_debug->command_payload.size()) {
                                std::memcpy(&cmd,
                                            bucket_debug->command_payload.data() + payload_index,
                                            sizeof(cmd));
                                std::cerr << "paint_example: stroke command " << i
                                          << " offset " << cmd.point_offset
                                          << " count " << cmd.point_count
                                          << " buffer points " << bucket_debug->stroke_points.size()
                                          << "\n";
                            }
                        }
                        payload_index += SP::UI::Scene::payload_size_bytes(kind);
                    }
                } else {
                    std::cerr << "paint_example: bucket debug build failed\n";
                }
            } else {
                std::cerr << "paint_example: bucket debug descriptor load failed\n";
            }
        }
        auto require_live_capture = options.screenshot_require_present
                                    || options.screenshot_compare_path.has_value();
        ScreenshotRunMetrics run_metrics{};
        run_metrics.timestamp_ns = now_timestamp_ns();
        run_metrics.require_present = require_live_capture;
        if (options.screenshot_path) {
            run_metrics.screenshot_path = options.screenshot_path->string();
        }
        auto refresh_diff_reference = [&]() {
            if (!options.screenshot_diff_path) {
                run_metrics.diff_path.reset();
                return;
            }
            std::error_code diff_error;
            if (std::filesystem::exists(*options.screenshot_diff_path, diff_error) && !diff_error) {
                run_metrics.diff_path = options.screenshot_diff_path->string();
                return;
            }
            run_metrics.diff_path.reset();
        };
        auto emit_metrics = [&](std::string_view status) {
            refresh_diff_reference();
            run_metrics.status.assign(status.begin(), status.end());
            record_last_run_metrics(space, run_metrics);
            if (options.screenshot_metrics_path) {
                write_metrics_snapshot_json(*options.screenshot_metrics_path, baseline_telemetry, run_metrics);
            }
        };
        bool hardware_capture = true;
        if (paint_gpu_enabled) {
            hardware_capture = wait_for_paint_capture_ready(space,
                                                            paint_widget_path,
                                                            std::chrono::milliseconds(2000));
            if (!hardware_capture) {
                std::cerr << "paint_example: paint GPU never became Ready before capture\n";
            }
        }
        if (hardware_capture) {
            auto warmup = capture_present_frame(space,
                                                window->path,
                                                window->view_name,
                                                std::chrono::milliseconds(1500));
            if (!warmup) {
                hardware_capture = false;
                std::cerr << "paint_example: Window::Present warmup failed\n";
            } else {
                if (auto capture_revision = wait_for_scene_revision(space,
                                                                    scene_result.path,
                                                                    std::chrono::seconds(10),
                                                                    latest_revision)) {
                    latest_revision = capture_revision;
                } else {
                    std::cerr << "paint_example: timed out waiting for scene revision > "
                              << latest_revision.value_or(0) << ", capturing latest published frame\n";
                }
                bool captured = capture_window_screenshot(space,
                                                          window->path,
                                                          window->view_name,
                                                          options.width,
                                                          options.height,
                                                          *options.screenshot_path,
                                                          std::chrono::milliseconds(1500));
                if (!captured) {
                    hardware_capture = false;
                    std::cerr << "paint_example: Window::Present capture failed\n";
                }
            }
        }
        if (!hardware_capture) {
            if (require_live_capture) {
                run_metrics.hardware_capture = hardware_capture;
                run_metrics.mean_error.reset();
                run_metrics.max_channel_delta.reset();
                emit_metrics("capture_failed");
                SP::System::ShutdownDeclarativeRuntime(space);
                return 1;
            }
            std::cerr << "paint_example: falling back to software renderer for screenshot\n";
            if (!write_image_png(strokes_preview, *options.screenshot_path)) {
                run_metrics.hardware_capture = hardware_capture;
                run_metrics.mean_error.reset();
                run_metrics.max_channel_delta.reset();
                emit_metrics("software_write_failed");
                SP::System::ShutdownDeclarativeRuntime(space);
                return 1;
            }
        }
        if (hardware_capture) {
            if (!overlay_strokes_onto_screenshot(*options.screenshot_path, strokes_preview, layout_metrics)) {
                std::cerr << "paint_example: stroke overlay failed\n";
            }
        }
        log_lifecycle_state("after_capture_attempt");
        if (options.screenshot_compare_path) {
            auto diff = compare_screenshots(*options.screenshot_compare_path,
                                            *options.screenshot_path,
                                            options.screenshot_diff_path);
            if (!diff) {
                run_metrics.hardware_capture = hardware_capture;
                run_metrics.mean_error.reset();
                run_metrics.max_channel_delta.reset();
                emit_metrics("compare_failed");
                SP::System::ShutdownDeclarativeRuntime(space);
                return 1;
            }
            if (diff->mean_error > options.screenshot_max_mean_error) {
                std::cerr << "paint_example: screenshot mean error " << diff->mean_error
                          << " exceeds threshold " << options.screenshot_max_mean_error
                          << " (max channel delta " << static_cast<int>(diff->max_channel_delta)
                          << ")\n";
                run_metrics.hardware_capture = hardware_capture;
                run_metrics.mean_error = diff->mean_error;
                run_metrics.max_channel_delta = diff->max_channel_delta;
                emit_metrics("mismatch");
                SP::System::ShutdownDeclarativeRuntime(space);
                return 1;
            }
            std::cout << "paint_example: screenshot baseline matched (mean error "
                      << diff->mean_error << ", max channel delta "
                      << static_cast<int>(diff->max_channel_delta) << ")\n";
            run_metrics.mean_error = diff->mean_error;
            run_metrics.max_channel_delta = diff->max_channel_delta;
        } else {
            run_metrics.mean_error.reset();
            run_metrics.max_channel_delta.reset();
        }
        run_metrics.hardware_capture = hardware_capture;
        emit_metrics(options.screenshot_compare_path ? std::string_view{"match"}
                                                     : std::string_view{"captured"});
        std::cout << "paint_example: saved screenshot to " << options.screenshot_path->string() << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    if (options.headless) {
        std::cout << "paint_example: headless mode enabled, declarative widgets mounted at\n"
                  << "  " << paint_widget_path << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    PresentLoopHooks hooks{};

    run_present_loop(space,
                     window->path,
                     window->view_name,
                     *bootstrap,
                     options.width,
                     options.height,
                     hooks);

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
