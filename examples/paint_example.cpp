#include "declarative_example_shared.hpp"

#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <charconv>
#include <iostream>
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

using namespace PathSpaceExamples;

namespace {

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool headless = false;
    std::optional<std::filesystem::path> screenshot_path;
    bool gpu_smoke = false;
    std::optional<std::filesystem::path> gpu_texture_path;
};

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
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

struct PaletteColor {
    const char* id;
    const char* label;
    std::array<float, 4> color;
};

auto palette_colors() -> std::vector<PaletteColor> {
    return {
        {"paint_palette_red", "Red", {0.905f, 0.173f, 0.247f, 1.0f}},
        {"paint_palette_orange", "Orange", {0.972f, 0.545f, 0.192f, 1.0f}},
        {"paint_palette_yellow", "Yellow", {0.995f, 0.847f, 0.207f, 1.0f}},
        {"paint_palette_green", "Green", {0.172f, 0.701f, 0.368f, 1.0f}},
        {"paint_palette_blue", "Blue", {0.157f, 0.407f, 0.933f, 1.0f}},
        {"paint_palette_purple", "Purple", {0.560f, 0.247f, 0.835f, 1.0f}},
    };
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

template <typename T>
auto replace_value(SP::PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
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

auto render_scripted_strokes_png(int width,
                                 int height,
                                 std::filesystem::path const& output_path,
                                 float brush_radius,
                                 std::array<float, 4> const& brush_color) -> bool {
    auto background = make_image(width, height, {0.07f, 0.08f, 0.12f, 1.0f});
    std::unordered_map<std::string, std::pair<float, float>> active_strokes;
    auto actions = scripted_stroke_actions("screenshot");
    for (auto const& action : actions) {
        switch (action.kind) {
        case WidgetOpKind::PaintStrokeBegin:
            active_strokes[action.target_id] = {action.pointer.local_x, action.pointer.local_y};
            draw_disc(background, action.pointer.local_x, action.pointer.local_y, brush_radius, brush_color);
            break;
        case WidgetOpKind::PaintStrokeUpdate: {
            auto it = active_strokes.find(action.target_id);
            if (it != active_strokes.end()) {
                draw_line(background,
                          it->second.first,
                          it->second.second,
                          action.pointer.local_x,
                          action.pointer.local_y,
                          brush_radius,
                          brush_color);
                it->second = {action.pointer.local_x, action.pointer.local_y};
            }
            break;
        }
        case WidgetOpKind::PaintStrokeCommit: {
            auto it = active_strokes.find(action.target_id);
            if (it != active_strokes.end()) {
                draw_line(background,
                          it->second.first,
                          it->second.second,
                          action.pointer.local_x,
                          action.pointer.local_y,
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
    return write_image_png(background, output_path);
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
};

auto make_history_binding(SP::PathSpace& space, std::string root_path) -> SP::Expected<HistoryBinding> {
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
        return std::unexpected(enable.error());
    }
    return HistoryBinding{
        .undo = std::move(undo_space),
        .root = std::move(root_path),
    };
}

struct BrushState {
    float size = 12.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

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

    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "paint_example: failed to launch declarative runtime\n";
        return 1;
    }

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

    auto status_label = SP::UI::Declarative::Label::Create(space,
                                                           window_view,
                                                           "status_label",
                                                           {.text = "Pick a color and drag on the canvas"});
    if (!status_label) {
        std::cerr << "paint_example: failed to create status label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto brush_state = std::make_shared<BrushState>();
    auto brush_label = SP::UI::Declarative::Label::Create(space,
                                                          window_view,
                                                          "brush_label",
                                                          {.text = format_brush_state(brush_state->size,
                                                                                      brush_state->color)});
    if (!brush_label) {
        std::cerr << "paint_example: failed to create brush label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    bool screenshot_mode = options.screenshot_path.has_value();

    SP::UI::Declarative::PaintSurface::Args paint_args{};
    paint_args.brush_size = brush_state->size;
    paint_args.brush_color = brush_state->color;
    paint_args.buffer_width = static_cast<std::uint32_t>(options.width);
    paint_args.buffer_height = static_cast<std::uint32_t>(options.height);
    paint_args.gpu_enabled = options.gpu_smoke || screenshot_mode;
    paint_args.on_draw = [status_label_path = *status_label](SP::UI::Declarative::PaintSurfaceContext& ctx) {
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Stroke recorded"),
                  "Label::SetText");
    };
    auto paint_surface = SP::UI::Declarative::PaintSurface::Create(space,
                                                                   window_view,
                                                                   "paint_surface",
                                                                   paint_args);
    if (!paint_surface) {
        std::cerr << "paint_example: failed to create paint surface\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    std::string paint_widget_path = paint_surface->getPath();

    auto history_binding_result = make_history_binding(space, paint_widget_path);
    if (!history_binding_result) {
        log_expected_error("failed to enable UndoableSpace history", history_binding_result.error());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto history_binding = std::make_shared<HistoryBinding>(std::move(*history_binding_result));

    SP::UI::Declarative::Slider::Args slider_args{};
    slider_args.minimum = 1.0f;
    slider_args.maximum = 64.0f;
    slider_args.step = 1.0f;
    slider_args.value = brush_state->size;
    slider_args.on_change = [brush_state,
                             paint_widget_path,
                             brush_label_path = *brush_label,
                             status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx) {
        brush_state->size = ctx.value;
        auto status = apply_brush_size(ctx.space, paint_widget_path, brush_state->size);
        if (!status) {
            log_error(status, "apply_brush_size");
            return;
        }
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      brush_label_path,
                                                      format_brush_state(brush_state->size, brush_state->color)),
                  "Label::SetText");
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Updated brush size"),
                  "Label::SetText");
    };
    auto brush_slider = SP::UI::Declarative::Slider::Create(space,
                                                            window_view,
                                                            "brush_slider",
                                                            slider_args);
    if (!brush_slider) {
        std::cerr << "paint_example: failed to create brush slider\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    for (auto const& entry : palette_colors()) {
        SP::UI::Declarative::Button::Args palette_args{};
        palette_args.label = entry.label;
        palette_args.on_press = [brush_state,
                                 paint_widget_path,
                                 brush_label_path = *brush_label,
                                 status_label_path = *status_label,
                                 entry](SP::UI::Declarative::ButtonContext& ctx) {
            brush_state->color = entry.color;
            auto status = apply_brush_color(ctx.space, paint_widget_path, brush_state->color);
            if (!status) {
                log_error(status, "apply_brush_color");
                return;
            }
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          brush_label_path,
                                                          format_brush_state(brush_state->size, brush_state->color)),
                      "Label::SetText");
            std::ostringstream message;
            message << "Selected " << entry.label << " paint";
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          status_label_path,
                                                          message.str()),
                      "Label::SetText");
        };
        auto button = SP::UI::Declarative::Button::Create(space,
                                                          window_view,
                                                          entry.id,
                                                          palette_args);
        if (!button) {
            std::cerr << "paint_example: failed to create palette button '" << entry.label << "'\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
    }

    auto undo_button = SP::UI::Declarative::Button::Create(
        space,
        window_view,
        "undo_button",
        SP::UI::Declarative::Button::Args{
            .label = "Undo Stroke",
            .on_press = [history_binding,
                         status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
                auto undo = history_binding->undo->undo(SP::ConcretePathStringView{history_binding->root});
                if (!undo) {
                    log_error(undo, "UndoableSpace::undo");
                    return;
                }
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              status_label_path,
                                                              "Undo applied"),
                          "Label::SetText");
            },
        });
    if (!undo_button) {
        std::cerr << "paint_example: failed to create undo button\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto redo_button = SP::UI::Declarative::Button::Create(
        space,
        window_view,
        "redo_button",
        SP::UI::Declarative::Button::Args{
            .label = "Redo Stroke",
            .on_press = [history_binding,
                         status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
                auto redo = history_binding->undo->redo(SP::ConcretePathStringView{history_binding->root});
                if (!redo) {
                    log_error(redo, "UndoableSpace::redo");
                    return;
                }
                log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                              status_label_path,
                                                              "Redo applied"),
                          "Label::SetText");
            },
        });
    if (!redo_button) {
        std::cerr << "paint_example: failed to create redo button\n";
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
        if (!playback_scripted_strokes(space, paint_widget_path)) {
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        if (auto capture_revision = wait_for_scene_revision(space,
                                                            scene_result.path,
                                                            std::chrono::seconds(10),
                                                            latest_revision)) {
            latest_revision = capture_revision;
        } else {
            std::cerr << "paint_example: timed out waiting for scene revision > "
                      << latest_revision.value_or(0) << ", capturing latest published frame\n";
        }
        if (!capture_window_screenshot(space,
                                       window->path,
                                       window->view_name,
                                       options.width,
                                       options.height,
                                       *options.screenshot_path,
                                       std::chrono::milliseconds(1500))) {
            std::cerr << "paint_example: Window::Present capture failed, falling back to software renderer\n";
            auto brush_radius = brush_state->size * 0.5f;
            if (!render_scripted_strokes_png(options.width,
                                             options.height,
                                             *options.screenshot_path,
                                             brush_radius,
                                             brush_state->color)) {
                SP::System::ShutdownDeclarativeRuntime(space);
                return 1;
            }
        }
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
