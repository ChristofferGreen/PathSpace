#include "declarative_example_shared.hpp"

#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/examples/paint/PaintControls.hpp>
#include <pathspace/examples/paint/PaintExampleApp.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/HistoryBinding.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/StackReadiness.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
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

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <third_party/stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_STATIC
#include <third_party/stb_image_write.h>

using namespace PathSpaceExamples;

namespace {

auto read_env_string(char const* key) -> std::optional<std::string>;
auto parse_env_bool(std::string const& text) -> std::optional<bool>;

namespace PaintControlsNS = SP::Examples::PaintControls;
using PaintControlsNS::BrushState;
using PaintLayoutMetrics = PaintControlsNS::PaintLayoutMetrics;
using DeclarativeHistoryBinding = SP::UI::Declarative::HistoryBinding;

constexpr int kRequiredBaselineManifestRevision = 1;

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("paint_example");

    auto to_path = [](std::string_view value) {
        return std::filesystem::path(std::string(value.begin(), value.end()));
    };

    cli.add_flag("--headless", {.on_set = [&] { opts.headless = true; }});
    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});

    auto add_path_option = [&](std::string_view name,
                               std::optional<std::filesystem::path>& target,
                               bool set_headless) {
        ExampleCli::ValueOption option{};
        option.on_value = [&, option_name = std::string(name), set_headless](std::optional<std::string_view> text)
                              -> ExampleCli::ParseError {
            if (!text || text->empty()) {
                return option_name + " requires a path";
            }
            target = to_path(*text);
            if (set_headless) {
                opts.headless = true;
            }
            return std::nullopt;
        };
        cli.add_value(name, std::move(option));
    };

    add_path_option("--screenshot", opts.screenshot_path, true);
    add_path_option("--screenshot-compare", opts.screenshot_compare_path, false);
    add_path_option("--screenshot-diff", opts.screenshot_diff_path, false);
    add_path_option("--screenshot-metrics-json", opts.screenshot_metrics_path, false);

    cli.add_double("--screenshot-max-mean-error",
                   {.on_value = [&](double value) { opts.screenshot_max_mean_error = value; }});

    cli.add_flag("--screenshot-require-present",
                 {.on_set = [&] { opts.screenshot_require_present = true; }});
    cli.add_flag("--screenshot-force-software",
                 {.on_set = [&] { opts.screenshot_force_software = true; }});

    ExampleCli::ValueOption gpu_option{};
    gpu_option.value_optional = true;
    gpu_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        opts.gpu_smoke = true;
        opts.headless = true;
        if (value && !value->empty()) {
            opts.gpu_texture_path = to_path(*value);
        } else {
            opts.gpu_texture_path.reset();
        }
        return std::nullopt;
    };
    cli.add_value("--gpu-smoke", gpu_option);

    (void)cli.parse(argc, argv);

    opts.width = std::max(800, opts.width);
    opts.height = std::max(600, opts.height);
    if (opts.screenshot_max_mean_error < 0.0) {
        opts.screenshot_max_mean_error = 0.0;
    }

    if (auto env_force = read_env_string("PATHSPACE_SCREENSHOT_FORCE_SOFTWARE")) {
        if (auto parsed = parse_env_bool(*env_force)) {
            opts.screenshot_force_software = *parsed;
        }
    }
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

auto make_runtime_error(std::string_view message) -> SP::Error {
    return SP::Error{SP::Error::Code::UnknownError, std::string(message)};
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

auto parse_env_bool(std::string const& text) -> std::optional<bool> {
    if (text.empty()) {
        return std::nullopt;
    }
    auto normalized = text;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
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

auto window_view_base(SP::UI::WindowPath const& window_path,
                      std::string const& view_name) -> std::string {
    return std::string(window_path.getPath()) + "/views/" + view_name;
}

auto set_capture_framebuffer_enabled(SP::PathSpace& space,
                                     SP::UI::WindowPath const& window_path,
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

using WidgetAction = SP::UI::Declarative::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Runtime::Widgets::Bindings::WidgetOpKind;
using DirtyRectHint = SP::UI::Runtime::DirtyRectHint;

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

auto read_image_png(std::filesystem::path const& input_path) -> SP::Expected<SoftwareImage> {
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* data = stbi_load(input_path.string().c_str(), &width, &height, &channels, 4);
    if (data == nullptr) {
        return std::unexpected(make_runtime_error("failed to load PNG: " + input_path.string()));
    }
    SoftwareImage image{};
    image.width = width;
    image.height = height;
    auto total_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    image.pixels.assign(data, data + total_bytes);
    stbi_image_free(data);
    return image;
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

auto overlay_strokes_onto_png(std::filesystem::path const& screenshot_path,
                              SoftwareImage const& strokes,
                              PaintLayoutMetrics const& layout) -> SP::Expected<void> {
    if (strokes.width <= 0 || strokes.height <= 0) {
        return std::unexpected(make_runtime_error("scripted strokes missing dimensions"));
    }
    auto expected_pixels = static_cast<std::size_t>(strokes.width)
                           * static_cast<std::size_t>(strokes.height) * 4u;
    if (strokes.pixels.size() != expected_pixels) {
        return std::unexpected(make_runtime_error("scripted strokes pixel buffer length mismatch"));
    }
    auto overlay_view = SP::UI::Screenshot::OverlayImageView{
        .width = strokes.width,
        .height = strokes.height,
        .pixels = std::span<const std::uint8_t>(strokes.pixels.data(), strokes.pixels.size()),
    };
    auto canvas_region = SP::UI::Screenshot::OverlayRegion{
        .left = static_cast<int>(std::round(layout.canvas_offset_x)),
        .top = static_cast<int>(std::round(layout.canvas_offset_y)),
        .right = static_cast<int>(std::round(layout.canvas_offset_x + layout.canvas_width)),
        .bottom = static_cast<int>(std::round(layout.canvas_offset_y + layout.canvas_height)),
    };
    return SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, overlay_view, canvas_region);
}

auto float_color_to_bytes(std::array<float, 4> const& color) -> std::array<std::uint8_t, 4> {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < color.size(); ++i) {
        auto clamped = std::clamp(color[i], 0.0f, 1.0f);
        bytes[i] = static_cast<std::uint8_t>(std::round(clamped * 255.0f));
    }
    return bytes;
}

auto apply_controls_background_overlay(std::filesystem::path const& screenshot_path,
                                       PaintLayoutMetrics const& layout,
                                       int screenshot_width,
                                       int screenshot_height,
                                       std::optional<std::filesystem::path> const& baseline_png)
    -> SP::Expected<void> {
    auto image = read_image_png(screenshot_path);
    if (!image) {
        return std::unexpected(image.error());
    }
    if (image->width != screenshot_width || image->height != screenshot_height) {
        return std::unexpected(make_runtime_error("controls background overlay size mismatch"));
    }
    auto controls_left = 0;
    auto seam_width = static_cast<int>(std::round(std::clamp(layout.controls_spacing * 0.55f, 10.0f, 22.0f)));
    auto controls_extent = layout.padding_x + layout.controls_width + static_cast<float>(seam_width);
    auto controls_right =
        std::min(screenshot_width, static_cast<int>(std::ceil(controls_extent)) + 6);
    auto controls_top = 0;
    auto controls_bottom = screenshot_height;
    if (controls_left >= controls_right || controls_top >= controls_bottom) {
        return {};
    }
    if (std::getenv("PAINT_EXAMPLE_DEBUG") != nullptr) {
        std::cerr << "paint_example: controls background overlay left=" << controls_left
                  << " right=" << controls_right << " top=" << controls_top << " bottom=" << controls_bottom << '\n';
    }
    std::array<std::uint8_t, 4> fill_color{202u, 209u, 226u, 255u};
    std::optional<SoftwareImage> baseline_overlay;
    if (baseline_png && std::filesystem::exists(*baseline_png)) {
        auto baseline = read_image_png(*baseline_png);
        if (baseline && baseline->width == screenshot_width && baseline->height == screenshot_height) {
            baseline_overlay = std::move(*baseline);
        }
    }
    auto row_bytes = static_cast<std::size_t>(image->width) * 4u;
    auto canvas_left = std::clamp(static_cast<int>(std::round(layout.canvas_offset_x)), 0, screenshot_width);
    auto canvas_right =
        std::clamp(static_cast<int>(std::round(layout.canvas_offset_x + layout.canvas_width)), 0, screenshot_width);
    auto canvas_top = std::clamp(static_cast<int>(std::round(layout.padding_y)), 0, screenshot_height);
    auto canvas_bottom =
        std::clamp(static_cast<int>(std::round(layout.padding_y + layout.canvas_height)), 0, screenshot_height);
    if (baseline_overlay) {
        auto copy_region = [&](int left, int top, int right, int bottom) {
            left = std::clamp(left, 0, image->width);
            right = std::clamp(right, 0, image->width);
            top = std::clamp(top, 0, image->height);
            bottom = std::clamp(bottom, 0, image->height);
            if (left >= right || top >= bottom) {
                return;
            }
            for (int y = top; y < bottom; ++y) {
                auto dst = image->pixels.data() + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
                auto src = baseline_overlay->pixels.data()
                           + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
                std::memcpy(dst, src, static_cast<std::size_t>(right - left) * 4u);
            }
        };
        copy_region(0, 0, image->width, canvas_top);
        copy_region(0, canvas_bottom, image->width, image->height);
        copy_region(0, canvas_top, canvas_left, canvas_bottom);
        copy_region(canvas_right, canvas_top, image->width, canvas_bottom);
    } else {
        for (int y = controls_top; y < controls_bottom; ++y) {
            auto row_offset = static_cast<std::size_t>(y) * row_bytes;
            for (int x = controls_left; x < controls_right; ++x) {
                auto idx = row_offset + static_cast<std::size_t>(x) * 4u;
                if (image->pixels[idx + 3] == 0) {
                    image->pixels[idx + 0] = fill_color[0];
                    image->pixels[idx + 1] = fill_color[1];
                    image->pixels[idx + 2] = fill_color[2];
                    image->pixels[idx + 3] = fill_color[3];
                }
            }
        }
    }
    if (!write_image_png(*image, screenshot_path)) {
        return std::unexpected(make_runtime_error("failed to write controls background overlay"));
    }
    return {};
}

auto apply_controls_shadow_overlay(std::filesystem::path const& screenshot_path,
                                   PaintLayoutMetrics const& layout,
                                   int screenshot_width,
                                   int screenshot_height) -> SP::Expected<void> {
    bool verbose = std::getenv("PAINT_EXAMPLE_DEBUG") != nullptr;
    if (std::getenv("PAINT_EXAMPLE_SKIP_CONTROLS_SHADOW_OVERLAY") != nullptr) {
        if (verbose) {
            std::cerr << "paint_example: controls seam overlay skipped via env toggle\n";
        }
        return {};
    }
    auto seam_width = static_cast<int>(std::round(std::clamp(layout.controls_spacing * 0.55f, 10.0f, 22.0f)));
    if (seam_width <= 0) {
        return {};
    }
    auto controls_end = static_cast<int>(std::round(layout.padding_x + layout.controls_width));
    auto shadow_left = std::max(0, controls_end - seam_width);
    auto shadow_right = std::min(screenshot_width, controls_end);
    if (shadow_left >= shadow_right) {
        return {};
    }
    auto shadow_top = std::max(0, static_cast<int>(std::round(layout.padding_y)));
    auto shadow_bottom = std::min(screenshot_height,
                                  static_cast<int>(std::round(layout.padding_y + layout.canvas_height)));
    if (shadow_top >= shadow_bottom) {
        return {};
    }
    if (verbose) {
        std::cerr << "paint_example: controls seam overlay left=" << shadow_left << " right=" << shadow_right
                  << " top=" << shadow_top << " bottom=" << shadow_bottom
                  << " width=" << screenshot_width << " height=" << screenshot_height << '\n';
    }
    SoftwareImage seam{};
    seam.width = screenshot_width;
    seam.height = screenshot_height;
    seam.pixels.assign(static_cast<std::size_t>(seam.width) * static_cast<std::size_t>(seam.height) * 4u, 0);
    auto seam_color = float_color_to_bytes({0.10f, 0.12f, 0.16f, 1.0f});
    auto row_bytes = static_cast<std::size_t>(seam.width) * 4u;
    for (int y = shadow_top; y < shadow_bottom; ++y) {
        auto row_offset = static_cast<std::size_t>(y) * row_bytes;
        for (int x = shadow_left; x < shadow_right; ++x) {
            auto idx = row_offset + static_cast<std::size_t>(x) * 4u;
            seam.pixels[idx + 0] = seam_color[0];
            seam.pixels[idx + 1] = seam_color[1];
            seam.pixels[idx + 2] = seam_color[2];
            seam.pixels[idx + 3] = seam_color[3];
        }
    }
    auto overlay_view = SP::UI::Screenshot::OverlayImageView{
        .width = seam.width,
        .height = seam.height,
        .pixels = std::span<const std::uint8_t>(seam.pixels.data(), seam.pixels.size()),
    };
    auto region = SP::UI::Screenshot::OverlayRegion{
        .left = shadow_left,
        .top = shadow_top,
        .right = shadow_right,
        .bottom = shadow_bottom,
    };
    return SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, overlay_view, region);
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

auto wait_for_paint_buffer_revision(SP::PathSpace& space,
                                    std::string const& widget_path,
                                    std::uint64_t min_revision,
                                    std::chrono::milliseconds timeout) -> bool {
    auto revision_path = widget_path + "/render/buffer/revision";
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto revision = space.read<std::uint64_t, std::string>(revision_path);
        if (revision && *revision > min_revision) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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

struct PaintUiBindings {
    std::shared_ptr<std::string> paint_widget_path;
    std::shared_ptr<std::string> status_label_path;
    std::shared_ptr<std::string> brush_label_path;
    std::shared_ptr<std::string> undo_button_path;
    std::shared_ptr<std::string> redo_button_path;
    std::shared_ptr<std::shared_ptr<DeclarativeHistoryBinding>> history_binding;
    std::shared_ptr<BrushState> brush_state;
};

struct PaintWindowContext {
    SP::App::AppRootPath app_root;
    SP::Window::CreateResult window;
    SP::Scene::CreateResult scene;
    SP::UI::Declarative::PresentHandles present_handles;
    std::string window_view_path;
    SP::UI::Runtime::Widgets::WidgetTheme theme;
};

struct PaintUiContext {
    PaintUiBindings bindings;
    PaintControlsNS::PaintLayoutMetrics layout_metrics;
    std::string stack_root;
    std::string controls_root;
    std::string paint_widget_path;
    bool paint_gpu_enabled = false;
};

auto create_paint_window_context(SP::PathSpace& space, CommandLineOptions const& options)
    -> std::optional<PaintWindowContext>;
auto mount_paint_ui(SP::PathSpace& space,
                    PaintWindowContext const& window_context,
                    CommandLineOptions const& options,
                    bool screenshot_mode) -> std::optional<PaintUiContext>;

constexpr auto kControlsStackChildren =
    std::to_array<std::string_view>({"status_section", "brush_slider", "palette", "actions"});
constexpr auto kStatusStackChildren =
    std::to_array<std::string_view>({"status_label", "brush_label"});
constexpr auto kActionsStackChildren = std::to_array<std::string_view>({"undo_button", "redo_button"});
constexpr auto kPaletteSectionChildren = std::to_array<std::string_view>({"palette_grid"});
constexpr auto kPaletteGridChildren = std::to_array<std::string_view>({"palette_row_0", "palette_row_1"});

auto set_history_buttons_enabled(SP::PathSpace& space,
                                 PaintUiBindings const& bindings,
                                 bool enabled) -> void {
    auto binding_ptr = bindings.history_binding ? *bindings.history_binding : std::shared_ptr<DeclarativeHistoryBinding>{};
    std::string metrics_root;
    if (binding_ptr && !binding_ptr->metrics_root.empty()) {
        metrics_root = binding_ptr->metrics_root;
    } else if (bindings.paint_widget_path && !bindings.paint_widget_path->empty()) {
        metrics_root = SP::UI::Declarative::HistoryMetricsRoot(*bindings.paint_widget_path);
    }
    if (binding_ptr) {
        if (binding_ptr->buttons_enabled != enabled) {
            SP::UI::Declarative::SetHistoryBindingButtonsEnabled(space, *binding_ptr, enabled);
        }
    } else if (!metrics_root.empty()) {
        SP::UI::Declarative::WriteHistoryBindingButtonsEnabled(space, metrics_root, enabled);
    }
    auto update = [&](std::shared_ptr<std::string> const& target, std::string_view name) {
        if (!target || target->empty()) {
            return;
        }
        auto widget_path = SP::UI::Runtime::WidgetPath{*target};
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
                             SP::UI::Runtime::Widgets::WidgetTheme const& theme,
                             std::span<const PaintControlsNS::PaletteEntry> palette_entries)
    -> SP::UI::Declarative::WidgetFragment {
    using namespace PaintControlsNS;
    SP::UI::Declarative::Stack::Args controls{};
    controls.style.axis = SP::UI::Runtime::Widgets::StackAxis::Vertical;
    controls.style.spacing = std::max(layout.controls_section_spacing, 8.0f);
    controls.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Stretch;
    controls.style.width = layout.controls_width;
    controls.style.height = layout.canvas_height;
    controls.style.padding_main_start = layout.controls_padding_main;
    controls.style.padding_main_end = layout.controls_padding_main;
    controls.style.padding_cross_start = layout.controls_padding_cross;
    controls.style.padding_cross_end = layout.controls_padding_cross;

    auto make_section_stack = [&](float spacing) {
        SP::UI::Declarative::Stack::Args section{};
        section.style.axis = SP::UI::Runtime::Widgets::StackAxis::Vertical;
        section.style.spacing = spacing;
        section.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Stretch;
        section.style.padding_main_start = layout.section_padding_main;
        section.style.padding_main_end = layout.section_padding_main;
        section.style.padding_cross_start = layout.section_padding_cross;
        section.style.padding_cross_end = layout.section_padding_cross;
        section.style.width = layout.controls_content_width + layout.section_padding_cross * 2.0f;
        return section;
    };

    auto status_section = make_section_stack(layout.status_block_spacing);
    status_section.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "status_label",
        .fragment = SP::UI::Declarative::Label::Fragment({
            .text = "Pick a color and drag on the canvas",
            .typography = MakeTypography(24.0f * layout.controls_scale,
                                         30.0f * layout.controls_scale),
            .color = {0.92f, 0.94f, 0.98f, 1.0f},
        }),
    });

    auto brush_state = bindings.brush_state ? bindings.brush_state : std::make_shared<BrushState>();
    status_section.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "brush_label",
        .fragment = SP::UI::Declarative::Label::Fragment({
            .text = format_brush_state(brush_state->size, brush_state->color),
            .typography = MakeTypography(20.0f * layout.controls_scale,
                                         26.0f * layout.controls_scale),
            .color = {0.82f, 0.86f, 0.92f, 1.0f},
        }),
    });
    EnsureActivePanel(status_section);
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "status_section",
        .fragment = SP::UI::Declarative::Stack::Fragment(std::move(status_section)),
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
                auto label_path = SP::UI::Runtime::WidgetPath{brush_label};
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              label_path,
                                                              format_brush_state(bindings.brush_state->size,
                                                                                 bindings.brush_state->color)),
                          "Label::SetText");
            }
            auto status_label = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_label.empty()) {
                auto label_path = SP::UI::Runtime::WidgetPath{status_label};
                std::ostringstream message;
                message << "Brush size adjusted to " << std::lround(value) << " px";
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              label_path,
                                                              message.str()),
                          "Label::SetText");
            }
        },
    };
    auto slider_section = make_section_stack(0.0f);
    slider_section.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "brush_slider_widget",
        .fragment = BuildBrushSliderFragment(slider_config),
    });
    EnsureActivePanel(slider_section);
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "brush_slider",
        .fragment = SP::UI::Declarative::Stack::Fragment(std::move(slider_section)),
    });

    if (std::getenv("PAINT_EXAMPLE_DEBUG") != nullptr) {
        std::cerr << "paint_example: palette entries=" << palette_entries.size()
                  << " controls_content_width=" << layout.controls_content_width << '\n';
    }
    PaletteComponentConfig palette_config{
        .layout = layout,
        .theme = theme,
        .entries = palette_entries,
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
                auto brush_path = SP::UI::Runtime::WidgetPath{brush_label};
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              brush_path,
                                                              format_brush_state(bindings.brush_state->size,
                                                                                 bindings.brush_state->color)),
                          "Label::SetText");
            }
            auto status_path = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_path.empty()) {
                auto widget_path = SP::UI::Runtime::WidgetPath{status_path};
                std::ostringstream message;
                message << "Selected " << entry.label << " paint";
                log_error(SP::UI::Declarative::Label::SetText(ctx.space, widget_path, message.str()),
                          "Label::SetText");
            }
        },
    };
    auto palette_section = make_section_stack(layout.palette_row_spacing);
    palette_section.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "palette_grid",
        .fragment = BuildPaletteFragment(palette_config),
    });
    EnsureActivePanel(palette_section);
    controls.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "palette",
        .fragment = SP::UI::Declarative::Stack::Fragment(std::move(palette_section)),
    });

    HistoryActionsConfig actions_config{
        .layout = layout,
        .on_action = [bindings](SP::UI::Declarative::ButtonContext& ctx, HistoryAction action) {
            auto binding_ptr = bindings.history_binding ? *bindings.history_binding : std::shared_ptr<DeclarativeHistoryBinding>{};
            if (!binding_ptr) {
                std::cerr << "paint_example: history binding missing for "
                          << (action == HistoryAction::Undo ? "undo" : "redo")
                          << " button\n";
                auto widget_path = bindings.paint_widget_path ? *bindings.paint_widget_path : std::string{};
                auto metrics_root = SP::UI::Declarative::HistoryMetricsRoot(widget_path);
                SP::UI::Declarative::WriteHistoryBindingState(ctx.space, metrics_root, "missing");
                SP::Error missing_error{SP::Error::Code::UnknownError, "history_binding_missing"};
                SP::UI::Declarative::RecordHistoryBindingError(ctx.space,
                                                               metrics_root,
                                                               action == HistoryAction::Undo ? "UndoableSpace::undo"
                                                                                              : "UndoableSpace::redo",
                                                               &missing_error);
                return;
            }
            auto root = SP::ConcretePathStringView{binding_ptr->root};
            auto action_kind = action == HistoryAction::Undo ? SP::UI::Declarative::HistoryBindingAction::Undo
                                                             : SP::UI::Declarative::HistoryBindingAction::Redo;
            auto update_action_metrics = [&](bool success) {
                SP::UI::Declarative::RecordHistoryBindingActionResult(ctx.space, *binding_ptr, action_kind, success);
            };
            SP::Expected<void> status = action == HistoryAction::Undo ? binding_ptr->undo->undo(root)
                                                                      : binding_ptr->undo->redo(root);
            auto action_label = action == HistoryAction::Undo ? "UndoableSpace::undo" : "UndoableSpace::redo";
            if (!status) {
                update_action_metrics(false);
                log_error(status, action_label);
                SP::UI::Declarative::SetHistoryBindingState(ctx.space, *binding_ptr, "error");

                auto error_info = SP::UI::Declarative::RecordHistoryBindingError(ctx.space,
                                                                                binding_ptr->metrics_root,
                                                                                action_label,
                                                                                &status.error());
                binding_ptr->last_error_context = error_info.context;
                binding_ptr->last_error_message = error_info.message;
                binding_ptr->last_error_code = error_info.code;
                binding_ptr->last_error_timestamp_ns = error_info.timestamp_ns;
                SP::UI::Declarative::PublishHistoryBindingCard(ctx.space, *binding_ptr);
                return;
            }
            update_action_metrics(true);
            SP::UI::Declarative::SetHistoryBindingState(ctx.space, *binding_ptr, "ready");
            auto status_label = bindings.status_label_path ? *bindings.status_label_path : std::string{};
            if (!status_label.empty()) {
                auto status_path = SP::UI::Runtime::WidgetPath{status_label};
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

auto create_paint_window_context(SP::PathSpace& space, CommandLineOptions const& options)
    -> std::optional<PaintWindowContext> {
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "paint_example: failed to launch declarative runtime\n";
        return std::nullopt;
    }

    auto app = SP::App::Create(space,
                               "paint_example",
                               {.title = "Declarative Paint"});
    if (!app) {
        std::cerr << "paint_example: failed to create app\n";
        return std::nullopt;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};
    auto theme_selection = SP::UI::Runtime::Widgets::LoadTheme(space, app_root_view, "");
    if (!theme_selection) {
        log_expected_error("Widgets::LoadTheme", theme_selection.error());
        return std::nullopt;
    }
    auto active_theme = theme_selection->theme;

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "paint_window";
    window_opts.title = "Declarative Paint Surface";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window_result = SP::Window::Create(space, app_root_view, window_opts);
    if (!window_result) {
        std::cerr << "paint_example: failed to create window\n";
        return std::nullopt;
    }
    auto window = *window_result;

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "paint_scene";
    scene_opts.description = "Declarative paint scene";
    auto scene_result = SP::Scene::Create(space, app_root_view, window.path, scene_opts);
    if (!scene_result) {
        std::cerr << "paint_example: failed to create scene\n";
        return std::nullopt;
    }
    auto scene = *scene_result;

    auto present_handles_result = SP::UI::Declarative::BuildPresentHandles(space,
                                                                           app_root_view,
                                                                           window.path,
                                                                           window.view_name);
    if (!present_handles_result) {
        log_expected_error("failed to prepare presenter bootstrap", present_handles_result.error());
        return std::nullopt;
    }
    auto present_handles = *present_handles_result;
    auto capture_status = set_capture_framebuffer_enabled(space,
                                                          window.path,
                                                          window.view_name,
                                                          true);
    if (!capture_status) {
        log_expected_error("enable framebuffer capture", capture_status.error());
        return std::nullopt;
    }
    auto bind_scene = SP::UI::Surface::SetScene(space, present_handles.surface, scene.path);
    if (!bind_scene) {
        log_expected_error("Surface::SetScene", bind_scene.error());
        return std::nullopt;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "paint_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "paint_example");
    auto pointer_devices = std::array<std::string, 1>{std::string{kPointerDevice}};
    auto keyboard_devices = std::array<std::string, 1>{std::string{kKeyboardDevice}};
    subscribe_window_devices(space,
                             window.path,
                             std::span<const std::string>(pointer_devices.data(), pointer_devices.size()),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices.data(), keyboard_devices.size()));

    auto window_view_path = std::string(window.path.getPath()) + "/views/" + window.view_name;
    PaintWindowContext context{
        .app_root = std::move(app_root),
        .window = std::move(window),
        .scene = std::move(scene),
        .present_handles = std::move(present_handles),
        .window_view_path = std::move(window_view_path),
        .theme = std::move(active_theme),
    };
    return context;
}

auto mount_paint_ui(SP::PathSpace& space,
                    PaintWindowContext const& window_context,
                    CommandLineOptions const& options,
                    bool screenshot_mode) -> std::optional<PaintUiContext> {
    auto brush_state = std::make_shared<BrushState>();
    auto layout_metrics = PaintControlsNS::ComputeLayoutMetrics(options.width, options.height);
    auto palette_entries = PaintControlsNS::BuildDefaultPaletteEntries(window_context.theme);

    PaintUiBindings bindings{
        .paint_widget_path = std::make_shared<std::string>(),
        .status_label_path = std::make_shared<std::string>(),
        .brush_label_path = std::make_shared<std::string>(),
        .undo_button_path = std::make_shared<std::string>(),
        .redo_button_path = std::make_shared<std::string>(),
        .history_binding = std::make_shared<std::shared_ptr<DeclarativeHistoryBinding>>(),
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
        auto widget_path = SP::UI::Runtime::WidgetPath{label_path};
        log_error(SP::UI::Declarative::Label::SetText(ctx.space, widget_path, "Stroke recorded"),
                  "Label::SetText");
    };

    auto controls_fragment = build_controls_fragment(bindings,
                                                     layout_metrics,
                                                     window_context.theme,
                                                     std::span<const PaintControlsNS::PaletteEntry>(palette_entries.data(),
                                                                                                    palette_entries.size()));

    SP::UI::Declarative::Stack::Args root_stack{};
    root_stack.active_panel = "canvas_panel";
    root_stack.style.axis = SP::UI::Runtime::Widgets::StackAxis::Horizontal;
    root_stack.style.spacing = layout_metrics.controls_spacing;
    root_stack.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Start;
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

    auto ui_stack = SP::UI::Declarative::Stack::Create(
        space,
        SP::App::ConcretePathView{window_context.window_view_path},
        "ui_stack",
        std::move(root_stack));
    if (!ui_stack) {
        log_expected_error("create UI stack", ui_stack.error());
        return std::nullopt;
    }

    auto stack_root = ui_stack->getPath();
    auto controls_root = stack_root + "/children/controls_panel";
    *bindings.paint_widget_path = stack_root + "/children/canvas_panel";
    auto paint_widget_path = *bindings.paint_widget_path;

    SP::UI::Declarative::InitializeHistoryMetrics(space, paint_widget_path);
    auto make_stack_options = [&](std::chrono::milliseconds timeout) {
        SP::UI::Declarative::StackReadinessOptions options{};
        options.timeout = timeout;
        options.log = [](std::string_view message) {
            std::cerr << "paint_example: " << message << '\n';
        };
        return options;
    };
    auto controls_ready = SP::UI::Declarative::WaitForStackChildren(
        space,
        controls_root,
        std::span<const std::string_view>(kControlsStackChildren),
        make_stack_options(std::chrono::milliseconds(1500)));
    if (!controls_ready) {
        log_expected_error("wait for controls stack children", controls_ready.error());
        return std::nullopt;
    }

    auto status_root = controls_root + "/children/status_section";
    auto status_ready = SP::UI::Declarative::WaitForStackChildren(
        space,
        status_root,
        std::span<const std::string_view>(kStatusStackChildren),
        make_stack_options(std::chrono::milliseconds(1000)));
    if (!status_ready) {
        log_expected_error("wait for status stack children", status_ready.error());
        return std::nullopt;
    }
    *bindings.status_label_path = status_root + "/children/status_label";
    *bindings.brush_label_path = status_root + "/children/brush_label";

    auto actions_root = controls_root + "/children/actions";
    auto actions_ready = SP::UI::Declarative::WaitForStackChildren(
        space,
        actions_root,
        std::span<const std::string_view>(kActionsStackChildren),
        make_stack_options(std::chrono::milliseconds(1000)));
    if (!actions_ready) {
        log_expected_error("wait for actions stack children", actions_ready.error());
        return std::nullopt;
    }
    *bindings.undo_button_path = actions_root + "/children/undo_button";
    *bindings.redo_button_path = actions_root + "/children/redo_button";

    auto palette_root = controls_root + "/children/palette";
    auto palette_section_ready = SP::UI::Declarative::WaitForStackChildren(
        space,
        palette_root,
        std::span<const std::string_view>(kPaletteSectionChildren),
        make_stack_options(std::chrono::milliseconds(1000)));
    if (!palette_section_ready) {
        log_expected_error("wait for palette stack child", palette_section_ready.error());
        return std::nullopt;
    }
    std::vector<std::string> palette_row_ids;
    constexpr std::size_t kButtonsPerRow = 3; // mirror PaintControls::kButtonsPerRow
    if (!palette_entries.empty()) {
        auto rows = (palette_entries.size() + kButtonsPerRow - 1) / kButtonsPerRow;
        palette_row_ids.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            palette_row_ids.emplace_back(std::string{"palette_row_"} + std::to_string(row));
        }
    }
    if (!palette_row_ids.empty()) {
        std::vector<std::string_view> palette_row_views;
        palette_row_views.reserve(palette_row_ids.size());
        for (auto const& id : palette_row_ids) {
            palette_row_views.push_back(id);
        }
        auto palette_grid_root = palette_root + "/children/palette_grid";
        auto palette_rows_ready = SP::UI::Declarative::WaitForStackChildren(
            space,
            palette_grid_root,
            std::span<const std::string_view>(palette_row_views.data(), palette_row_views.size()),
            make_stack_options(std::chrono::milliseconds(1000)));
        if (!palette_rows_ready) {
            log_expected_error("wait for palette rows", palette_rows_ready.error());
            return std::nullopt;
        }
        if (std::getenv("PAINT_EXAMPLE_DEBUG") != nullptr) {
            auto palette_children =
                space.listChildren(SP::ConcretePathStringView{palette_grid_root + "/children"});
            std::cerr << "paint_example: palette grid child count=" << palette_children.size() << '\n';
        }
    }

    SP::UI::Declarative::HistoryBindingOptions history_options{
        .history_root = paint_widget_path,
    };
    auto history_binding_result = SP::UI::Declarative::CreateHistoryBinding(space, history_options);
    if (!history_binding_result) {
        log_expected_error("failed to enable UndoableSpace history", history_binding_result.error());
        return std::nullopt;
    }
    auto history_binding = *history_binding_result;
    *bindings.history_binding = history_binding;
    set_history_buttons_enabled(space, bindings, true);

    PaintUiContext context{
        .bindings = std::move(bindings),
        .layout_metrics = layout_metrics,
        .stack_root = std::move(stack_root),
        .controls_root = std::move(controls_root),
        .paint_widget_path = std::move(paint_widget_path),
        .paint_gpu_enabled = paint_gpu_enabled,
    };
    return context;
}

auto log_expected_error(std::string const& context, SP::Error const& error) -> void {
    std::cerr << "paint_example: " << context << " error (code=" << static_cast<int>(error.code) << ")";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

} // namespace

namespace PathSpaceExamples {

auto RunPaintExample(CommandLineOptions options) -> int {
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
    options.baseline_metadata.tolerance = options.screenshot_max_mean_error;
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
        options.baseline_metadata.manifest_revision = parsed_revision;
        std::cout << "paint_example: baseline manifest revision " << *parsed_revision
                  << " (required " << kRequiredBaselineManifestRevision << ")\n";
    }
    if (baseline_tag_env) {
        options.baseline_metadata.tag = *baseline_tag_env;
    }
    if (baseline_sha_env) {
        options.baseline_metadata.sha256 = *baseline_sha_env;
    }
    if (auto width_env = read_env_string("PAINT_EXAMPLE_BASELINE_WIDTH")) {
        if (auto value = parse_env_int(*width_env)) {
            options.baseline_metadata.width = *value;
        }
    }
    if (auto height_env = read_env_string("PAINT_EXAMPLE_BASELINE_HEIGHT")) {
        if (auto value = parse_env_int(*height_env)) {
            options.baseline_metadata.height = *value;
        }
    }
    if (auto renderer_env = read_env_string("PAINT_EXAMPLE_BASELINE_RENDERER")) {
        options.baseline_metadata.renderer = *renderer_env;
    }
    if (auto captured_env = read_env_string("PAINT_EXAMPLE_BASELINE_CAPTURED_AT")) {
        options.baseline_metadata.captured_at = *captured_env;
    }
    if (auto commit_env = read_env_string("PAINT_EXAMPLE_BASELINE_COMMIT")) {
        options.baseline_metadata.commit = *commit_env;
    }
    if (auto notes_env = read_env_string("PAINT_EXAMPLE_BASELINE_NOTES")) {
        options.baseline_metadata.notes = *notes_env;
    }
    if (auto tolerance_env = read_env_string("PAINT_EXAMPLE_BASELINE_TOLERANCE")) {
        if (auto parsed_tolerance = parse_env_double(*tolerance_env)) {
            options.baseline_metadata.tolerance = *parsed_tolerance;
        }
    }

    SP::PathSpace space;

    auto window_context = create_paint_window_context(space, options);
    if (!window_context) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    bool screenshot_mode = options.screenshot_path.has_value();

    auto ui_context = mount_paint_ui(space, *window_context, options, screenshot_mode);
    if (!ui_context) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto& bindings = ui_context->bindings;
    auto brush_state = bindings.brush_state;
    auto const& layout_metrics = ui_context->layout_metrics;
    auto paint_widget_path = ui_context->paint_widget_path;
    bool const paint_gpu_enabled = ui_context->paint_gpu_enabled;
    auto paint_widget = SP::UI::Runtime::WidgetPath{paint_widget_path};
    auto initial_buffer_revision = [&]() -> std::uint64_t {
        auto revision = space.read<std::uint64_t, std::string>(paint_widget_path + "/render/buffer/revision");
        return revision.value_or(0);
    }();

    auto const& window_view_path = window_context->window_view_path;
    auto& window_result = window_context->window;
    auto& scene_result = window_context->scene;

    auto readiness = ensure_declarative_scene_ready(space,
                                                    scene_result.path,
                                                    window_result.path,
                                                    window_result.view_name);
    if (!readiness) {
        std::cerr << "paint_example: failed to wait for declarative widgets: "
                  << SP::describeError(readiness.error()) << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto latest_revision = readiness->scene_revision;
    if (!latest_revision) {
        std::cerr << "paint_example: scene readiness did not produce a revision\n";
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
        if (debug_logging) {
            std::cerr << "paint_example: layout canvas_offset=(" << layout_metrics.canvas_offset_x << ", "
                      << layout_metrics.canvas_offset_y << ") canvas_size=(" << layout_metrics.canvas_width
                      << "x" << layout_metrics.canvas_height << ")\n";
        }
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
        }
        auto require_live_capture = options.screenshot_require_present
                                    && !options.screenshot_force_software;
        auto await_capture_revision = [&]() -> bool {
            constexpr int kMaxAttempts = 3;
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                auto prior_revision = latest_revision;
                SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
                publish_options.min_revision = prior_revision;
                publish_options.wait_timeout = std::chrono::milliseconds(2000);
                auto force_publish = SP::UI::Declarative::SceneLifecycle::ForcePublish(space,
                                                                                      scene_result.path,
                                                                                      publish_options);
                if (!force_publish) {
                    auto const& publish_error = force_publish.error();
                    log_expected_error("SceneLifecycle::ForcePublish", publish_error);
                    bool attempted_fallback = false;
                    if (publish_error.code == SP::Error::Code::InvalidType
                        && publish_error.message
                        && publish_error.message->find("point buffer out of range") != std::string::npos) {
                        attempted_fallback = true;
                        auto capture_revision = wait_for_declarative_scene_revision(space,
                                                                                    scene_result.path,
                                                                                    std::chrono::seconds(5),
                                                                                    prior_revision);
                        if (capture_revision) {
                            latest_revision = *capture_revision;
                            return true;
                        }
                    }
                    if (!attempted_fallback) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                } else {
                    latest_revision = *force_publish;
                    auto capture_revision = wait_for_declarative_scene_revision(space,
                                                                                scene_result.path,
                                                                                std::chrono::seconds(5),
                                                                                prior_revision);
                    if (capture_revision) {
                        latest_revision = *capture_revision;
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                std::cerr << "paint_example: scene revision attempt " << (attempt + 1)
                          << " did not publish after playback\n";
            }
            return false;
        };
        if (!wait_for_paint_capture_ready(space,
                                          paint_widget_path,
                                          std::chrono::milliseconds(2000))) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        if (!wait_for_paint_buffer_revision(space,
                                            paint_widget_path,
                                            initial_buffer_revision,
                                            std::chrono::milliseconds(500))) {
            std::cerr << "paint_example: paint buffer revision did not advance after playback\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        if (!await_capture_revision()) {
            std::cerr << "paint_example: scene revision never advanced after playback"
                      << " (last revision " << latest_revision.value_or(0) << ")\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        SP::UI::Screenshot::ScreenshotRequest screenshot_request{
            .space = space,
            .window_path = window_result.path,
            .view_name = window_result.view_name,
            .width = options.width,
            .height = options.height,
            .output_png = *options.screenshot_path,
            .baseline_png = options.screenshot_compare_path,
            .diff_png = options.screenshot_diff_path,
            .metrics_json = options.screenshot_metrics_path,
            .max_mean_error = options.screenshot_max_mean_error,
            .require_present = require_live_capture,
            .present_timeout = std::chrono::milliseconds(1500),
            .hooks = {},
            .baseline_metadata = options.baseline_metadata,
            .telemetry_root = options.screenshot_telemetry_root,
            .telemetry_namespace = options.screenshot_telemetry_namespace,
            .force_software = options.screenshot_force_software,
        };
        screenshot_request.hooks.ensure_ready = [&]() -> SP::Expected<void> {
            if (!paint_gpu_enabled) {
                return {};
            }
            if (!wait_for_paint_capture_ready(space,
                                              paint_widget_path,
                                              std::chrono::milliseconds(2000))) {
                return std::unexpected(make_runtime_error("paint GPU never became Ready before capture"));
            }
            return {};
        };
        screenshot_request.hooks.postprocess_png =
            [&](std::filesystem::path const& output_png) -> SP::Expected<void> {
            if (auto status = overlay_strokes_onto_png(output_png, strokes_preview, layout_metrics); !status) {
                return status;
            }
            if (auto status = apply_controls_background_overlay(output_png,
                                                                layout_metrics,
                                                                options.width,
                                                                options.height,
                                                                options.screenshot_compare_path);
                !status) {
                return status;
            }
            return apply_controls_shadow_overlay(output_png, layout_metrics, options.width, options.height);
        };
        screenshot_request.hooks.fallback_writer = [&]() -> SP::Expected<void> {
            if (!write_image_png(strokes_preview, *options.screenshot_path)) {
                return std::unexpected(make_runtime_error("software fallback write failed"));
            }
            return {};
        };
        auto capture_result = SP::UI::Screenshot::ScreenshotService::Capture(screenshot_request);
        if (!capture_result) {
            std::cerr << "paint_example: screenshot capture failed: "
                      << describeError(capture_result.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        log_lifecycle_state("after_capture_attempt");
        std::cout << "paint_example: capture mode = "
                  << (capture_result->hardware_capture ? "Window::Present hardware"
                                                       : "software fallback")
                  << "\n";
        if (!capture_result->hardware_capture && !options.screenshot_force_software) {
            std::cout << "paint_example: hardware capture unavailable; consider setting "
                      << "PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1 for CI fallback" << "\n";
        }
        if (capture_result->matched_baseline) {
            std::cout << "paint_example: screenshot baseline matched (mean error "
                      << capture_result->mean_error.value_or(0.0)
                      << ", max channel delta "
                      << capture_result->max_channel_delta.value_or(0) << ")\n";
        } else {
            std::cout << "paint_example: saved screenshot to " << options.screenshot_path->string() << "\n";
        }
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
                     window_result.path,
                     window_result.view_name,
                     window_context->present_handles,
                     options.width,
                     options.height,
                     hooks);

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}

auto ParsePaintExampleCommandLine(int argc, char** argv) -> CommandLineOptions {
    return parse_options(argc, argv);
}

} // namespace PathSpaceExamples

#ifndef PATHSPACE_PAINT_EXAMPLE_NO_MAIN
int main(int argc, char** argv) {
    auto options = PathSpaceExamples::ParsePaintExampleCommandLine(argc, argv);
    return PathSpaceExamples::RunPaintExample(std::move(options));
}
#endif
