#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

#if !defined(PATHSPACE_ENABLE_UI)
int main() {
    std::cerr << "widgets_example requires PATHSPACE_ENABLE_UI=ON.\n";
    return 1;
}
#elif !defined(__APPLE__)
int main(int argc, char** argv) {
    std::cerr << "widgets_example currently supports only macOS builds.\n";
    return 1;
}
#else

#include <pathspace/ui/LocalWindowBridge.hpp>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

namespace {

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
namespace SceneData = SP::UI::Scene;
namespace SceneBuilders = SP::UI::Builders::Scene;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;
namespace WidgetFocus = SP::UI::Builders::Widgets::Focus;
namespace Diagnostics = SP::UI::Builders::Diagnostics;
namespace TextBuilder = SP::UI::Builders::Text;
using TextBuildResult = TextBuilder::BuildResult;

constexpr float kDefaultMargin = 32.0f;
constexpr unsigned int kKeycodeTab = 0x30;        // macOS virtual key codes
constexpr unsigned int kKeycodeSpace = 0x31;
constexpr unsigned int kKeycodeReturn = 0x24;
constexpr unsigned int kKeycodeLeft = 0x7B;
constexpr unsigned int kKeycodeRight = 0x7C;
constexpr unsigned int kKeycodeDown = 0x7D;
constexpr unsigned int kKeycodeUp = 0x7E;

static bool g_debug_capture_enabled = [] {
    const char* env = std::getenv("WIDGETS_EXAMPLE_DEBUG_CAPTURE");
    return env && env[0] != '\0' && env[0] != '0';
}();

static auto debug_capture_enabled() -> bool {
    return g_debug_capture_enabled;
}

static void set_debug_capture_enabled(bool enabled) {
    g_debug_capture_enabled = enabled;
}

struct CommandLineOptions {
    std::optional<std::string> screenshot_path;
    std::optional<std::string> theme_name;
    bool debug_capture = debug_capture_enabled();
    bool show_help = false;
};

static auto parse_debug_value(std::string value) -> std::optional<bool> {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return std::nullopt;
}

static void print_usage() {
    std::cout << "widgets_example options:\n"
              << "  --screenshot <path>   Save a PNG screenshot of the window then exit\n"
              << "  --theme <name>        Apply a named widget theme (sunset [default, blue], skylight [orange])\n"
              << "  --debug[=on|off]      Toggle debug capture logs and dumps (alias: --no-debug)\n";
}

static auto parse_command_line(int argc, char** argv) -> std::optional<CommandLineOptions> {
    CommandLineOptions options{};
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--screenshot") {
            if (i + 1 < argc) {
                std::string value(argv[++i]);
                if (value.empty()) {
                    std::cerr << "widgets_example: --screenshot requires a file path\n";
                    return std::nullopt;
                }
                options.screenshot_path = std::move(value);
            } else {
                std::cerr << "widgets_example: --screenshot requires a file path\n";
                return std::nullopt;
            }
        } else if (arg.rfind("--screenshot=", 0) == 0) {
            std::string value = arg.substr(std::string("--screenshot=").size());
            if (value.empty()) {
                std::cerr << "widgets_example: --screenshot requires a file path\n";
                return std::nullopt;
            }
            options.screenshot_path = std::move(value);
        } else if (arg == "--theme") {
            if (i + 1 < argc) {
                std::string value(argv[++i]);
                if (value.empty()) {
                    std::cerr << "widgets_example: --theme requires a theme name\n";
                    return std::nullopt;
                }
                options.theme_name = std::move(value);
            } else {
                std::cerr << "widgets_example: --theme requires a theme name\n";
                return std::nullopt;
            }
        } else if (arg.rfind("--theme=", 0) == 0) {
            std::string value = arg.substr(std::string("--theme=").size());
            if (value.empty()) {
                std::cerr << "widgets_example: --theme requires a theme name\n";
                return std::nullopt;
            }
            options.theme_name = std::move(value);
        } else if (arg == "--debug") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string value(argv[++i]);
                if (auto parsed = parse_debug_value(std::move(value))) {
                    options.debug_capture = *parsed;
                } else {
                    std::cerr << "widgets_example: invalid --debug value (expected on/off, true/false, or 1/0)\n";
                    return std::nullopt;
                }
            } else {
                options.debug_capture = true;
            }
        } else if (arg == "--no-debug") {
            options.debug_capture = false;
        } else if (arg.rfind("--debug=", 0) == 0) {
            std::string value = arg.substr(std::string("--debug=").size());
            if (value.empty()) {
                std::cerr << "widgets_example: --debug requires a value (on/off)\n";
                return std::nullopt;
            }
            if (auto parsed = parse_debug_value(std::move(value))) {
                options.debug_capture = *parsed;
            } else {
                std::cerr << "widgets_example: invalid --debug value (expected on/off, true/false, or 1/0)\n";
                return std::nullopt;
            }
        } else if (arg == "--help") {
            options.show_help = true;
            return options;
        }
    }

    if (options.screenshot_path && options.screenshot_path->empty()) {
        std::cerr << "widgets_example: screenshot path cannot be empty\n";
        return std::nullopt;
    }

    return options;
}

struct WidgetBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    auto normalize() -> void {
        if (max_x < min_x) {
            std::swap(max_x, min_x);
        }
        if (max_y < min_y) {
            std::swap(max_y, min_y);
        }
    }

    auto include(WidgetBounds const& other) -> void {
        WidgetBounds normalized_other = other;
        normalized_other.normalize();
        normalize();
        min_x = std::min(min_x, normalized_other.min_x);
        min_y = std::min(min_y, normalized_other.min_y);
        max_x = std::max(max_x, normalized_other.max_x);
        max_y = std::max(max_y, normalized_other.max_y);
    }

    [[nodiscard]] auto is_valid() const -> bool {
        return std::isfinite(min_x) && std::isfinite(min_y)
               && std::isfinite(max_x) && std::isfinite(max_y)
               && max_x >= min_x && max_y >= min_y;
    }

    auto contains(float x, float y) const -> bool {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }

    auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

static auto widget_bounds_from_preview_rect(Widgets::TreePreviewRect const& rect) -> WidgetBounds {
    WidgetBounds bounds{
        rect.min_x,
        rect.min_y,
        rect.max_x,
        rect.max_y,
    };
    bounds.normalize();
    return bounds;
}

static auto widget_bounds_from_preview_rect(Widgets::TreePreviewRect const& rect,
                                            float dx,
                                            float dy) -> WidgetBounds {
    WidgetBounds bounds = widget_bounds_from_preview_rect(rect);
    bounds.min_x += dx;
    bounds.max_x += dx;
    bounds.min_y += dy;
    bounds.max_y += dy;
    return bounds;
}

struct ListLayout {
    WidgetBounds bounds;
    std::vector<WidgetBounds> item_bounds;
    float content_top = 0.0f;
    float item_height = 0.0f;
};

struct StackPreviewLayout {
    WidgetBounds bounds;
    std::vector<WidgetBounds> child_bounds;
};

struct TreeRowLayout {
    WidgetBounds bounds;
    std::string node_id;
    std::string label;
    WidgetBounds toggle;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
    bool enabled = true;
};

struct TreeLayout {
    WidgetBounds bounds;
    float content_top = 0.0f;
    float row_height = 0.0f;
    std::vector<TreeRowLayout> rows;
};

struct GalleryLayout {
    WidgetBounds button;
    WidgetBounds button_footprint;
    WidgetBounds toggle;
    WidgetBounds toggle_footprint;
    WidgetBounds slider;
    WidgetBounds slider_track;
    std::optional<WidgetBounds> slider_caption;
    WidgetBounds slider_footprint;
    ListLayout list;
    std::optional<WidgetBounds> list_caption;
    WidgetBounds list_footprint;
    StackPreviewLayout stack;
    std::optional<WidgetBounds> stack_caption;
    WidgetBounds stack_footprint;
    TreeLayout tree;
    std::optional<WidgetBounds> tree_caption;
    WidgetBounds tree_footprint;
};

static void write_frame_capture_png_or_exit(SP::UI::Builders::SoftwareFramebuffer const& framebuffer,
                                            std::filesystem::path const& output_path) {
    if (framebuffer.width <= 0 || framebuffer.height <= 0) {
        std::cerr << "widgets_example: framebuffer capture has invalid dimensions "
                  << framebuffer.width << "x" << framebuffer.height << '\n';
        std::exit(1);
    }

    auto const format = framebuffer.pixel_format;
    bool const is_rgba = format == SP::UI::Builders::PixelFormat::RGBA8Unorm
                      || format == SP::UI::Builders::PixelFormat::RGBA8Unorm_sRGB;
    bool const is_bgra = format == SP::UI::Builders::PixelFormat::BGRA8Unorm
                      || format == SP::UI::Builders::PixelFormat::BGRA8Unorm_sRGB;
    if (!(is_rgba || is_bgra)) {
        std::cerr << "widgets_example: framebuffer capture pixel format not supported for PNG export ("
                  << static_cast<int>(format) << ")\n";
        std::exit(1);
    }

    auto const parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec{};
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "widgets_example: failed to create directory '" << parent.string()
                      << "' for screenshot: " << ec.message() << '\n';
            std::exit(1);
        }
    }

    auto const stride = static_cast<int>(framebuffer.row_stride_bytes);
    if (stride <= 0 || framebuffer.pixels.size() < static_cast<std::size_t>(stride) * framebuffer.height) {
        std::cerr << "widgets_example: framebuffer capture has invalid stride/data\n";
        std::exit(1);
    }

    int write_result = 0;
    if (is_rgba) {
        write_result = stbi_write_png(output_path.string().c_str(),
                                      framebuffer.width,
                                      framebuffer.height,
                                      4,
                                      framebuffer.pixels.data(),
                                      stride);
    } else {
        std::vector<std::uint8_t> converted(
            static_cast<std::size_t>(framebuffer.width) * static_cast<std::size_t>(framebuffer.height) * 4);
        for (int y = 0; y < framebuffer.height; ++y) {
            auto const* src_row = framebuffer.pixels.data() + static_cast<std::size_t>(y) * stride;
            auto* dst_row = converted.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(framebuffer.width) * 4;
            for (int x = 0; x < framebuffer.width; ++x) {
                auto const* src = src_row + static_cast<std::size_t>(x) * 4;
                auto* dst = dst_row + static_cast<std::size_t>(x) * 4;
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        write_result = stbi_write_png(output_path.string().c_str(),
                                      framebuffer.width,
                                      framebuffer.height,
                                      4,
                                      converted.data(),
                                      framebuffer.width * 4);
    }

    if (write_result == 0) {
        std::cerr << "widgets_example: failed to write screenshot '" << output_path.string() << "'\n";
        std::exit(1);
    }
}

static auto mix_color(std::array<float, 4> base,
                      std::array<float, 4> target,
                      float amount) -> std::array<float, 4> {
    amount = std::clamp(amount, 0.0f, 1.0f);
    std::array<float, 4> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = std::clamp(base[i] * (1.0f - amount) + target[i] * amount, 0.0f, 1.0f);
    }
    out[3] = std::clamp(base[3], 0.0f, 1.0f);
    return out;
}

static auto lighten(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

static auto darken(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {0.0f, 0.0f, 0.0f, color[3]}, amount);
}

static auto desaturate(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    auto gray = std::array<float, 4>{0.5f, 0.5f, 0.5f, color[3]};
    return mix_color(color, gray, amount);
}

auto identity_transform() -> SceneData::Transform {
    SceneData::Transform t{};
    for (int i = 0; i < 16; ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

auto build_text_bucket(std::string_view text,
                       float origin_x,
                       float origin_y,
                       Widgets::TypographyStyle const& typography,
                       std::array<float, 4> color,
                       std::uint64_t drawable_id,
                       std::string authoring_id,
                       float z_value) -> std::optional<TextBuildResult>;

auto append_bucket(SceneData::DrawableBucketSnapshot& dest,
                   SceneData::DrawableBucketSnapshot const& src) -> void;

inline constexpr float kGalleryFocusExpand = 6.0f;
inline constexpr float kGalleryFocusThickness = 4.0f;
inline constexpr float kGalleryFocusMargin = kGalleryFocusExpand + kGalleryFocusThickness;

static auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

static auto lighten_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    amount = clamp_unit(amount);
    for (int i = 0; i < 3; ++i) {
        color[i] = clamp_unit(color[i] * (1.0f - amount) + amount);
    }
    return color;
}

static auto expand_for_focus_margin(WidgetBounds& bounds) -> void {
    bounds.normalize();
    bounds.min_x -= kGalleryFocusMargin;
    bounds.min_y -= kGalleryFocusMargin;
    bounds.max_x += kGalleryFocusMargin;
    bounds.max_y += kGalleryFocusMargin;
    bounds.normalize();
    if (bounds.min_x < 0.0f) {
        bounds.min_x = 0.0f;
    }
    if (bounds.min_y < 0.0f) {
        bounds.min_y = 0.0f;
    }
}

auto append_focus_highlight_preview(SceneData::DrawableBucketSnapshot& bucket,
                                    float width,
                                    float height,
                                    std::string_view authoring_id,
                                    std::array<float, 4> color,
                                    bool pulsing = true) -> void {
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = width;
    float max_y = height;
    if (kGalleryFocusExpand > 0.0f) {
        min_x = std::min(min_x - kGalleryFocusExpand, 0.0f);
        min_y = std::min(min_y - kGalleryFocusExpand, 0.0f);
        max_x = std::max(max_x + kGalleryFocusExpand, width);
        max_y = std::max(max_y + kGalleryFocusExpand, height);
    }

    float thickness_limit = std::min(width, height) * 0.5f;
    float clamped_thickness = std::clamp(kGalleryFocusThickness, 1.0f, thickness_limit);

    std::uint64_t drawable_id = 0xF0C0F100ull + static_cast<std::uint64_t>(bucket.drawable_ids.size());
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(identity_transform());

    SceneData::BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    SceneData::BoundingSphere sphere{};
    sphere.center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f, 0.0f};
    float radius_x = max_x - sphere.center[0];
    float radius_y = max_y - sphere.center[1];
    sphere.radius = std::sqrt(radius_x * radius_x + radius_y * radius_y);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(8);
    bucket.z_values.push_back(5.0f);
    bucket.material_ids.push_back(0);
    bucket.pipeline_flags.push_back(pulsing ? SP::UI::PipelineFlags::HighlightPulse : 0u);
    bucket.visibility.push_back(1);
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_counts.push_back(4);
    bucket.opaque_indices.push_back(static_cast<std::uint32_t>(bucket.opaque_indices.size()));
    bucket.clip_head_indices.push_back(-1);

    auto push_rect = [&](float r_min_x,
                         float r_min_y,
                         float r_max_x,
                         float r_max_y) {
        SceneData::RectCommand rect{};
        rect.min_x = r_min_x;
        rect.min_y = r_min_y;
        rect.max_x = r_max_x;
        rect.max_y = r_max_y;
        rect.color = color;
        auto payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &rect,
                    sizeof(SceneData::RectCommand));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));
    };

    push_rect(min_x, min_y, max_x, min_y + clamped_thickness);
    push_rect(min_x, max_y - clamped_thickness, max_x, max_y);
    push_rect(min_x, min_y + clamped_thickness, min_x + clamped_thickness, max_y - clamped_thickness);
    push_rect(max_x - clamped_thickness, min_y + clamped_thickness, max_x, max_y - clamped_thickness);

    std::string authoring = authoring_id.empty()
                                ? std::string("widget/gallery/focus/highlight")
                                : std::string(authoring_id) + "/focus";
    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        drawable_id,
        std::move(authoring),
        0,
        0});
    bucket.drawable_fingerprints.push_back(drawable_id);
}

static auto make_dirty_hint(WidgetBounds const& bounds) -> Builders::DirtyRectHint {
    WidgetBounds normalized = bounds;
    normalized.normalize();
    Builders::DirtyRectHint hint{};
    hint.min_x = normalized.min_x;
    hint.min_y = normalized.min_y;
    hint.max_x = normalized.max_x;
    hint.max_y = normalized.max_y;
    return hint;
}

auto build_button_preview(Widgets::ButtonStyle const& style,
                          Widgets::ButtonState const& state) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    float width = std::max(style.width, 1.0f);
    float height = std::max(style.height, 1.0f);

    bucket.drawable_ids = {0xB17B0001ull};
    bucket.world_transforms = {identity_transform()};

    SceneData::BoundingSphere sphere{};
    sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0] + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres = {sphere};

    SceneData::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {width, height, 0.0f};
    bucket.bounds_boxes = {box};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.front(), "widget/button/background", 0, 0}};
    bucket.drawable_fingerprints = {0xB17B0001ull};

    auto color = style.background_color;
    if (!state.enabled) {
        color = desaturate(color, 0.6f);
    } else if (state.pressed) {
        color = darken(color, 0.25f);
    } else if (state.hovered) {
        color = lighten(color, 0.15f);
    }

    float radius_limit = std::min(width, height) * 0.5f;
    float corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);

    if (corner_radius > 0.0f) {
        SceneData::RoundedRectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = width;
        rect.max_y = height;
        rect.radius_top_left = corner_radius;
        rect.radius_top_right = corner_radius;
        rect.radius_bottom_left = corner_radius;
        rect.radius_bottom_right = corner_radius;
        rect.color = color;

        bucket.command_payload.resize(sizeof(SceneData::RoundedRectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RoundedRectCommand));
        bucket.command_kinds = {static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect)};
    } else {
        SceneData::RectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = width;
        rect.max_y = height;
        rect.color = color;

        bucket.command_payload.resize(sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RectCommand));
        bucket.command_kinds = {static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect)};
    }
    if (state.focused) {
        auto highlight = lighten_color(style.background_color, 0.35f);
        append_focus_highlight_preview(bucket, width, height, "widget/gallery/button", highlight, true);
    }
    return bucket;
}

auto build_toggle_preview(Widgets::ToggleStyle const& style,
                          Widgets::ToggleState const& state) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    float width = std::max(style.width, 16.0f);
    float height = std::max(style.height, 16.0f);

    bucket.drawable_ids = {0x701701u, 0x701702u};
    bucket.world_transforms = {identity_transform(), identity_transform()};

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    trackSphere.radius = std::sqrt(trackSphere.center[0] * trackSphere.center[0]
                                   + trackSphere.center[1] * trackSphere.center[1]);

    float thumbRadius = height * 0.5f - 2.0f;
    float thumbCenterX = state.checked ? (width - thumbRadius - 2.0f) : (thumbRadius + 2.0f);

    SceneData::BoundingSphere thumbSphere{};
    thumbSphere.center = {thumbCenterX, height * 0.5f, 0.0f};
    thumbSphere.radius = thumbRadius;

    bucket.bounds_spheres = {trackSphere, thumbSphere};

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, 0.0f, 0.0f};
    trackBox.max = {width, height, 0.0f};

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumbCenterX - thumbRadius, height * 0.5f - thumbRadius, 0.0f};
    thumbBox.max = {thumbCenterX + thumbRadius, height * 0.5f + thumbRadius, 0.0f};

    bucket.bounds_boxes = {trackBox, thumbBox};
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 1};
    bucket.z_values = {0.0f, 0.1f};
    bucket.material_ids = {0, 0};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0, 1};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        SceneData::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "widget/toggle/track", 0, 0},
        SceneData::DrawableAuthoringMapEntry{bucket.drawable_ids[1], "widget/toggle/thumb", 0, 0},
    };
    bucket.drawable_fingerprints = {0x701701u, 0x701702u};

    auto track_color = state.checked ? style.track_on_color : style.track_off_color;
    auto thumb_color = style.thumb_color;
    if (!state.enabled) {
        track_color = desaturate(track_color, 0.5f);
        thumb_color = desaturate(thumb_color, 0.5f);
    } else if (state.hovered) {
        track_color = lighten(track_color, 0.1f);
        thumb_color = lighten(thumb_color, 0.1f);
    }

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = 0.0f;
    trackRect.max_x = width;
    trackRect.max_y = height;
    trackRect.radius_top_left = height * 0.5f;
    trackRect.radius_top_right = height * 0.5f;
    trackRect.radius_bottom_right = height * 0.5f;
    trackRect.radius_bottom_left = height * 0.5f;
    trackRect.color = track_color;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumbRadius;
    thumbRect.radius_top_right = thumbRadius;
    thumbRect.radius_bottom_right = thumbRadius;
    thumbRect.radius_bottom_left = thumbRadius;
    thumbRect.color = thumb_color;

    auto payload_track = sizeof(SceneData::RoundedRectCommand);
    auto payload_thumb = sizeof(SceneData::RoundedRectCommand);
    bucket.command_payload.resize(payload_track + payload_thumb);
    std::memcpy(bucket.command_payload.data(), &trackRect, payload_track);
    std::memcpy(bucket.command_payload.data() + payload_track, &thumbRect, payload_thumb);
    bucket.command_kinds = {
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
    };
    if (state.focused) {
        auto base = state.checked ? style.track_on_color : style.track_off_color;
        auto highlight = lighten_color(base, 0.30f);
        append_focus_highlight_preview(bucket, width, height, "widget/gallery/toggle", highlight, true);
    }
    return bucket;
}

auto build_slider_preview(Widgets::SliderStyle const& style,
                          Widgets::SliderState const& state,
                          Widgets::SliderRange const& range) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    float width = std::max(style.width, 1.0f);
    float height = std::max(style.height, 16.0f);
    float track_height = std::clamp(style.track_height, 1.0f, height);
    float thumb_radius = std::clamp(style.thumb_radius, track_height * 0.5f, height * 0.5f);

    bucket.drawable_ids = {0x51D301u, 0x51D302u, 0x51D303u};
    bucket.world_transforms = {identity_transform(), identity_transform(), identity_transform()};

    float clamped_min = std::min(range.minimum, range.maximum);
    float clamped_max = std::max(range.minimum, range.maximum);
    float value = std::clamp(state.value, clamped_min, clamped_max);
    float denom = std::max(clamped_max - clamped_min, 1e-6f);
    float progress = std::clamp((value - clamped_min) / denom, 0.0f, 1.0f);

    float center_y = height * 0.5f;
    float track_half = track_height * 0.5f;
    float track_top = center_y - track_half;
    float fill_width = std::max(progress * width, 0.0f);
    float thumb_x = std::clamp(progress * width, thumb_radius, width - thumb_radius);

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, track_top, 0.0f};
    trackBox.max = {width, track_top + track_height, 0.0f};

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {width * 0.5f, center_y, 0.0f};
    trackSphere.radius = std::sqrt(std::pow(track_half, 2.0f) + std::pow(width * 0.5f, 2.0f));

    SceneData::BoundingBox fillBox{};
    fillBox.min = {0.0f, track_top, 0.0f};
    fillBox.max = {fill_width, track_top + track_height, 0.0f};

    SceneData::BoundingSphere fillSphere{};
    fillSphere.center = {fill_width * 0.5f, center_y, 0.0f};
    fillSphere.radius = std::sqrt(std::pow(fillSphere.center[0], 2.0f) + track_half * track_half);

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumb_x - thumb_radius, center_y - thumb_radius, 0.0f};
    thumbBox.max = {thumb_x + thumb_radius, center_y + thumb_radius, 0.0f};

    SceneData::BoundingSphere thumbSphere{};
    thumbSphere.center = {thumb_x, center_y, 0.0f};
    thumbSphere.radius = thumb_radius;

    bucket.bounds_boxes = {trackBox, fillBox, thumbBox};
    bucket.bounds_box_valid = {1, 1, 1};
    bucket.bounds_spheres = {trackSphere, fillSphere, thumbSphere};
    bucket.layers = {0, 1, 2};
    bucket.z_values = {0.0f, 0.05f, 0.1f};
    bucket.material_ids = {0, 0, 0};
    bucket.pipeline_flags = {0, 0, 0};
    bucket.visibility = {1, 1, 1};
    bucket.command_offsets = {0, 1, 2};
    bucket.command_counts = {1, 1, 1};
    bucket.opaque_indices = {0, 1, 2};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1, -1, -1};
    bucket.authoring_map = {
        SceneData::DrawableAuthoringMapEntry{bucket.drawable_ids[0], "widget/slider/track", 0, 0},
        SceneData::DrawableAuthoringMapEntry{bucket.drawable_ids[1], "widget/slider/fill", 0, 0},
        SceneData::DrawableAuthoringMapEntry{bucket.drawable_ids[2], "widget/slider/thumb", 0, 0},
    };
    bucket.drawable_fingerprints = {0x51D301u, 0x51D302u, 0x51D303u};

    auto track_color = style.track_color;
    auto fill_color = style.fill_color;
    auto thumb_color = style.thumb_color;
    if (!state.enabled) {
        track_color = desaturate(track_color, 0.5f);
        fill_color = desaturate(fill_color, 0.5f);
        thumb_color = desaturate(thumb_color, 0.5f);
    } else {
        if (state.hovered) {
            track_color = lighten(track_color, 0.05f);
            fill_color = lighten(fill_color, state.dragging ? 0.2f : 0.1f);
            thumb_color = lighten(thumb_color, state.dragging ? 0.2f : 0.1f);
        }
    }

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = track_top;
    trackRect.max_x = width;
    trackRect.max_y = track_top + track_height;
    trackRect.radius_top_left = track_half;
    trackRect.radius_top_right = track_half;
    trackRect.radius_bottom_right = track_half;
    trackRect.radius_bottom_left = track_half;
    trackRect.color = track_color;

    SceneData::RectCommand fillRect{};
    fillRect.min_x = 0.0f;
    fillRect.min_y = track_top;
    fillRect.max_x = fill_width;
    fillRect.max_y = track_top + track_height;
    fillRect.color = fill_color;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumb_radius;
    thumbRect.radius_top_right = thumb_radius;
    thumbRect.radius_bottom_right = thumb_radius;
    thumbRect.radius_bottom_left = thumb_radius;
    thumbRect.color = thumb_color;

    auto payload_track = sizeof(SceneData::RoundedRectCommand);
    auto payload_fill = sizeof(SceneData::RectCommand);
    auto payload_thumb = sizeof(SceneData::RoundedRectCommand);
    bucket.command_payload.resize(payload_track + payload_fill + payload_thumb);
    std::uint8_t* payload_ptr = bucket.command_payload.data();
    std::memcpy(payload_ptr, &trackRect, payload_track);
    std::memcpy(payload_ptr + payload_track, &fillRect, payload_fill);
    std::memcpy(payload_ptr + payload_track + payload_fill, &thumbRect, payload_thumb);

    bucket.command_kinds = {
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect),
        static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
    };
    if (state.focused) {
        auto highlight = lighten_color(style.fill_color, 0.25f);
        append_focus_highlight_preview(bucket, width, height, "widget/gallery/slider", highlight, true);
    }
    return bucket;
}

static auto build_stack_preview(Widgets::StackLayoutStyle const& style,
                                Widgets::StackLayoutState const& layout,
                                Widgets::WidgetTheme const& theme,
                                StackPreviewLayout& preview) -> SceneData::DrawableBucketSnapshot {
    auto preview_result = Widgets::BuildStackPreview(
        style,
        layout,
        Widgets::StackPreviewOptions{
            .authoring_root = "widget/gallery/stack",
            .background_color = {0.10f, 0.12f, 0.16f, 1.0f},
            .child_start_color = theme.accent_text_color,
            .child_end_color = theme.caption_color,
            .child_opacity = 0.85f,
            .mix_scale = 0.6f,
        });

    preview.bounds = WidgetBounds{
        preview_result.layout.bounds.min_x,
        preview_result.layout.bounds.min_y,
        preview_result.layout.bounds.max_x,
        preview_result.layout.bounds.max_y,
    };
    preview.child_bounds.clear();
    preview.child_bounds.reserve(preview_result.layout.child_bounds.size());
    for (auto const& child : preview_result.layout.child_bounds) {
        preview.child_bounds.push_back(WidgetBounds{
            child.min_x,
            child.min_y,
            child.max_x,
            child.max_y,
        });
    }
    return std::move(preview_result.bucket);
}

static auto build_tree_preview(Widgets::TreeStyle const& style,
                               std::vector<Widgets::TreeNode> const& nodes,
                               Widgets::TreeState const& state,
                               Widgets::WidgetTheme const& theme,
                               TreeLayout& layout_info) -> SceneData::DrawableBucketSnapshot {
    auto preview_result = Widgets::BuildTreePreview(
        style,
        nodes,
        state,
        Widgets::TreePreviewOptions{
            .authoring_root = "widget/gallery/tree",
            .pulsing_highlight = state.focused,
        });

    layout_info.bounds = widget_bounds_from_preview_rect(preview_result.layout.bounds);
    layout_info.content_top = preview_result.layout.content_top;
    layout_info.row_height = preview_result.layout.row_height;
    layout_info.rows.clear();
    layout_info.rows.reserve(preview_result.layout.rows.size());

    auto bucket = std::move(preview_result.bucket);

    for (std::size_t index = 0; index < preview_result.layout.rows.size(); ++index) {
        auto const& row = preview_result.layout.rows[index];
        layout_info.rows.push_back(TreeRowLayout{
            .bounds = widget_bounds_from_preview_rect(row.row_bounds),
            .node_id = row.id,
            .label = row.label,
            .toggle = widget_bounds_from_preview_rect(row.toggle_bounds),
            .depth = row.depth,
            .expandable = row.expandable,
            .expanded = row.expanded,
            .loading = row.loading,
            .enabled = row.enabled && preview_result.layout.state.enabled,
        });

        auto const& layout_row = layout_info.rows.back();
        float const row_top = layout_row.bounds.min_y;
        float const toggle_right = layout_row.toggle.max_x;

        float label_x = toggle_right + 10.0f;
        float const label_height = style.label_typography.line_height;
        float text_top = row_top + std::max(0.0f, (layout_info.row_height - label_height) * 0.5f);
        float baseline = text_top + style.label_typography.baseline_shift;

        auto text_color = style.text_color;
        if (!layout_row.enabled || !state.enabled) {
            text_color = desaturate(text_color, 0.4f);
        }
        if (layout_row.loading) {
            text_color = lighten(text_color, 0.15f);
        }

        auto authoring_id = std::string("widget/gallery/tree/label/")
            + (layout_row.node_id.empty() ? "placeholder" : layout_row.node_id);

        auto label = build_text_bucket(layout_row.label.empty() ? "(node)" : layout_row.label,
                                       label_x,
                                       baseline,
                                       style.label_typography,
                                       text_color,
                                       0x41A30000ull + static_cast<std::uint64_t>(index),
                                       authoring_id,
                                       0.2f);
        if (label) {
            append_bucket(bucket, label->bucket);
        }
    }

    return bucket;
}

template <typename T>
auto unwrap_or_exit(SP::Expected<T> value, std::string const& context) -> T {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
    return *std::move(value);
}

auto unwrap_or_exit(SP::Expected<void> value, std::string const& context) -> void {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
}

template <typename T>
auto replace_value(PathSpace& space, std::string const& path, T const& value) -> void {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        std::cerr << "failed clearing '" << path << "'";
        if (error.message) {
            std::cerr << ": " << *error.message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        std::cerr << "failed writing '" << path << "'";
        if (inserted.errors.front().message) {
            std::cerr << ": " << *inserted.errors.front().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
}

template <typename Cmd>
auto read_command(std::vector<std::uint8_t> const& payload, std::size_t offset) -> Cmd {
    Cmd cmd{};
    std::memcpy(&cmd, payload.data() + offset, sizeof(Cmd));
    return cmd;
}

template <typename Cmd>
auto write_command(std::vector<std::uint8_t>& payload, std::size_t offset, Cmd const& cmd) -> void {
    std::memcpy(payload.data() + offset, &cmd, sizeof(Cmd));
}

auto translate_bucket(SceneData::DrawableBucketSnapshot& bucket, float dx, float dy) -> void {
    for (auto& sphere : bucket.bounds_spheres) {
        sphere.center[0] += dx;
        sphere.center[1] += dy;
    }
    for (auto& box : bucket.bounds_boxes) {
        box.min[0] += dx;
        box.max[0] += dx;
        box.min[1] += dy;
        box.max[1] += dy;
    }
    std::size_t offset = 0;
    for (auto kind_value : bucket.command_kinds) {
        auto kind = static_cast<SceneData::DrawCommandKind>(kind_value);
        switch (kind) {
        case SceneData::DrawCommandKind::Rect: {
            auto cmd = read_command<SceneData::RectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case SceneData::DrawCommandKind::RoundedRect: {
            auto cmd = read_command<SceneData::RoundedRectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case SceneData::DrawCommandKind::TextGlyphs: {
            auto cmd = read_command<SceneData::TextGlyphsCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        default:
            break;
        }
        offset += SceneData::payload_size_bytes(kind);
    }
}

auto append_bucket(SceneData::DrawableBucketSnapshot& dest,
                   SceneData::DrawableBucketSnapshot const& src) -> void {
    if (src.drawable_ids.empty()) {
        return;
    }
    auto drawable_base = static_cast<std::uint32_t>(dest.drawable_ids.size());
    auto command_base = static_cast<std::uint32_t>(dest.command_kinds.size());
    auto clip_base = static_cast<std::int32_t>(dest.clip_nodes.size());

    dest.drawable_ids.insert(dest.drawable_ids.end(), src.drawable_ids.begin(), src.drawable_ids.end());
    dest.world_transforms.insert(dest.world_transforms.end(), src.world_transforms.begin(), src.world_transforms.end());
    dest.bounds_spheres.insert(dest.bounds_spheres.end(), src.bounds_spheres.begin(), src.bounds_spheres.end());
    dest.bounds_boxes.insert(dest.bounds_boxes.end(), src.bounds_boxes.begin(), src.bounds_boxes.end());
    dest.bounds_box_valid.insert(dest.bounds_box_valid.end(), src.bounds_box_valid.begin(), src.bounds_box_valid.end());
    dest.layers.insert(dest.layers.end(), src.layers.begin(), src.layers.end());
    dest.z_values.insert(dest.z_values.end(), src.z_values.begin(), src.z_values.end());
    dest.material_ids.insert(dest.material_ids.end(), src.material_ids.begin(), src.material_ids.end());
    dest.pipeline_flags.insert(dest.pipeline_flags.end(), src.pipeline_flags.begin(), src.pipeline_flags.end());
    dest.visibility.insert(dest.visibility.end(), src.visibility.begin(), src.visibility.end());

    for (auto offset : src.command_offsets) {
        dest.command_offsets.push_back(offset + command_base);
    }
    dest.command_counts.insert(dest.command_counts.end(), src.command_counts.begin(), src.command_counts.end());

    dest.command_kinds.insert(dest.command_kinds.end(), src.command_kinds.begin(), src.command_kinds.end());
    dest.command_payload.insert(dest.command_payload.end(), src.command_payload.begin(), src.command_payload.end());

    for (auto index : src.opaque_indices) {
        dest.opaque_indices.push_back(index + drawable_base);
    }
    for (auto index : src.alpha_indices) {
        dest.alpha_indices.push_back(index + drawable_base);
    }

    for (auto const& entry : src.layer_indices) {
        SceneData::LayerIndices adjusted{entry.layer, {}};
        adjusted.indices.reserve(entry.indices.size());
        for (auto idx : entry.indices) {
            adjusted.indices.push_back(idx + drawable_base);
        }
        dest.layer_indices.push_back(std::move(adjusted));
    }

    for (auto node : src.clip_nodes) {
        if (node.next >= 0) {
            node.next += clip_base;
        }
        dest.clip_nodes.push_back(node);
    }
    for (auto head : src.clip_head_indices) {
        if (head >= 0) {
            dest.clip_head_indices.push_back(head + clip_base);
        } else {
            dest.clip_head_indices.push_back(-1);
        }
    }

    dest.authoring_map.insert(dest.authoring_map.end(), src.authoring_map.begin(), src.authoring_map.end());
    dest.drawable_fingerprints.insert(dest.drawable_fingerprints.end(),
                                      src.drawable_fingerprints.begin(),
                                      src.drawable_fingerprints.end());
}

auto build_text_bucket(std::string_view text,
                       float origin_x,
                       float origin_y,
                       Widgets::TypographyStyle const& typography,
                       std::array<float, 4> color,
                       std::uint64_t drawable_id,
                       std::string authoring_id,
                       float z_value) -> std::optional<TextBuildResult> {
    return TextBuilder::BuildTextBucket(text,
                                        origin_x,
                                        origin_y,
                                        typography,
                                        color,
                                        drawable_id,
                                        std::move(authoring_id),
                                        z_value);
}

static auto widget_bounds_from_text(TextBuildResult const& text) -> std::optional<WidgetBounds> {
    if (text.bucket.bounds_boxes.empty()) {
        return std::nullopt;
    }
    auto const& box = text.bucket.bounds_boxes.front();
    WidgetBounds bounds{
        box.min[0],
        box.min[1],
        box.max[0],
        box.max[1],
    };
    bounds.normalize();
    return bounds;
}

auto make_background_bucket(float width, float height) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    auto drawable_id = 0x9000FFF0ull;
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(identity_transform());

    SceneData::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {width, height, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    SceneData::BoundingSphere sphere{};
    sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0]
                              + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(0);
    bucket.z_values.push_back(0.0f);
    bucket.material_ids.push_back(0);
    bucket.pipeline_flags.push_back(0);
    bucket.visibility.push_back(1);
    bucket.command_offsets.push_back(0);
    bucket.command_counts.push_back(1);
    bucket.opaque_indices.push_back(0);
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_head_indices.push_back(-1);

    SceneData::RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = width;
    rect.max_y = height;
    rect.color = {0.11f, 0.12f, 0.15f, 1.0f};

    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));
    bucket.command_payload.resize(sizeof(SceneData::RectCommand));
    std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        drawable_id, "widget/gallery/background", 0, 0});
    bucket.drawable_fingerprints.push_back(drawable_id);
    return bucket;
}

struct GalleryBuildResult {
    SceneData::DrawableBucketSnapshot bucket;
    int width = 0;
    int height = 0;
    GalleryLayout layout;
};

auto build_gallery_bucket(PathSpace& space,
                          AppRootPathView appRoot,
                          Widgets::ButtonPaths const& button,
                          Widgets::ButtonStyle const& button_style,
                          Widgets::ButtonState const& button_state,
                          std::string const& button_label,
                          Widgets::TogglePaths const& toggle,
                          Widgets::ToggleStyle const& toggle_style,
                          Widgets::ToggleState const& toggle_state,
                          Widgets::SliderPaths const& slider,
                          Widgets::SliderStyle const& slider_style,
                          Widgets::SliderState const& slider_state,
                          Widgets::SliderRange const& slider_range,
                          Widgets::ListPaths const& list,
                          Widgets::ListStyle const& list_style,
                          Widgets::ListState const& list_state,
                          std::vector<Widgets::ListItem> const& list_items,
                          Widgets::StackLayoutParams const& stack_params,
                          Widgets::StackLayoutState const& stack_layout,
                          Widgets::TreePaths const& tree,
                          Widgets::TreeStyle const& tree_style,
                          Widgets::TreeState const& tree_state,
                          std::vector<Widgets::TreeNode> const& tree_nodes,
                          Widgets::WidgetTheme const& theme,
                          std::optional<std::string_view> focused_widget) -> GalleryBuildResult {
    (void)space;
    (void)appRoot;
    std::vector<SceneData::DrawableBucketSnapshot> pending;
    pending.reserve(16);

    float left = kDefaultMargin;
    float max_width = 0.0f;
    float max_height = 0.0f;
    std::uint64_t next_drawable_id = 0xA1000000ull;
    GalleryLayout layout{};

    // Title text
    Widgets::TypographyStyle heading_typography = theme.heading;
    float heading_line_height = heading_typography.line_height;
    auto title_text = build_text_bucket("PathSpace Widgets",
                                        left,
                                        kDefaultMargin + heading_typography.baseline_shift,
                                        heading_typography,
                                        theme.heading_color,
                                        next_drawable_id++,
                                        "widget/gallery/title",
                                        0.4f);
    float cursor_y = kDefaultMargin;
    if (title_text) {
        pending.emplace_back(std::move(title_text->bucket));
        max_width = std::max(max_width, left + title_text->width);
        max_height = std::max(max_height, cursor_y + heading_line_height);
    }
    cursor_y += heading_line_height + 24.0f;

    // Button widget
    {
        auto bucket = build_button_preview(button_style, button_state);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        float widget_height = button_style.height;
        max_width = std::max(max_width, left + button_style.width);
        max_height = std::max(max_height, cursor_y + widget_height);
        layout.button = WidgetBounds{
            left,
            cursor_y,
            left + button_style.width,
            cursor_y + widget_height,
        };

        float label_width = TextBuilder::MeasureTextWidth(button_label, button_style.typography);
        float label_line_height = button_style.typography.line_height;
       float label_x = left + std::max(0.0f, (button_style.width - label_width) * 0.5f);
       float label_top = cursor_y + std::max(0.0f, (button_style.height - label_line_height) * 0.5f);
       float label_y = label_top + button_style.typography.baseline_shift;
       auto label = build_text_bucket(button_label,
                                      label_x,
                                      label_y,
                                      button_style.typography,
                                      button_style.text_color,
                                      next_drawable_id++,
                                      "widget/gallery/button/label",
                                      0.6f);
        std::optional<WidgetBounds> button_label_bounds;
        if (label) {
            button_label_bounds = widget_bounds_from_text(*label);
            if (!button_label_bounds) {
                WidgetBounds fallback{
                    label_x,
                    label_top,
                    label_x + label->width,
                    label_top + label_line_height,
                };
                fallback.normalize();
                button_label_bounds = fallback;
            }
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, label_x + label->width);
            max_height = std::max(max_height, label_top + label_line_height);
        }
        layout.button_footprint = layout.button;
        if (button_label_bounds) {
            layout.button_footprint.include(*button_label_bounds);
        }
        layout.button_footprint.normalize();
        expand_for_focus_margin(layout.button_footprint);
        cursor_y += widget_height + 48.0f;
    }

    // Toggle widget
    {
        auto bucket = build_toggle_preview(toggle_style, toggle_state);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + toggle_style.width);
        max_height = std::max(max_height, cursor_y + toggle_style.height);
        layout.toggle = WidgetBounds{
            left,
            cursor_y,
            left + toggle_style.width,
            cursor_y + toggle_style.height,
        };

        Widgets::TypographyStyle toggle_label_typography = theme.caption;
        float toggle_label_line = toggle_label_typography.line_height;
        float toggle_label_x = left + toggle_style.width + 24.0f;
       float toggle_label_top = cursor_y + std::max(0.0f, (toggle_style.height - toggle_label_line) * 0.5f);
       auto label = build_text_bucket("Toggle",
                                      toggle_label_x,
                                      toggle_label_top + toggle_label_typography.baseline_shift,
                                      toggle_label_typography,
                                      theme.accent_text_color,
                                      next_drawable_id++,
                                      "widget/gallery/toggle/label",
                                      0.6f);
        std::optional<WidgetBounds> toggle_label_bounds;
        if (label) {
            toggle_label_bounds = widget_bounds_from_text(*label);
            if (!toggle_label_bounds) {
                WidgetBounds fallback{
                    toggle_label_x,
                    toggle_label_top,
                    toggle_label_x + label->width,
                    toggle_label_top + toggle_label_line,
                };
                fallback.normalize();
                toggle_label_bounds = fallback;
            }
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, toggle_label_x + label->width);
            max_height = std::max(max_height, toggle_label_top + toggle_label_line);
        }
        layout.toggle_footprint = layout.toggle;
        if (toggle_label_bounds) {
            layout.toggle_footprint.include(*toggle_label_bounds);
        }
        layout.toggle_footprint.normalize();
        expand_for_focus_margin(layout.toggle_footprint);
        cursor_y += toggle_style.height + 40.0f;
    }

    // Slider widget with label
    {
        std::optional<SceneData::DrawableBucketSnapshot> caption_bucket;
        std::string slider_caption = "Volume " + std::to_string(static_cast<int>(std::round(slider_state.value)));
        Widgets::TypographyStyle slider_caption_typography = slider_style.label_typography;
        float slider_caption_line = slider_caption_typography.line_height;
        auto caption = build_text_bucket(slider_caption,
                                         left,
                                         cursor_y + slider_caption_typography.baseline_shift,
                                         slider_caption_typography,
                                         slider_style.label_color,
                                         next_drawable_id++,
                                         "widget/gallery/slider/caption",
                                         0.6f);
        if (caption) {
            caption_bucket = std::move(caption->bucket);
            if (!caption_bucket->bounds_boxes.empty()) {
                auto const& bounds = caption_bucket->bounds_boxes.front();
                layout.slider_caption = WidgetBounds{
                    bounds.min[0],
                    bounds.min[1],
                    bounds.max[0],
                    bounds.max[1],
                };
            } else {
                float caption_min_y = cursor_y;
                float caption_max_y = cursor_y + slider_caption_line;
                layout.slider_caption = WidgetBounds{
                    left,
                    caption_min_y,
                    left + caption->width,
                    caption_max_y,
                };
            }
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + slider_caption_line);
        } else {
            layout.slider_caption.reset();
        }
        cursor_y += slider_caption_line + 12.0f;

        auto bucket = build_slider_preview(slider_style, slider_state, slider_range);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + slider_style.width);
        max_height = std::max(max_height, cursor_y + slider_style.height);
        layout.slider = WidgetBounds{
            left,
            cursor_y,
            left + slider_style.width,
            cursor_y + slider_style.height,
        };
        float slider_center_y = cursor_y + slider_style.height * 0.5f;
        float slider_half_track = slider_style.track_height * 0.5f;
        layout.slider_track = WidgetBounds{
            left,
            slider_center_y - slider_half_track,
            left + slider_style.width,
            slider_center_y + slider_half_track,
        };
        layout.slider_footprint = layout.slider;
        layout.slider_footprint.include(layout.slider_track);
        if (layout.slider_caption) {
            layout.slider_footprint.include(*layout.slider_caption);
        }
        layout.slider_footprint.normalize();
        expand_for_focus_margin(layout.slider_footprint);

        if (caption_bucket) {
            pending.emplace_back(std::move(*caption_bucket));
        }

        cursor_y += slider_style.height + 48.0f;
    }

    // List widget with per-item labels
    {
        Widgets::TypographyStyle list_caption_typography = theme.caption;
        float list_caption_line = list_caption_typography.line_height;
        std::optional<WidgetBounds> list_caption_bounds;
        auto caption = build_text_bucket("Inventory",
                                         left,
                                         cursor_y + list_caption_typography.baseline_shift,
                                         list_caption_typography,
                                         theme.caption_color,
                                         next_drawable_id++,
                                         "widget/gallery/list/caption",
                                         0.6f);
        if (caption) {
            list_caption_bounds = widget_bounds_from_text(*caption);
            if (!list_caption_bounds) {
                WidgetBounds fallback{
                    left,
                    cursor_y,
                    left + caption->width,
                    cursor_y + list_caption_line,
                };
                fallback.normalize();
                list_caption_bounds = fallback;
            }
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + list_caption_line);
        }
        layout.list_caption = list_caption_bounds;
        cursor_y += list_caption_line + 12.0f;

        auto preview = Widgets::BuildListPreview(list_style,
                                                 list_items,
                                                 list_state,
                                                 Widgets::ListPreviewOptions{
                                                     .authoring_root = "widget/gallery/list",
                                                     .label_inset = 16.0f,
                                                     .pulsing_highlight = true,
                                                 });
        translate_bucket(preview.bucket, left, cursor_y);
        pending.emplace_back(std::move(preview.bucket));
        max_width = std::max(max_width, left + preview.layout.bounds.max_x);
        max_height = std::max(max_height, cursor_y + preview.layout.bounds.max_y);

        layout.list.bounds = WidgetBounds{
            preview.layout.bounds.min_x + left,
            preview.layout.bounds.min_y + cursor_y,
            preview.layout.bounds.max_x + left,
            preview.layout.bounds.max_y + cursor_y,
        };
        layout.list.item_height = preview.layout.item_height;
        layout.list.content_top = cursor_y + preview.layout.content_top;
        layout.list.item_bounds.clear();
        layout.list.item_bounds.reserve(preview.layout.rows.size());
        for (auto const& row : preview.layout.rows) {
            layout.list.item_bounds.push_back(WidgetBounds{
                row.row_bounds.min_x + left,
                row.row_bounds.min_y + cursor_y,
                row.row_bounds.max_x + left,
                row.row_bounds.max_y + cursor_y,
            });
        }
        layout.list_footprint = layout.list.bounds;
        if (layout.list_caption) {
            layout.list_footprint.include(*layout.list_caption);
        }
        layout.list_footprint.normalize();
        expand_for_focus_margin(layout.list_footprint);

        auto const& sanitized_style = preview.layout.style;
        float list_height = preview.layout.bounds.height();
        std::size_t label_count = std::min(preview.layout.rows.size(), list_items.size());
        for (std::size_t index = 0; index < label_count; ++index) {
            auto const& row = preview.layout.rows[index];
            auto const& item = list_items[index];
            float label_x = left + row.label_bounds.min_x;
            float label_top = cursor_y + row.label_bounds.min_y;
            float label_baseline = cursor_y + row.label_baseline;
            auto label = build_text_bucket(item.label,
                                           label_x,
                                           label_baseline,
                                           sanitized_style.item_typography,
                                           sanitized_style.item_text_color,
                                           next_drawable_id++,
                                           "widget/gallery/list/item/" + row.id,
                                           0.65f);
            if (label) {
                pending.emplace_back(std::move(label->bucket));
                max_width = std::max(max_width, label_x + label->width);
                max_height = std::max(max_height, label_top + sanitized_style.item_typography.line_height);
            }

        }
        cursor_y += list_height + 48.0f;
    }

    // Stack layout preview
    {
        Widgets::TypographyStyle caption_typography = theme.caption;
        float caption_line = caption_typography.line_height;
        std::optional<WidgetBounds> stack_caption_bounds;
        auto caption = build_text_bucket("Stack layout preview",
                                         left,
                                         cursor_y + caption_typography.baseline_shift,
                                         caption_typography,
                                         theme.caption_color,
                                         next_drawable_id++,
                                         "widget/gallery/stack/caption",
                                         0.6f);
        if (caption) {
            stack_caption_bounds = widget_bounds_from_text(*caption);
            if (!stack_caption_bounds) {
                WidgetBounds fallback{
                    left,
                    cursor_y,
                    left + caption->width,
                    cursor_y + caption_line,
                };
                fallback.normalize();
                stack_caption_bounds = fallback;
            }
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + caption_line);
        }
        layout.stack_caption = stack_caption_bounds;
        cursor_y += caption_line + 12.0f;

        StackPreviewLayout stack_preview{};
        auto stack_bucket = build_stack_preview(stack_params.style,
                                                stack_layout,
                                                theme,
                                                stack_preview);
        translate_bucket(stack_bucket, left, cursor_y);
        pending.emplace_back(std::move(stack_bucket));

        layout.stack.bounds = WidgetBounds{
            stack_preview.bounds.min_x + left,
            stack_preview.bounds.min_y + cursor_y,
            stack_preview.bounds.max_x + left,
            stack_preview.bounds.max_y + cursor_y,
        };
        layout.stack.child_bounds.clear();
        layout.stack.child_bounds.reserve(stack_preview.child_bounds.size());
        for (auto const& child : stack_preview.child_bounds) {
            layout.stack.child_bounds.push_back(WidgetBounds{
                child.min_x + left,
                child.min_y + cursor_y,
                child.max_x + left,
                child.max_y + cursor_y,
            });
        }

        layout.stack_footprint = layout.stack.bounds;
        if (layout.stack_caption) {
            layout.stack_footprint.include(*layout.stack_caption);
        }
        layout.stack_footprint.normalize();
        expand_for_focus_margin(layout.stack_footprint);
        max_width = std::max(max_width, layout.stack.bounds.max_x);
        max_height = std::max(max_height, layout.stack.bounds.max_y);
        cursor_y += stack_preview.bounds.height() + 36.0f;
    }

    // Tree view preview
    {
        Widgets::TypographyStyle caption_typography = theme.caption;
        float caption_line = caption_typography.line_height;
        std::optional<WidgetBounds> tree_caption_bounds;
        auto caption = build_text_bucket("Tree view preview",
                                         left,
                                         cursor_y + caption_typography.baseline_shift,
                                         caption_typography,
                                         theme.caption_color,
                                         next_drawable_id++,
                                         "widget/gallery/tree/caption",
                                         0.6f);
        if (caption) {
            tree_caption_bounds = widget_bounds_from_text(*caption);
            if (!tree_caption_bounds) {
                WidgetBounds fallback{
                    left,
                    cursor_y,
                    left + caption->width,
                    cursor_y + caption_line,
                };
                fallback.normalize();
                tree_caption_bounds = fallback;
            }
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + caption_line);
        }
        layout.tree_caption = tree_caption_bounds;
        cursor_y += caption_line + 12.0f;

        TreeLayout tree_preview{};
        auto tree_bucket = build_tree_preview(tree_style,
                                              tree_nodes,
                                              tree_state,
                                              theme,
                                              tree_preview);
        translate_bucket(tree_bucket, left, cursor_y);
        pending.emplace_back(std::move(tree_bucket));

        layout.tree.bounds = WidgetBounds{
            tree_preview.bounds.min_x + left,
            tree_preview.bounds.min_y + cursor_y,
            tree_preview.bounds.max_x + left,
            tree_preview.bounds.max_y + cursor_y,
        };
        layout.tree.content_top = tree_preview.content_top + cursor_y;
        layout.tree.row_height = tree_preview.row_height;
        layout.tree.rows.clear();
        layout.tree.rows.reserve(tree_preview.rows.size());
        for (auto const& row : tree_preview.rows) {
            layout.tree.rows.push_back(TreeRowLayout{
                .bounds = WidgetBounds{
                    row.bounds.min_x + left,
                    row.bounds.min_y + cursor_y,
                    row.bounds.max_x + left,
                    row.bounds.max_y + cursor_y,
                },
                .node_id = row.node_id,
                .label = row.label,
                .toggle = WidgetBounds{
                    row.toggle.min_x + left,
                    row.toggle.min_y + cursor_y,
                    row.toggle.max_x + left,
                    row.toggle.max_y + cursor_y,
                },
                .depth = row.depth,
                .expandable = row.expandable,
                .expanded = row.expanded,
                .loading = row.loading,
                .enabled = row.enabled,
            });
        }

        layout.tree_footprint = layout.tree.bounds;
        if (layout.tree_caption) {
            layout.tree_footprint.include(*layout.tree_caption);
        }
        layout.tree_footprint.normalize();
        expand_for_focus_margin(layout.tree_footprint);
        max_width = std::max(max_width, layout.tree.bounds.max_x);
        max_height = std::max(max_height, layout.tree.bounds.max_y);
        cursor_y += tree_preview.bounds.height() + 48.0f;
    }

    // Footer hint
    Widgets::TypographyStyle footer_typography = theme.caption;
    float footer_line_height = footer_typography.line_height;
    auto footer = build_text_bucket("Close window to exit",
                                    left,
                                    cursor_y + footer_typography.baseline_shift,
                                    footer_typography,
                                    theme.muted_text_color,
                                    next_drawable_id++,
                                    "widget/gallery/footer",
                                    0.6f);
    if (footer) {
        pending.emplace_back(std::move(footer->bucket));
        max_width = std::max(max_width, left + footer->width);
        max_height = std::max(max_height, cursor_y + footer_line_height);
    }
    cursor_y += footer_line_height;

    float canvas_width = std::max(max_width + kDefaultMargin, 360.0f);
    float canvas_height = std::max(max_height + kDefaultMargin, 360.0f);

    SceneData::DrawableBucketSnapshot gallery{};
    auto background = make_background_bucket(canvas_width, canvas_height);
    append_bucket(gallery, background);

    for (auto const& bucket : pending) {
        append_bucket(gallery, bucket);
    }

    GalleryBuildResult result{};
    result.bucket = std::move(gallery);
    result.width = static_cast<int>(std::ceil(canvas_width));
    result.height = static_cast<int>(std::ceil(canvas_height));
    result.layout = std::move(layout);
    return result;
}

struct GallerySceneResult {
    ScenePath scene;
    int width = 0;
    int height = 0;
    GalleryLayout layout;
};

auto publish_gallery_scene(PathSpace& space,
                           AppRootPathView appRoot,
                           Widgets::ButtonPaths const& button,
                           Widgets::ButtonStyle const& button_style,
                           Widgets::ButtonState const& button_state,
                           std::string const& button_label,
                           Widgets::TogglePaths const& toggle,
                           Widgets::ToggleStyle const& toggle_style,
                           Widgets::ToggleState const& toggle_state,
                           Widgets::SliderPaths const& slider,
                           Widgets::SliderStyle const& slider_style,
                           Widgets::SliderState const& slider_state,
                           Widgets::SliderRange const& slider_range,
                           Widgets::ListPaths const& list,
                           Widgets::ListStyle const& list_style,
                           Widgets::ListState const& list_state,
                           std::vector<Widgets::ListItem> const& list_items,
                           Widgets::StackLayoutParams const& stack_params,
                           Widgets::StackLayoutState const& stack_layout,
                           Widgets::TreePaths const& tree,
                           Widgets::TreeStyle const& tree_style,
                           Widgets::TreeState const& tree_state,
                           std::vector<Widgets::TreeNode> const& tree_nodes,
                           Widgets::WidgetTheme const& theme,
                           std::optional<std::string> focused_widget_path = std::nullopt) -> GallerySceneResult {
    SceneParams gallery_params{
        .name = "gallery",
        .description = "widgets gallery composed scene",
    };
    auto gallery_scene = unwrap_or_exit(SceneBuilders::Create(space, appRoot, gallery_params),
                                        "create gallery scene");

    std::optional<std::string_view> focus_view;
    if (focused_widget_path && !focused_widget_path->empty()) {
        focus_view = *focused_widget_path;
    }

    auto build = build_gallery_bucket(space,
                                      appRoot,
                                      button,
                                      button_style,
                                      button_state,
                                      button_label,
                                      toggle,
                                      toggle_style,
                                      toggle_state,
                                      slider,
                                      slider_style,
                                      slider_state,
                                      slider_range,
                                      list,
                                      list_style,
                                      list_state,
                                      list_items,
                                      stack_params,
                                      stack_layout,
                                      tree,
                                      tree_style,
                                      tree_state,
                                      tree_nodes,
                                      theme,
                                      focus_view);

    SceneData::SceneSnapshotBuilder builder(space, appRoot, gallery_scene);
    SceneData::SnapshotPublishOptions opts{};
    opts.metadata.author = "widgets_example";
    opts.metadata.tool_version = "widgets_example";
    opts.metadata.created_at = std::chrono::system_clock::now();
    opts.metadata.drawable_count = build.bucket.drawable_ids.size();
    opts.metadata.command_count = build.bucket.command_kinds.size();

    unwrap_or_exit(builder.publish(opts, build.bucket), "publish gallery snapshot");
    unwrap_or_exit(SceneBuilders::WaitUntilReady(space,
                                         gallery_scene,
                                         std::chrono::milliseconds{50}),
                   "wait for gallery scene");

    return GallerySceneResult{
        .scene = gallery_scene,
        .width = build.width,
        .height = build.height,
        .layout = std::move(build.layout),
    };
}

enum class FocusTarget {
    Button,
    Toggle,
    Slider,
    List,
    Tree,
};

static auto focus_target_to_string(FocusTarget target) -> const char* {
    switch (target) {
    case FocusTarget::Button:
        return "Button";
    case FocusTarget::Toggle:
        return "Toggle";
    case FocusTarget::Slider:
        return "Slider";
    case FocusTarget::List:
        return "List";
    case FocusTarget::Tree:
        return "Tree";
    }
    return "Unknown";
}

struct WidgetsExampleContext;

static void write_debug_dump(WidgetsExampleContext const& ctx, std::filesystem::path const& path);

struct WidgetsExampleContext {
    PathSpace* space = nullptr;
    SP::App::AppRootPath app_root{std::string{}};
    Widgets::ButtonPaths button_paths;
    Widgets::TogglePaths toggle_paths;
    Widgets::SliderPaths slider_paths;
    Widgets::ListPaths list_paths;
    Widgets::StackPaths stack_paths;
    Widgets::TreePaths tree_paths;
    Widgets::WidgetTheme theme{};
    Widgets::ButtonStyle button_style{};
    std::string button_label;
    Widgets::ToggleStyle toggle_style{};
    Widgets::SliderStyle slider_style{};
    Widgets::ListStyle list_style{};
    Widgets::SliderRange slider_range{};
    std::vector<Widgets::ListItem> list_items;
    Widgets::StackLayoutParams stack_params{};
    Widgets::StackLayoutState stack_layout{};
    Widgets::TreeStyle tree_style{};
    Widgets::TreeState tree_state{};
    std::vector<Widgets::TreeNode> tree_nodes;
    WidgetBindings::ButtonBinding button_binding{};
    WidgetBindings::ToggleBinding toggle_binding{};
    WidgetBindings::SliderBinding slider_binding{};
    WidgetBindings::ListBinding list_binding{};
    WidgetBindings::StackBinding stack_binding{};
    WidgetBindings::TreeBinding tree_binding{};
    Widgets::ButtonState button_state{};
    Widgets::ToggleState toggle_state{};
    Widgets::SliderState slider_state{};
    Widgets::ListState list_state{};
    GallerySceneResult gallery{};
    std::string target_path;
    WidgetFocus::Config focus_config{};
    bool pointer_down = false;
    bool slider_dragging = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    FocusTarget focus_target = FocusTarget::Button;
    int focus_list_index = 0;
    int focus_tree_index = 0;
    std::string tree_pointer_down_id;
    bool tree_pointer_toggle = false;
    bool debug_capture_pending = false;
    int debug_capture_index = 0;
    bool debug_capture_after_refresh = false;
};

static void write_debug_dump(WidgetsExampleContext const& ctx, std::filesystem::path const& path) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "widgets_example: failed to write debug dump '" << path.string() << "'\n";
        return;
    }
    out << std::fixed << std::setprecision(3);
    out << "pointer=" << ctx.pointer_x << "," << ctx.pointer_y
        << " pointer_down=" << std::boolalpha << ctx.pointer_down
        << " slider_dragging=" << ctx.slider_dragging << '\n';
    out << "slider_state value=" << ctx.slider_state.value
        << " hovered=" << ctx.slider_state.hovered
        << " focused=" << ctx.slider_state.focused
        << " dragging=" << ctx.slider_state.dragging << '\n';
    out << "focus_target=" << focus_target_to_string(ctx.focus_target) << '\n';

    auto print_bounds = [&](std::string_view label, WidgetBounds const& bounds) {
        out << label << " min=(" << bounds.min_x << ',' << bounds.min_y
            << ") max=(" << bounds.max_x << ',' << bounds.max_y << ")\n";
    };

    print_bounds("button.footprint", ctx.gallery.layout.button_footprint);
    print_bounds("toggle.bounds", ctx.gallery.layout.toggle);
    print_bounds("toggle.footprint", ctx.gallery.layout.toggle_footprint);
    print_bounds("slider.bounds", ctx.gallery.layout.slider);
    print_bounds("slider.track", ctx.gallery.layout.slider_track);
    if (ctx.gallery.layout.slider_caption) {
        print_bounds("slider.caption", *ctx.gallery.layout.slider_caption);
    } else {
        out << "slider.caption=<missing>\n";
    }
    print_bounds("slider.footprint", ctx.gallery.layout.slider_footprint);
    print_bounds("list.bounds", ctx.gallery.layout.list.bounds);
    if (ctx.gallery.layout.list_caption) {
        print_bounds("list.caption", *ctx.gallery.layout.list_caption);
    } else {
        out << "list.caption=<missing>\n";
    }
    print_bounds("list.footprint", ctx.gallery.layout.list_footprint);
    print_bounds("stack.bounds", ctx.gallery.layout.stack.bounds);
    if (ctx.gallery.layout.stack_caption) {
        print_bounds("stack.caption", *ctx.gallery.layout.stack_caption);
    } else {
        out << "stack.caption=<missing>\n";
    }
    print_bounds("stack.footprint", ctx.gallery.layout.stack_footprint);
    print_bounds("tree.bounds", ctx.gallery.layout.tree.bounds);
    if (ctx.gallery.layout.tree_caption) {
        print_bounds("tree.caption", *ctx.gallery.layout.tree_caption);
    } else {
        out << "tree.caption=<missing>\n";
    }
    print_bounds("tree.footprint", ctx.gallery.layout.tree_footprint);

    auto const& rect = ctx.slider_binding.options.dirty_rect;
    out << "binding.dirty_rect min=(" << rect.min_x << ',' << rect.min_y
        << ") max=(" << rect.max_x << ',' << rect.max_y << ")\n";
    out << "binding.auto_render=" << ctx.slider_binding.options.auto_render << '\n';

    if (ctx.space) {
        auto damage_tiles = ctx.space->read<std::vector<Builders::DirtyRectHint>, std::string>(ctx.target_path + "/output/v1/common/damageTiles");
        if (damage_tiles) {
            out << "renderer.damage_tiles=" << damage_tiles->size() << '\n';
            for (std::size_t idx = 0; idx < damage_tiles->size(); ++idx) {
                auto const& tile = damage_tiles->at(idx);
                out << "  tile[" << idx << "]=(" << tile.min_x << ',' << tile.min_y
                    << ") -> (" << tile.max_x << ',' << tile.max_y << ")\n";
            }
        } else {
            out << "renderer.damage_tiles=<error:" << damage_tiles.error().message.value_or("unavailable") << ">\n";
        }
    }

    out << "gallery.size=" << ctx.gallery.width << "x" << ctx.gallery.height << '\n';
}

static void refresh_gallery(WidgetsExampleContext& ctx);

static auto center_of(WidgetBounds const& bounds) -> std::pair<float, float> {
    return {bounds.min_x + bounds.width() * 0.5f,
            bounds.min_y + bounds.height() * 0.5f};
}

struct PointerOverride {
    WidgetsExampleContext& ctx;
    float previous_x;
    float previous_y;

    PointerOverride(WidgetsExampleContext& context, float x, float y)
        : ctx(context), previous_x(context.pointer_x), previous_y(context.pointer_y) {
        ctx.pointer_x = x;
        ctx.pointer_y = y;
    }

    ~PointerOverride() {
        ctx.pointer_x = previous_x;
        ctx.pointer_y = previous_y;
    }
};

enum class TraceEventKind {
    MouseAbsolute,
    MouseRelative,
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    KeyUp,
};

struct TraceEvent {
    TraceEventKind kind = TraceEventKind::MouseAbsolute;
    double time_ms = 0.0;
    int x = -1;
    int y = -1;
    int dx = 0;
    int dy = 0;
    int wheel = 0;
    int button = 0;
    unsigned int keycode = 0;
    unsigned int modifiers = 0;
    char32_t character = U'\0';
    bool repeat = false;
};

class WidgetTrace {
public:
    void init_from_env();
    void record_mouse(SP::UI::LocalMouseEvent const& ev);
    void record_key(SP::UI::LocalKeyEvent const& ev);
    void flush();

    [[nodiscard]] auto recording() const -> bool { return record_enabled_; }
    [[nodiscard]] auto replaying() const -> bool { return replay_enabled_; }
    [[nodiscard]] auto record_path() const -> std::string const& { return record_path_; }
    [[nodiscard]] auto replay_path() const -> std::string const& { return replay_path_; }
    [[nodiscard]] auto events() const -> std::vector<TraceEvent> const& { return replay_events_; }

private:
    bool record_enabled_ = false;
    bool replay_enabled_ = false;
    bool start_time_valid_ = false;
    std::string record_path_;
    std::string replay_path_;
    std::vector<TraceEvent> recorded_events_;
    std::vector<TraceEvent> replay_events_;
    std::chrono::steady_clock::time_point start_time_{};

    void ensure_start();
    void append_record(TraceEvent event);
    [[nodiscard]] static auto kind_to_string(TraceEventKind kind) -> std::string_view;
    [[nodiscard]] static auto string_to_kind(std::string_view value) -> std::optional<TraceEventKind>;
    [[nodiscard]] auto format_event(TraceEvent const& event) const -> std::string;
    [[nodiscard]] static auto parse_line(std::string const& line) -> std::optional<TraceEvent>;
};

auto widget_trace() -> WidgetTrace& {
    static WidgetTrace trace{};
    return trace;
}

void WidgetTrace::ensure_start() {
    if (!start_time_valid_) {
        start_time_ = std::chrono::steady_clock::now();
        start_time_valid_ = true;
    }
}

void WidgetTrace::append_record(TraceEvent event) {
    ensure_start();
    auto now = std::chrono::steady_clock::now();
    event.time_ms = std::chrono::duration<double, std::milli>(now - start_time_).count();
    recorded_events_.push_back(event);
}

auto WidgetTrace::kind_to_string(TraceEventKind kind) -> std::string_view {
    switch (kind) {
    case TraceEventKind::MouseAbsolute:
        return "mouse_absolute";
    case TraceEventKind::MouseRelative:
        return "mouse_relative";
    case TraceEventKind::MouseDown:
        return "mouse_down";
    case TraceEventKind::MouseUp:
        return "mouse_up";
    case TraceEventKind::MouseWheel:
        return "mouse_wheel";
    case TraceEventKind::KeyDown:
        return "key_down";
    case TraceEventKind::KeyUp:
        return "key_up";
    }
    return "unknown";
}

auto WidgetTrace::string_to_kind(std::string_view value) -> std::optional<TraceEventKind> {
    if (value == "mouse_absolute") {
        return TraceEventKind::MouseAbsolute;
    }
    if (value == "mouse_relative") {
        return TraceEventKind::MouseRelative;
    }
    if (value == "mouse_down") {
        return TraceEventKind::MouseDown;
    }
    if (value == "mouse_up") {
        return TraceEventKind::MouseUp;
    }
    if (value == "mouse_wheel") {
        return TraceEventKind::MouseWheel;
    }
    if (value == "key_down") {
        return TraceEventKind::KeyDown;
    }
    if (value == "key_up") {
        return TraceEventKind::KeyUp;
    }
    return std::nullopt;
}

auto WidgetTrace::format_event(TraceEvent const& event) const -> std::string {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << event.time_ms
        << ' ' << "event=" << kind_to_string(event.kind)
        << " x=" << event.x
        << " y=" << event.y
        << " dx=" << event.dx
        << " dy=" << event.dy
        << " wheel=" << event.wheel
        << " button=" << event.button
        << " keycode=" << event.keycode
        << " modifiers=" << event.modifiers
        << " repeat=" << (event.repeat ? 1 : 0)
        << " char=" << static_cast<std::uint32_t>(event.character);
    return oss.str();
}

auto WidgetTrace::parse_line(std::string const& line) -> std::optional<TraceEvent> {
    if (line.empty()) {
        return std::nullopt;
    }
    std::istringstream iss(line);
    TraceEvent event{};
    if (!(iss >> event.time_ms)) {
        return std::nullopt;
    }
    std::string token;
    while (iss >> token) {
        auto pos = token.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        auto key = token.substr(0, pos);
        auto value = token.substr(pos + 1);
        if (key == "event") {
            auto kind = string_to_kind(value);
            if (!kind) {
                return std::nullopt;
            }
            event.kind = *kind;
        } else if (key == "x") {
            event.x = std::stoi(value);
        } else if (key == "y") {
            event.y = std::stoi(value);
        } else if (key == "dx") {
            event.dx = std::stoi(value);
        } else if (key == "dy") {
            event.dy = std::stoi(value);
        } else if (key == "wheel") {
            event.wheel = std::stoi(value);
        } else if (key == "button") {
            event.button = std::stoi(value);
        } else if (key == "keycode") {
            event.keycode = static_cast<unsigned int>(std::stoul(value));
        } else if (key == "modifiers") {
            event.modifiers = static_cast<unsigned int>(std::stoul(value));
        } else if (key == "repeat") {
            event.repeat = (value != "0");
        } else if (key == "char") {
            event.character = static_cast<char32_t>(std::stoul(value));
        }
    }
    return event;
}

void WidgetTrace::init_from_env() {
    if (record_enabled_ || replay_enabled_) {
        return;
    }

    if (const char* replay = std::getenv("WIDGETS_EXAMPLE_TRACE_REPLAY")) {
        if (replay && replay[0] != '\0') {
            replay_enabled_ = true;
            replay_path_ = replay;
            std::ifstream input(replay_path_);
            if (!input) {
                std::cerr << "widgets_example: failed to open replay trace '" << replay_path_ << "'\n";
                std::exit(1);
            }
            std::string line;
            while (std::getline(input, line)) {
                auto trimmed_begin = line.find_first_not_of(" \t");
                if (trimmed_begin == std::string::npos) {
                    continue;
                }
                auto trimmed_end = line.find_last_not_of(" \t");
                auto trimmed = line.substr(trimmed_begin, trimmed_end - trimmed_begin + 1);
                auto parsed = parse_line(trimmed);
                if (parsed) {
                    replay_events_.push_back(*parsed);
                }
            }
            if (replay_events_.empty()) {
                std::cerr << "widgets_example: replay trace '" << replay_path_ << "' contained no events\n";
            }
        }
    }

    if (replay_enabled_) {
        return;
    }

    if (const char* record = std::getenv("WIDGETS_EXAMPLE_TRACE_RECORD")) {
        if (record && record[0] != '\0') {
            record_enabled_ = true;
            record_path_ = record;
            start_time_valid_ = false;
            recorded_events_.clear();
        }
    }
}

void WidgetTrace::record_mouse(SP::UI::LocalMouseEvent const& ev) {
    if (!record_enabled_) {
        return;
    }
    TraceEvent event{};
    switch (ev.type) {
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        event.kind = TraceEventKind::MouseAbsolute;
        break;
    case SP::UI::LocalMouseEventType::Move:
        event.kind = TraceEventKind::MouseRelative;
        break;
    case SP::UI::LocalMouseEventType::ButtonDown:
        event.kind = TraceEventKind::MouseDown;
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        event.kind = TraceEventKind::MouseUp;
        break;
    case SP::UI::LocalMouseEventType::Wheel:
        event.kind = TraceEventKind::MouseWheel;
        break;
    }
    event.x = ev.x;
    event.y = ev.y;
    event.dx = ev.dx;
    event.dy = ev.dy;
    event.wheel = ev.wheel;
    event.button = static_cast<int>(ev.button);
    append_record(event);
}

void WidgetTrace::record_key(SP::UI::LocalKeyEvent const& ev) {
    if (!record_enabled_) {
        return;
    }
    TraceEvent event{};
    event.kind = (ev.type == SP::UI::LocalKeyEventType::KeyDown)
                     ? TraceEventKind::KeyDown
                     : TraceEventKind::KeyUp;
    event.keycode = ev.keycode;
    event.modifiers = ev.modifiers;
    event.repeat = ev.repeat;
    event.character = ev.character;
    append_record(event);
}

void WidgetTrace::flush() {
    if (!record_enabled_) {
        return;
    }
    namespace fs = std::filesystem;
    try {
        fs::path path(record_path_);
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }
        std::ofstream output(record_path_);
        if (!output) {
            std::cerr << "widgets_example: failed to open trace output '" << record_path_ << "'\n";
            return;
        }
        for (auto const& event : recorded_events_) {
            output << format_event(event) << '\n';
        }
        output.flush();
        std::cout << "widgets_example: captured " << recorded_events_.size()
                  << " events to '" << record_path_ << "'\n";
    } catch (std::exception const& ex) {
        std::cerr << "widgets_example: failed writing trace '" << record_path_
                  << "': " << ex.what() << "\n";
    }
}

static auto slider_thumb_position(WidgetsExampleContext const& ctx, float value) -> std::pair<float, float> {
    auto const& bounds = ctx.gallery.layout.slider;
    if (bounds.width() <= 0.0f) {
        return center_of(bounds);
    }
    float clamped = std::clamp(value, ctx.slider_range.minimum, ctx.slider_range.maximum);
    float range = ctx.slider_range.maximum - ctx.slider_range.minimum;
    float t = range > 0.0f ? (clamped - ctx.slider_range.minimum) / range : 0.0f;
    float x = bounds.min_x + bounds.width() * std::clamp(t, 0.0f, 1.0f);
    float y = bounds.min_y + bounds.height() * 0.5f;
    return {x, y};
}

static auto list_item_center(WidgetsExampleContext const& ctx, int index) -> std::pair<float, float> {
    if (index < 0 || index >= static_cast<int>(ctx.gallery.layout.list.item_bounds.size())) {
        return center_of(ctx.gallery.layout.list.bounds);
    }
    return center_of(ctx.gallery.layout.list.item_bounds[static_cast<std::size_t>(index)]);
}

static auto tree_row_index_from_position(WidgetsExampleContext const& ctx, float y) -> int {
    auto const& rows = ctx.gallery.layout.tree.rows;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto const& bounds = rows[i].bounds;
        if (y >= bounds.min_y && y <= bounds.max_y) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static auto tree_row_center(WidgetsExampleContext const& ctx, int index) -> std::pair<float, float> {
    auto const& rows = ctx.gallery.layout.tree.rows;
    if (index < 0 || index >= static_cast<int>(rows.size())) {
        return center_of(ctx.gallery.layout.tree.bounds);
    }
    return center_of(rows[static_cast<std::size_t>(index)].bounds);
}

static bool tree_toggle_contains(WidgetsExampleContext const& ctx,
                                 int index,
                                 float x,
                                 float y) {
    auto const& rows = ctx.gallery.layout.tree.rows;
    if (index < 0 || index >= static_cast<int>(rows.size())) {
        return false;
    }
    auto const& row = rows[static_cast<std::size_t>(index)];
    if (!row.expandable) {
        return false;
    }
    auto const& bounds = row.toggle;
    return x >= bounds.min_x && x <= bounds.max_x && y >= bounds.min_y && y <= bounds.max_y;
}

static int tree_parent_index(WidgetsExampleContext const& ctx, int index) {
    auto const& rows = ctx.gallery.layout.tree.rows;
    if (index <= 0 || index >= static_cast<int>(rows.size())) {
        return -1;
    }
    int depth = rows[static_cast<std::size_t>(index)].depth;
    for (int i = index - 1; i >= 0; --i) {
        if (rows[static_cast<std::size_t>(i)].depth < depth) {
            return i;
        }
    }
    return -1;
}

static int tree_first_child_index(WidgetsExampleContext const& ctx, int index) {
    auto const& rows = ctx.gallery.layout.tree.rows;
    if (index < 0 || index >= static_cast<int>(rows.size() - 1)) {
        return -1;
    }
    int depth = rows[static_cast<std::size_t>(index)].depth;
    auto const& next = rows[static_cast<std::size_t>(index + 1)];
    if (next.depth > depth) {
        return index + 1;
    }
    return -1;
}

static auto slider_keyboard_step(WidgetsExampleContext const& ctx) -> float {
    float step = ctx.slider_range.step;
    if (step <= 0.0f) {
        float range = ctx.slider_range.maximum - ctx.slider_range.minimum;
        step = std::max(range * 0.05f, 0.01f);
    }
    return step;
}

static auto focus_widget_for_target(WidgetsExampleContext const& ctx,
                                    FocusTarget target) -> WidgetPath const* {
    switch (target) {
    case FocusTarget::Button:
        return &ctx.button_paths.root;
    case FocusTarget::Toggle:
        return &ctx.toggle_paths.root;
    case FocusTarget::Slider:
        return &ctx.slider_paths.root;
    case FocusTarget::List:
        return &ctx.list_paths.root;
    case FocusTarget::Tree:
        return &ctx.tree_paths.root;
    }
    return nullptr;
}

static std::optional<FocusTarget> focus_target_from_path(WidgetsExampleContext const& ctx,
                                                         std::string const& widget_path) {
    if (widget_path == ctx.button_paths.root.getPath()) {
        return FocusTarget::Button;
    }
    if (widget_path == ctx.toggle_paths.root.getPath()) {
        return FocusTarget::Toggle;
    }
    if (widget_path == ctx.slider_paths.root.getPath()) {
        return FocusTarget::Slider;
    }
    if (widget_path == ctx.list_paths.root.getPath()) {
        return FocusTarget::List;
    }
    if (widget_path == ctx.tree_paths.root.getPath()) {
        return FocusTarget::Tree;
    }
    return std::nullopt;
}

static bool reload_widget_states(WidgetsExampleContext& ctx) {
    if (!ctx.space) {
        return false;
    }

    bool updated = false;

    if (auto state = ctx.space->read<Widgets::ButtonState, std::string>(std::string(ctx.button_paths.state.getPath()))) {
        ctx.button_state = *state;
        updated = true;
    }

    if (auto state = ctx.space->read<Widgets::ToggleState, std::string>(std::string(ctx.toggle_paths.state.getPath()))) {
        ctx.toggle_state = *state;
        updated = true;
    }

    if (auto state = ctx.space->read<Widgets::SliderState, std::string>(std::string(ctx.slider_paths.state.getPath()))) {
        ctx.slider_state = *state;
        updated = true;
    }

    if (auto state = ctx.space->read<Widgets::ListState, std::string>(std::string(ctx.list_paths.state.getPath()))) {
        ctx.list_state = *state;
        ctx.focus_list_index = ctx.list_state.hovered_index >= 0 ? ctx.list_state.hovered_index
                                                                 : ctx.list_state.selected_index;
        updated = true;
    }

    if (auto state = ctx.space->read<Widgets::TreeState, std::string>(std::string(ctx.tree_paths.state.getPath()))) {
        ctx.tree_state = *state;
        if (!ctx.tree_state.hovered_id.empty()) {
            auto const& rows = ctx.gallery.layout.tree.rows;
            auto it = std::find_if(rows.begin(), rows.end(), [&](TreeRowLayout const& row) {
                return row.node_id == ctx.tree_state.hovered_id;
            });
            if (it != rows.end()) {
                ctx.focus_tree_index = static_cast<int>(std::distance(rows.begin(), it));
            }
        }
        updated = true;
    }

    return updated;
}

static bool refresh_focus_target_from_space(WidgetsExampleContext& ctx) {
    if (!ctx.space || ctx.focus_config.focus_state.getPath().empty()) {
        return false;
    }
    auto focus_state = WidgetFocus::Current(*ctx.space,
                                            SP::ConcretePathStringView{ctx.focus_config.focus_state.getPath()});
    if (!focus_state) {
        std::cerr << "widgets_example: unable to read focus state: "
                  << focus_state.error().message.value_or("unknown error") << "\n";
        return false;
    }

    auto previous = ctx.focus_target;
    if (focus_state->has_value()) {
        if (auto mapped = focus_target_from_path(ctx, **focus_state)) {
            ctx.focus_target = *mapped;
        }
    }
    bool const changed = (ctx.focus_target != previous);

    if (changed
        && previous == FocusTarget::Slider
        && ctx.focus_target != FocusTarget::Slider
        && debug_capture_enabled()
        && ctx.debug_capture_index < 32) {
        ctx.debug_capture_after_refresh = true;
    }

    return changed;
}

static bool set_focus_target(WidgetsExampleContext& ctx,
                             FocusTarget target,
                             bool update_visuals = true) {
    bool target_changed = (ctx.focus_target != target);
    ctx.focus_target = target;

    bool changed = false;
    if (update_visuals && ctx.space) {
        if (auto* widget_path = focus_widget_for_target(ctx, target)) {
            auto set_result = WidgetFocus::Set(*ctx.space, ctx.focus_config, *widget_path);
            if (!set_result) {
                std::cerr << "widgets_example: failed to set focus state: "
                          << set_result.error().message.value_or("unknown error") << "\n";
            } else {
                changed |= set_result->changed;
            }
        }
        changed |= reload_widget_states(ctx);
        changed |= refresh_focus_target_from_space(ctx);
        if (debug_capture_enabled() && (changed || target_changed) && ctx.debug_capture_index < 32) {
            ctx.debug_capture_after_refresh = true;
        }
    }
    return changed || target_changed;
}

static void cycle_focus(WidgetsExampleContext& ctx, bool forward) {
    constexpr FocusTarget order[] = {
        FocusTarget::Button,
        FocusTarget::Toggle,
        FocusTarget::Slider,
        FocusTarget::List,
        FocusTarget::Tree,
    };
    int current = 0;
    for (std::size_t i = 0; i < std::size(order); ++i) {
        if (order[i] == ctx.focus_target) {
            current = static_cast<int>(i);
            break;
        }
    }
    int delta = forward ? 1 : -1;
    int next = (current + delta + static_cast<int>(std::size(order))) % static_cast<int>(std::size(order));
    if (set_focus_target(ctx, order[static_cast<std::size_t>(next)])) {
        refresh_gallery(ctx);
    }
}

static auto make_pointer_info(WidgetsExampleContext const& ctx, bool inside) -> WidgetBindings::PointerInfo {
    WidgetBindings::PointerInfo info{};
    info.scene_x = ctx.pointer_x;
    info.scene_y = ctx.pointer_y;
    info.inside = inside;
    info.primary = true;
    return info;
}

static auto slider_value_from_position(WidgetsExampleContext const& ctx, float x) -> float {
    auto const& bounds = ctx.gallery.layout.slider;
    float width = bounds.width();
    if (width <= 0.0f) {
        return ctx.slider_range.minimum;
    }
    float t = (x - bounds.min_x) / width;
    t = std::clamp(t, 0.0f, 1.0f);
    return ctx.slider_range.minimum + t * (ctx.slider_range.maximum - ctx.slider_range.minimum);
}

static auto list_index_from_position(WidgetsExampleContext const& ctx, float y) -> int {
    auto const& layout = ctx.gallery.layout.list;
    if (layout.item_height <= 0.0f || layout.item_bounds.empty()) {
        return -1;
    }
    float relative = y - layout.content_top;
    if (relative < 0.0f) {
        return -1;
    }
    int index = static_cast<int>(std::floor(relative / layout.item_height));
    if (index < 0 || index >= static_cast<int>(layout.item_bounds.size())) {
        return -1;
    }
    return index;
}

static void refresh_gallery(WidgetsExampleContext& ctx) {
    auto view = SP::App::AppRootPathView{ctx.app_root.getPath()};
    std::optional<std::string> focused_widget_path;
    if (ctx.space && !ctx.focus_config.focus_state.getPath().empty()) {
        auto focus_state_view = SP::ConcretePathStringView{ctx.focus_config.focus_state.getPath()};
        auto focus_state = WidgetFocus::Current(*ctx.space, focus_state_view);
        if (!focus_state) {
            std::cerr << "widgets_example: unable to read focus state: "
                      << focus_state.error().message.value_or("unknown error") << "\n";
        } else if (focus_state->has_value()) {
            focused_widget_path = **focus_state;
        }
    }

    std::optional<std::string> resolved_focus_path = focused_widget_path;
    if (!resolved_focus_path || resolved_focus_path->empty()) {
        switch (ctx.focus_target) {
        case FocusTarget::Button:
            resolved_focus_path = ctx.button_paths.root.getPath();
            break;
        case FocusTarget::Toggle:
            resolved_focus_path = ctx.toggle_paths.root.getPath();
            break;
        case FocusTarget::Slider:
            resolved_focus_path = ctx.slider_paths.root.getPath();
            break;
        case FocusTarget::List:
            resolved_focus_path = ctx.list_paths.root.getPath();
            break;
        case FocusTarget::Tree:
            resolved_focus_path = ctx.tree_paths.root.getPath();
            break;
        }
    }

    auto const focus_matches = [&](WidgetPath const& path) {
        return resolved_focus_path && *resolved_focus_path == path.getPath();
    };

    auto copy_with_focus = [&](auto const& state, WidgetPath const& path) {
        auto copy = state;
        bool should_focus = focus_matches(path);
        if (copy.focused != should_focus) {
            copy.focused = should_focus;
        }
        return copy;
    };

    auto button_preview_state = copy_with_focus(ctx.button_state, ctx.button_paths.root);
    auto toggle_preview_state = copy_with_focus(ctx.toggle_state, ctx.toggle_paths.root);
    auto slider_preview_state = copy_with_focus(ctx.slider_state, ctx.slider_paths.root);
    auto list_preview_state = copy_with_focus(ctx.list_state, ctx.list_paths.root);
    auto tree_preview_state = copy_with_focus(ctx.tree_state, ctx.tree_paths.root);

    if (debug_capture_enabled()) {
        auto print_focus = [&](char const* label, bool focused) {
            std::cout << "[focus] " << label << " preview_focused="
                      << std::boolalpha << focused << std::noboolalpha << '\n';
        };
        std::cout << "[focus] resolved_path="
                  << (resolved_focus_path ? *resolved_focus_path : std::string{"<none>"})
                  << " slider_state.focused=" << std::boolalpha << ctx.slider_state.focused
                  << " preview_slider.focused=" << slider_preview_state.focused
                  << std::noboolalpha << '\n';
        print_focus("button", button_preview_state.focused);
        print_focus("toggle", toggle_preview_state.focused);
        print_focus("list", list_preview_state.focused);
        print_focus("tree", tree_preview_state.focused);
    }

    ctx.gallery = publish_gallery_scene(*ctx.space,
                                        view,
                                        ctx.button_paths,
                                        ctx.button_style,
                                        button_preview_state,
                                        ctx.button_label,
                                        ctx.toggle_paths,
                                        ctx.toggle_style,
                                        toggle_preview_state,
                                        ctx.slider_paths,
                                        ctx.slider_style,
                                        slider_preview_state,
                                        ctx.slider_range,
                                        ctx.list_paths,
                                        ctx.list_style,
                                        list_preview_state,
                                        ctx.list_items,
                                        ctx.stack_params,
                                        ctx.stack_layout,
                                        ctx.tree_paths,
                                        ctx.tree_style,
                                        tree_preview_state,
                                        ctx.tree_nodes,
                                        ctx.theme,
                                        resolved_focus_path);

    if (debug_capture_enabled() && ctx.slider_state.dragging) {
        if (ctx.gallery.layout.slider_caption) {
            auto const& cap = *ctx.gallery.layout.slider_caption;
            std::cout << "[debug] slider caption bounds while dragging value="
                      << ctx.slider_state.value << " bounds=("
                      << cap.min_x << "," << cap.min_y << ") -> ("
                      << cap.max_x << "," << cap.max_y << ")\n";
        } else {
            std::cout << "[debug] slider caption missing while dragging value="
                      << ctx.slider_state.value << std::endl;
        }
        if (ctx.debug_capture_index < 32) {
            ctx.debug_capture_pending = true;
        }
    }

    if (debug_capture_enabled() && ctx.debug_capture_after_refresh && ctx.debug_capture_index < 32) {
        ctx.debug_capture_pending = true;
    }

    ctx.debug_capture_after_refresh = false;

}

static auto dispatch_button(WidgetsExampleContext& ctx,
                            Widgets::ButtonState const& desired,
                            WidgetBindings::WidgetOpKind kind,
                            bool inside) -> bool {
    auto pointer = make_pointer_info(ctx, inside);
    auto result = WidgetBindings::DispatchButton(*ctx.space,
                                                 ctx.button_binding,
                                                 desired,
                                                 kind,
                                                 pointer);
    if (!result) {
        std::cerr << "widgets_example: button dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        auto updated = ctx.space->read<Widgets::ButtonState, std::string>(std::string(ctx.button_paths.state.getPath()));
        if (updated) {
            ctx.button_state = *updated;
        } else {
            ctx.button_state = desired;
        }
    }
    return *result;
}

static auto dispatch_toggle(WidgetsExampleContext& ctx,
                             Widgets::ToggleState const& desired,
                             WidgetBindings::WidgetOpKind kind,
                             bool inside) -> bool {
    auto pointer = make_pointer_info(ctx, inside);
    auto result = WidgetBindings::DispatchToggle(*ctx.space,
                                                 ctx.toggle_binding,
                                                 desired,
                                                 kind,
                                                 pointer);
    if (!result) {
        std::cerr << "widgets_example: toggle dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        auto updated = ctx.space->read<Widgets::ToggleState, std::string>(std::string(ctx.toggle_paths.state.getPath()));
        if (updated) {
            ctx.toggle_state = *updated;
        } else {
            ctx.toggle_state = desired;
        }
    }
    return *result;
}

static auto dispatch_slider(WidgetsExampleContext& ctx,
                             Widgets::SliderState const& desired,
                             WidgetBindings::WidgetOpKind kind,
                             bool inside) -> bool {
    auto pointer = make_pointer_info(ctx, inside);
    auto result = WidgetBindings::DispatchSlider(*ctx.space,
                                                 ctx.slider_binding,
                                                 desired,
                                                 kind,
                                                 pointer);
    if (!result) {
        std::cerr << "widgets_example: slider dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        auto updated = ctx.space->read<Widgets::SliderState, std::string>(std::string(ctx.slider_paths.state.getPath()));
        if (updated) {
            ctx.slider_state = *updated;
        } else {
            ctx.slider_state = desired;
        }
    }
    return *result;
}

static auto dispatch_list(WidgetsExampleContext& ctx,
                           Widgets::ListState const& desired,
                           WidgetBindings::WidgetOpKind kind,
                           bool inside,
                           int item_index,
                           float scroll_delta) -> bool {
    auto pointer = make_pointer_info(ctx, inside);
    auto result = WidgetBindings::DispatchList(*ctx.space,
                                               ctx.list_binding,
                                               desired,
                                               kind,
                                               pointer,
                                               item_index,
                                               scroll_delta);
    if (!result) {
        std::cerr << "widgets_example: list dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        auto updated = ctx.space->read<Widgets::ListState, std::string>(std::string(ctx.list_paths.state.getPath()));
        if (updated) {
            ctx.list_state = *updated;
            ctx.focus_list_index = ctx.list_state.hovered_index >= 0 ? ctx.list_state.hovered_index
                                                                     : ctx.list_state.selected_index;
        } else {
            ctx.list_state = desired;
        }
    }
    return *result;
}

static auto dispatch_tree(WidgetsExampleContext& ctx,
                          Widgets::TreeState const& desired,
                          WidgetBindings::WidgetOpKind kind,
                          bool inside,
                          std::string_view node_id,
                          float scroll_delta) -> bool {
    auto pointer = make_pointer_info(ctx, inside);
    auto result = WidgetBindings::DispatchTree(*ctx.space,
                                               ctx.tree_binding,
                                               desired,
                                               kind,
                                               node_id,
                                               pointer,
                                               scroll_delta);
    if (!result) {
        std::cerr << "widgets_example: tree dispatch failed: "
                  << result.error().message.value_or("unknown error") << "\n";
        return false;
    }
    if (*result) {
        auto updated = ctx.space->read<Widgets::TreeState, std::string>(std::string(ctx.tree_paths.state.getPath()));
        if (updated) {
            ctx.tree_state = *updated;
            if (!ctx.tree_state.hovered_id.empty()) {
                auto const& rows = ctx.gallery.layout.tree.rows;
                auto it = std::find_if(rows.begin(), rows.end(), [&](TreeRowLayout const& row) {
                    return row.node_id == ctx.tree_state.hovered_id;
                });
                if (it != rows.end()) {
                    ctx.focus_tree_index = static_cast<int>(std::distance(rows.begin(), it));
                }
            }
        } else {
            ctx.tree_state = desired;
        }
    }
    return *result;
}

static void handle_pointer_move(WidgetsExampleContext& ctx, float x, float y) {
    ctx.pointer_x = x;
    ctx.pointer_y = y;
    bool changed = false;

    bool inside_button = ctx.gallery.layout.button.contains(x, y);
    if (!ctx.pointer_down) {
        if (inside_button != ctx.button_state.hovered) {
            Widgets::ButtonState desired = ctx.button_state;
            desired.hovered = inside_button;
            auto op = inside_button ? WidgetBindings::WidgetOpKind::HoverEnter
                                    : WidgetBindings::WidgetOpKind::HoverExit;
            changed |= dispatch_button(ctx, desired, op, inside_button);
        }
    } else if (ctx.button_state.pressed && !inside_button && ctx.button_state.hovered) {
        Widgets::ButtonState desired = ctx.button_state;
        desired.hovered = false;
        changed |= dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::HoverExit, false);
    }

    bool inside_toggle = ctx.gallery.layout.toggle.contains(x, y);
    if (inside_toggle != ctx.toggle_state.hovered) {
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.hovered = inside_toggle;
        auto op = inside_toggle ? WidgetBindings::WidgetOpKind::HoverEnter
                                : WidgetBindings::WidgetOpKind::HoverExit;
        changed |= dispatch_toggle(ctx, desired, op, inside_toggle);
    }

    bool inside_list = ctx.gallery.layout.list.bounds.contains(x, y);
    int hover_index = inside_list ? list_index_from_position(ctx, y) : -1;
    if (hover_index != ctx.list_state.hovered_index) {
        Widgets::ListState desired = ctx.list_state;
        desired.hovered_index = hover_index;
        changed |= dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListHover,
                                 inside_list,
                                 hover_index,
                                 0.0f);
    }
    if (hover_index >= 0) {
        ctx.focus_list_index = hover_index;
    }

    bool inside_tree = ctx.gallery.layout.tree.bounds.contains(x, y);
    int tree_index = inside_tree ? tree_row_index_from_position(ctx, y) : -1;
    std::string hovered_id;
    if (tree_index >= 0 && static_cast<std::size_t>(tree_index) < ctx.gallery.layout.tree.rows.size()) {
        hovered_id = ctx.gallery.layout.tree.rows[static_cast<std::size_t>(tree_index)].node_id;
    }
    if (hovered_id != ctx.tree_state.hovered_id) {
        Widgets::TreeState desired = ctx.tree_state;
        desired.hovered_id = hovered_id;
        changed |= dispatch_tree(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::TreeHover,
                                 inside_tree,
                                 hovered_id,
                                 0.0f);
    }
    if (tree_index >= 0) {
        ctx.focus_tree_index = tree_index;
    }

    if (ctx.slider_dragging) {
        Widgets::SliderState desired = ctx.slider_state;
        desired.dragging = true;
        desired.hovered = ctx.gallery.layout.slider.contains(x, y);
        desired.value = slider_value_from_position(ctx, x);
        changed |= dispatch_slider(ctx, desired, WidgetBindings::WidgetOpKind::SliderUpdate, desired.hovered);
    }

    if (changed) {
        refresh_gallery(ctx);
    }
}

static void handle_pointer_down(WidgetsExampleContext& ctx) {
    ctx.pointer_down = true;
    bool changed = false;
    ctx.tree_pointer_down_id.clear();
    ctx.tree_pointer_toggle = false;

    if (ctx.gallery.layout.button.contains(ctx.pointer_x, ctx.pointer_y)) {
        Widgets::ButtonState desired = ctx.button_state;
        desired.hovered = true;
        desired.pressed = true;
        changed |= dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::Press, true);
    }

    if (ctx.gallery.layout.toggle.contains(ctx.pointer_x, ctx.pointer_y)) {
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.hovered = true;
        changed |= dispatch_toggle(ctx, desired, WidgetBindings::WidgetOpKind::Press, true);
    }

    if (ctx.gallery.layout.slider.contains(ctx.pointer_x, ctx.pointer_y)) {
        ctx.slider_dragging = true;
        Widgets::SliderState desired = ctx.slider_state;
        desired.dragging = true;
        desired.hovered = true;
        desired.value = slider_value_from_position(ctx, ctx.pointer_x);
        changed |= dispatch_slider(ctx, desired, WidgetBindings::WidgetOpKind::SliderBegin, true);
    }

    if (ctx.gallery.layout.list.bounds.contains(ctx.pointer_x, ctx.pointer_y)) {
        int index = list_index_from_position(ctx, ctx.pointer_y);
        Widgets::ListState desired = ctx.list_state;
        desired.hovered_index = index;
        changed |= dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListHover,
                                 true,
                                 index,
                                 0.0f);
        ctx.focus_list_index = index;
    }

    if (ctx.gallery.layout.tree.bounds.contains(ctx.pointer_x, ctx.pointer_y)) {
        int index = tree_row_index_from_position(ctx, ctx.pointer_y);
        if (index >= 0) {
            ctx.focus_tree_index = index;
            auto const& row = ctx.gallery.layout.tree.rows[static_cast<std::size_t>(index)];
            Widgets::TreeState desired = ctx.tree_state;
            desired.hovered_id = row.node_id;
            ctx.tree_pointer_down_id = row.node_id;
            ctx.tree_pointer_toggle = tree_toggle_contains(ctx, index, ctx.pointer_x, ctx.pointer_y);
            changed |= dispatch_tree(ctx,
                                     desired,
                                     WidgetBindings::WidgetOpKind::TreeHover,
                                     true,
                                     row.node_id,
                                     0.0f);
        }
    }

    if (refresh_focus_target_from_space(ctx)) {
        reload_widget_states(ctx);
        changed = true;
    }

    if (changed) {
        refresh_gallery(ctx);
    }
}

static void handle_pointer_up(WidgetsExampleContext& ctx) {
    bool changed = false;

    bool inside_button = ctx.gallery.layout.button.contains(ctx.pointer_x, ctx.pointer_y);
    if (ctx.button_state.pressed) {
        Widgets::ButtonState desired = ctx.button_state;
        desired.pressed = false;
        desired.hovered = inside_button;
        changed |= dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::Release, inside_button);
        if (inside_button) {
            changed |= dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::Activate, true);
        }
    }

    bool inside_toggle = ctx.gallery.layout.toggle.contains(ctx.pointer_x, ctx.pointer_y);
    if (inside_toggle) {
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.hovered = true;
        desired.checked = !ctx.toggle_state.checked;
        changed |= dispatch_toggle(ctx, desired, WidgetBindings::WidgetOpKind::Toggle, true);
    }

    if (ctx.slider_dragging) {
        ctx.slider_dragging = false;
        bool inside_slider = ctx.gallery.layout.slider.contains(ctx.pointer_x, ctx.pointer_y);
        Widgets::SliderState desired = ctx.slider_state;
        desired.dragging = false;
        desired.hovered = inside_slider;
        desired.value = slider_value_from_position(ctx, ctx.pointer_x);
        changed |= dispatch_slider(ctx, desired, WidgetBindings::WidgetOpKind::SliderCommit, inside_slider);
    }

    bool inside_list = ctx.gallery.layout.list.bounds.contains(ctx.pointer_x, ctx.pointer_y);
    int index = inside_list ? list_index_from_position(ctx, ctx.pointer_y) : -1;
    if (inside_list && index >= 0) {
        Widgets::ListState desired = ctx.list_state;
        desired.selected_index = index;
        changed |= dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListSelect,
                                 true,
                                 index,
                                 0.0f);
        changed |= dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListActivate,
                                 true,
                                 index,
                                 0.0f);
        ctx.focus_list_index = index;
    }

    bool inside_tree = ctx.gallery.layout.tree.bounds.contains(ctx.pointer_x, ctx.pointer_y);
    int tree_index = inside_tree ? tree_row_index_from_position(ctx, ctx.pointer_y) : -1;
    if (!ctx.tree_pointer_down_id.empty() && tree_index >= 0
        && static_cast<std::size_t>(tree_index) < ctx.gallery.layout.tree.rows.size()) {
        auto const& row = ctx.gallery.layout.tree.rows[static_cast<std::size_t>(tree_index)];
        if (row.node_id == ctx.tree_pointer_down_id) {
            Widgets::TreeState desired = ctx.tree_state;
            desired.hovered_id = row.node_id;
            desired.selected_id = row.node_id;
            if (ctx.tree_pointer_toggle) {
                changed |= dispatch_tree(ctx,
                                         desired,
                                         WidgetBindings::WidgetOpKind::TreeToggle,
                                         inside_tree,
                                         row.node_id,
                                         0.0f);
                desired = ctx.tree_state;
                desired.hovered_id = row.node_id;
                desired.selected_id = row.node_id;
            }
            changed |= dispatch_tree(ctx,
                                     desired,
                                     WidgetBindings::WidgetOpKind::TreeSelect,
                                     inside_tree,
                                     row.node_id,
                                     0.0f);
            ctx.focus_tree_index = tree_index;
        }
    }
    ctx.tree_pointer_down_id.clear();
    ctx.tree_pointer_toggle = false;

    ctx.pointer_down = false;

    if (refresh_focus_target_from_space(ctx)) {
        reload_widget_states(ctx);
        changed = true;
    }

    if (changed) {
        refresh_gallery(ctx);
    }
}

static void handle_pointer_wheel(WidgetsExampleContext& ctx, int wheel_delta) {
    if (wheel_delta == 0) {
        return;
    }
    bool changed = false;
    if (ctx.gallery.layout.list.bounds.contains(ctx.pointer_x, ctx.pointer_y)) {
        float scroll_pixels = static_cast<float>(-wheel_delta) * (ctx.gallery.layout.list.item_height * 0.25f);
        Widgets::ListState desired = ctx.list_state;
        changed |= dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListScroll,
                                 true,
                                 ctx.list_state.hovered_index,
                                 scroll_pixels);
    }
    if (ctx.gallery.layout.tree.bounds.contains(ctx.pointer_x, ctx.pointer_y)) {
        float scroll_pixels = static_cast<float>(-wheel_delta) * (ctx.gallery.layout.tree.row_height * 0.25f);
        Widgets::TreeState desired = ctx.tree_state;
        changed |= dispatch_tree(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::TreeScroll,
                                 true,
                                 ctx.tree_state.hovered_id,
                                 scroll_pixels);
    }
    if (changed) {
        refresh_gallery(ctx);
    }
}

struct ScreenshotScriptState {
    enum class Stage {
        Inactive,
        MoveToPress,
        Press,
        Drag,
        Hold,
        AwaitCapture,
        Release,
        Done,
    };

    Stage stage = Stage::Inactive;
    int frames_in_stage = 0;
    float press_x = 0.0f;
    float press_y = 0.0f;
    float drag_x = 0.0f;
    float drag_y = 0.0f;
    bool capture_ready = false;
    bool capture_completed = false;

    void initialize(GalleryLayout const& layout) {
        auto track = layout.slider_track;
        auto slider = layout.slider;
        float min_x = track.min_x;
        float max_x = track.max_x;
        float min_y = track.min_y;
        float max_y = track.max_y;

        if (max_x <= min_x) {
            min_x = slider.min_x;
            max_x = slider.max_x;
        }
        if (max_y <= min_y) {
            min_y = slider.min_y;
            max_y = slider.max_y;
        }

        float width = std::max(max_x - min_x, 1.0f);
        float height = std::max(max_y - min_y, 1.0f);

        press_x = std::clamp(min_x + width * 0.25f, min_x, max_x);
        drag_x = std::clamp(min_x + width * 0.75f, min_x, max_x);

        press_y = min_y + height * 0.5f;
        drag_y = press_y;

        stage = Stage::MoveToPress;
        frames_in_stage = 0;
        capture_ready = false;
        capture_completed = false;
    }

    void advance(WidgetsExampleContext& ctx) {
        switch (stage) {
        case Stage::Inactive:
        case Stage::Done:
            return;
        case Stage::MoveToPress:
            handle_pointer_move(ctx, press_x, press_y);
            stage = Stage::Press;
            frames_in_stage = 0;
            break;
        case Stage::Press:
            handle_pointer_down(ctx);
            stage = Stage::Drag;
            frames_in_stage = 0;
            break;
        case Stage::Drag:
            handle_pointer_move(ctx, drag_x, drag_y);
            stage = Stage::Hold;
            frames_in_stage = 0;
            break;
        case Stage::Hold:
            if (++frames_in_stage >= 2) {
                capture_ready = true;
                stage = Stage::AwaitCapture;
                frames_in_stage = 0;
            }
            break;
        case Stage::AwaitCapture:
            if (capture_completed) {
                stage = Stage::Release;
            }
            break;
        case Stage::Release:
            handle_pointer_up(ctx);
            stage = Stage::Done;
            frames_in_stage = 0;
            break;
        }
    }

    void mark_capture_complete() {
        capture_completed = true;
        capture_ready = false;
        if (stage == Stage::AwaitCapture) {
            stage = Stage::Release;
        }
    }

    [[nodiscard]] auto wants_capture() const -> bool {
        return capture_ready && !capture_completed;
    }

    [[nodiscard]] auto active() const -> bool {
        return stage != Stage::Inactive && stage != Stage::Done;
    }
};

static void process_widget_actions(WidgetsExampleContext& ctx) {
    auto process_root = [&](WidgetPath const& root) {
        auto queue = WidgetReducers::WidgetOpsQueue(root);
        auto reduced = WidgetReducers::ReducePending(*ctx.space,
                                                    ConcretePathStringView{queue.getPath()});
        if (!reduced) {
            std::cerr << "widgets_example: failed to reduce widget ops for " << root.getPath()
                      << ": " << reduced.error().message.value_or("unknown error") << "\n";
            return;
        }
        if (reduced->empty()) {
            return;
        }
        auto actions_queue = WidgetReducers::DefaultActionsQueue(root);
        auto span = std::span<const WidgetReducers::WidgetAction>(reduced->data(), reduced->size());
        auto publish = WidgetReducers::PublishActions(*ctx.space,
                                                      ConcretePathStringView{actions_queue.getPath()},
                                                      span);
        if (!publish) {
            std::cerr << "widgets_example: failed to publish widget actions for " << root.getPath()
                      << ": " << publish.error().message.value_or("unknown error") << "\n";
            return;
        }
        while (true) {
            auto action = ctx.space->take<WidgetReducers::WidgetAction, std::string>(actions_queue.getPath());
            if (!action) {
                break;
            }
            std::cout << "[widgets_example] action widget=" << action->widget_path
                      << " kind=" << static_cast<int>(action->kind)
                      << " value=" << action->analog_value << std::endl;
        }
    };

    process_root(ctx.button_paths.root);
   process_root(ctx.toggle_paths.root);
    process_root(ctx.slider_paths.root);
    process_root(ctx.list_paths.root);
    process_root(ctx.stack_paths.root);
    process_root(ctx.tree_paths.root);
}

static auto slider_pointer_for_value(WidgetsExampleContext const& ctx, float value) -> std::pair<float, float> {
    auto const range_min = ctx.slider_range.minimum;
    auto const range_max = ctx.slider_range.maximum;
    float clamped = std::clamp(value, range_min, range_max);
    float denom = std::max(range_max - range_min, 1e-6f);
    float progress = std::clamp((clamped - range_min) / denom, 0.0f, 1.0f);

    auto const& bounds = ctx.gallery.layout.slider;
    float x = bounds.min_x + bounds.width() * progress;
    float y = bounds.min_y + ctx.slider_style.height * 0.5f;
    return {x, y};
}

static void capture_headless_screenshot(PathSpace& space,
                                        WidgetsExampleContext& ctx,
                                        Builders::App::BootstrapResult const& bootstrap,
                                        std::filesystem::path const& output_path) {
    auto present = unwrap_or_exit(Builders::Window::Present(space,
                                                            bootstrap.window,
                                                            bootstrap.view_name),
                                  "present gallery window");
    if (!present.stats.presented && present.stats.skipped) {
        std::cerr << "widgets_example: present skipped while capturing screenshot\n";
    }

    auto framebuffer = unwrap_or_exit(
        Diagnostics::ReadSoftwareFramebuffer(space,
                                             SP::ConcretePathStringView{ctx.target_path}),
        "read software framebuffer");

    write_frame_capture_png_or_exit(framebuffer, output_path);
    std::cout << "widgets_example saved screenshot to '" << output_path.string() << "'\n";
}

static void handle_local_mouse(SP::UI::LocalMouseEvent const& ev, void* user_data) {
    auto* ctx = static_cast<WidgetsExampleContext*>(user_data);
    if (!ctx) {
        return;
    }
    widget_trace().record_mouse(ev);
    switch (ev.type) {
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        if (ev.x >= 0 && ev.y >= 0) {
            handle_pointer_move(*ctx, static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        break;
    case SP::UI::LocalMouseEventType::Move:
        handle_pointer_move(*ctx,
                            ctx->pointer_x + static_cast<float>(ev.dx),
                            ctx->pointer_y + static_cast<float>(ev.dy));
        break;
    case SP::UI::LocalMouseEventType::ButtonDown:
        if (ev.x >= 0 && ev.y >= 0) {
            handle_pointer_move(*ctx, static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            handle_pointer_down(*ctx);
        }
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        if (ev.x >= 0 && ev.y >= 0) {
            handle_pointer_move(*ctx, static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            handle_pointer_up(*ctx);
        }
        break;
    case SP::UI::LocalMouseEventType::Wheel:
        handle_pointer_wheel(*ctx, ev.wheel);
        break;
    }
}

static void clear_local_mouse(void* user_data) {
    auto* ctx = static_cast<WidgetsExampleContext*>(user_data);
    if (!ctx) {
        return;
    }
    ctx->pointer_down = false;
    ctx->slider_dragging = false;
}

constexpr char kSimulatedPointerQueue[] = "/system/devices/in/pointer/default/events";

static auto to_local_button(SP::MouseButton button) -> SP::UI::LocalMouseButton {
    switch (button) {
    case SP::MouseButton::Left:
        return SP::UI::LocalMouseButton::Left;
    case SP::MouseButton::Right:
        return SP::UI::LocalMouseButton::Right;
    case SP::MouseButton::Middle:
        return SP::UI::LocalMouseButton::Middle;
    case SP::MouseButton::Button4:
        return SP::UI::LocalMouseButton::Button4;
    case SP::MouseButton::Button5:
        return SP::UI::LocalMouseButton::Button5;
    }
    return SP::UI::LocalMouseButton::Left;
}

static auto make_local_mouse_event(SP::PathIOMouse::Event const& event) -> SP::UI::LocalMouseEvent {
    SP::UI::LocalMouseEvent local{};
    switch (event.type) {
    case SP::MouseEventType::Move:
        local.type = SP::UI::LocalMouseEventType::Move;
        local.dx = event.dx;
        local.dy = event.dy;
        break;
    case SP::MouseEventType::AbsoluteMove:
        local.type = SP::UI::LocalMouseEventType::AbsoluteMove;
        local.x = event.x;
        local.y = event.y;
        break;
    case SP::MouseEventType::ButtonDown:
        local.type = SP::UI::LocalMouseEventType::ButtonDown;
        local.button = to_local_button(event.button);
        local.x = event.x;
        local.y = event.y;
        break;
    case SP::MouseEventType::ButtonUp:
        local.type = SP::UI::LocalMouseEventType::ButtonUp;
        local.button = to_local_button(event.button);
        local.x = event.x;
        local.y = event.y;
        break;
    case SP::MouseEventType::Wheel:
        local.type = SP::UI::LocalMouseEventType::Wheel;
        local.wheel = event.wheel;
        break;
    }
    return local;
}

static void dispatch_simulated_mouse_event(WidgetsExampleContext& ctx,
                                           SP::PathIOMouse::Event const& event) {
    if (!ctx.space) {
        return;
    }

    auto inserted = ctx.space->insert(kSimulatedPointerQueue, event);
    if (!inserted.errors.empty()) {
        auto const& err = inserted.errors.front();
        std::cerr << "widgets_example: failed to insert simulated mouse event: "
                  << err.message.value_or("unknown error") << "\n";
    }

    auto local = make_local_mouse_event(event);
    handle_local_mouse(local, &ctx);

    auto consumed = ctx.space->take<SP::PathIOMouse::Event, std::string>(kSimulatedPointerQueue);
    if (!consumed && consumed.error().code != SP::Error::Code::NoObjectFound
        && consumed.error().code != SP::Error::Code::NoSuchPath) {
        std::cerr << "widgets_example: failed to consume simulated mouse event: "
                  << consumed.error().message.value_or("unknown error") << "\n";
    }
}

static void simulate_slider_drag_for_screenshot(WidgetsExampleContext& ctx, float target_value) {
    if (!ctx.space) {
        return;
    }

    float min_value = ctx.slider_range.minimum;
    float max_value = ctx.slider_range.maximum;
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    float clamped_target = std::clamp(target_value, min_value, max_value);

    auto const start_value = ctx.slider_state.value;
    auto [start_x, start_y] = slider_pointer_for_value(ctx, start_value);
    auto [target_x, target_y] = slider_pointer_for_value(ctx, clamped_target);

    auto send_absolute_move = [&](float x, float y) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::AbsoluteMove;
        ev.x = static_cast<int>(std::lround(x));
        ev.y = static_cast<int>(std::lround(y));
        dispatch_simulated_mouse_event(ctx, ev);
        process_widget_actions(ctx);
    };

    auto send_button_down = [&](float x, float y) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonDown;
        ev.button = SP::MouseButton::Left;
        ev.x = static_cast<int>(std::lround(x));
        ev.y = static_cast<int>(std::lround(y));
        dispatch_simulated_mouse_event(ctx, ev);
        process_widget_actions(ctx);
    };

    send_absolute_move(start_x, start_y);
    send_button_down(start_x, start_y);
    send_absolute_move(target_x, target_y);

    set_focus_target(ctx, FocusTarget::Slider);
    ctx.slider_dragging = true;
}

static bool has_modifier(unsigned int modifiers, unsigned int mask) {
    return (modifiers & mask) != 0U;
}

static void adjust_slider_value(WidgetsExampleContext& ctx, float delta) {
    if (delta == 0.0f) {
        return;
    }
    if (ctx.slider_range.maximum <= ctx.slider_range.minimum) {
        return;
    }
    Widgets::SliderState desired = ctx.slider_state;
    desired.hovered = true;
    desired.value = std::clamp(ctx.slider_state.value + delta,
                               ctx.slider_range.minimum,
                               ctx.slider_range.maximum);
    if (std::abs(desired.value - ctx.slider_state.value) <= 1e-6f) {
        return;
    }
    auto thumb = slider_thumb_position(ctx, desired.value);
    PointerOverride pointer_override(ctx, thumb.first, thumb.second);
    bool changed = false;
    changed |= dispatch_slider(ctx, desired, WidgetBindings::WidgetOpKind::SliderUpdate, true);
    changed |= dispatch_slider(ctx, desired, WidgetBindings::WidgetOpKind::SliderCommit, true);
    if (changed) {
        refresh_gallery(ctx);
    } else {
        ctx.slider_state = desired;
    }
}

static void move_list_focus(WidgetsExampleContext& ctx, int direction) {
    if (ctx.gallery.layout.list.item_bounds.empty()) {
        return;
    }
    int max_index = static_cast<int>(ctx.gallery.layout.list.item_bounds.size()) - 1;
    if (ctx.focus_list_index < 0) {
        ctx.focus_list_index = ctx.list_state.selected_index >= 0 ? ctx.list_state.selected_index : 0;
    }
    ctx.focus_list_index = std::clamp(ctx.focus_list_index + direction, 0, max_index);
    Widgets::ListState desired = ctx.list_state;
    desired.hovered_index = ctx.focus_list_index;
    desired.selected_index = ctx.focus_list_index;
    auto center = list_item_center(ctx, ctx.focus_list_index);
    PointerOverride pointer_override(ctx, center.first, center.second);
    if (dispatch_list(ctx,
                      desired,
                      WidgetBindings::WidgetOpKind::ListSelect,
                      true,
                      ctx.focus_list_index,
                      0.0f)) {
        refresh_gallery(ctx);
    } else {
        ctx.list_state = desired;
    }
}

static bool ensure_tree_focus_index(WidgetsExampleContext& ctx) {
    auto const& rows = ctx.gallery.layout.tree.rows;
    if (rows.empty()) {
        return false;
    }
    if (ctx.focus_tree_index < 0 || ctx.focus_tree_index >= static_cast<int>(rows.size())) {
        if (!ctx.tree_state.selected_id.empty()) {
            auto it = std::find_if(rows.begin(), rows.end(), [&](TreeRowLayout const& row) {
                return row.node_id == ctx.tree_state.selected_id;
            });
            if (it != rows.end()) {
                ctx.focus_tree_index = static_cast<int>(std::distance(rows.begin(), it));
            } else {
                ctx.focus_tree_index = 0;
            }
        } else {
            ctx.focus_tree_index = 0;
        }
    }
    ctx.focus_tree_index = std::clamp(ctx.focus_tree_index, 0, static_cast<int>(rows.size()) - 1);
    return true;
}

static void move_tree_focus(WidgetsExampleContext& ctx, int direction) {
    if (!ensure_tree_focus_index(ctx)) {
        return;
    }
    auto const& rows = ctx.gallery.layout.tree.rows;
    ctx.focus_tree_index = std::clamp(ctx.focus_tree_index + direction,
                                      0,
                                      static_cast<int>(rows.size()) - 1);
    auto const& row = rows[static_cast<std::size_t>(ctx.focus_tree_index)];
    Widgets::TreeState desired = ctx.tree_state;
    desired.hovered_id = row.node_id;
    desired.selected_id = row.node_id;
    auto center = tree_row_center(ctx, ctx.focus_tree_index);
    PointerOverride pointer_override(ctx, center.first, center.second);
    if (dispatch_tree(ctx,
                      desired,
                      WidgetBindings::WidgetOpKind::TreeSelect,
                      true,
                      row.node_id,
                      0.0f)) {
        refresh_gallery(ctx);
    } else {
        ctx.tree_state = desired;
    }
}

static void tree_apply_op(WidgetsExampleContext& ctx, WidgetBindings::WidgetOpKind op) {
    if (!ensure_tree_focus_index(ctx)) {
        return;
    }
    auto const& rows = ctx.gallery.layout.tree.rows;
    auto const& row = rows[static_cast<std::size_t>(ctx.focus_tree_index)];
    if ((op == WidgetBindings::WidgetOpKind::TreeToggle
         || op == WidgetBindings::WidgetOpKind::TreeExpand
         || op == WidgetBindings::WidgetOpKind::TreeCollapse)
        && !row.expandable) {
        return;
    }
    Widgets::TreeState desired = ctx.tree_state;
    desired.hovered_id = row.node_id;
    if (op == WidgetBindings::WidgetOpKind::TreeSelect) {
        desired.selected_id = row.node_id;
    }
    auto center = tree_row_center(ctx, ctx.focus_tree_index);
    PointerOverride pointer_override(ctx, center.first, center.second);
    if (dispatch_tree(ctx,
                      desired,
                      op,
                      true,
                      row.node_id,
                      0.0f)) {
        refresh_gallery(ctx);
    } else {
        ctx.tree_state = desired;
    }
}

static void refresh_stack_layout(WidgetsExampleContext& ctx) {
    ctx.stack_params = unwrap_or_exit(Widgets::DescribeStack(*ctx.space, ctx.stack_paths),
                                      "describe stack layout");
    ctx.stack_layout = unwrap_or_exit(Widgets::ReadStackLayout(*ctx.space, ctx.stack_paths),
                                      "read stack layout");
}

static void adjust_stack_spacing(WidgetsExampleContext& ctx, float delta) {
    float previous = ctx.stack_params.style.spacing;
    float spacing = previous + delta;
    ctx.stack_params.style.spacing = std::max(spacing, 0.0f);
    auto updated = WidgetBindings::UpdateStack(*ctx.space, ctx.stack_binding, ctx.stack_params);
    if (!updated) {
        ctx.stack_params.style.spacing = previous;
        std::cerr << "widgets_example: failed to update stack spacing: "
                  << updated.error().message.value_or("unknown error") << "\n";
        return;
    }
    if (!*updated) {
        ctx.stack_params.style.spacing = previous;
        return;
    }
    refresh_stack_layout(ctx);
    refresh_gallery(ctx);
}

static void toggle_stack_axis(WidgetsExampleContext& ctx) {
    Widgets::StackAxis previous = ctx.stack_params.style.axis;
    ctx.stack_params.style.axis = (ctx.stack_params.style.axis == Widgets::StackAxis::Vertical)
        ? Widgets::StackAxis::Horizontal
        : Widgets::StackAxis::Vertical;
    auto updated = WidgetBindings::UpdateStack(*ctx.space, ctx.stack_binding, ctx.stack_params);
    if (!updated) {
        ctx.stack_params.style.axis = previous;
        std::cerr << "widgets_example: failed to toggle stack axis: "
                  << updated.error().message.value_or("unknown error") << "\n";
        return;
    }
    if (!*updated) {
        ctx.stack_params.style.axis = previous;
        return;
    }
    refresh_stack_layout(ctx);
    refresh_gallery(ctx);
}

static void activate_focused_widget(WidgetsExampleContext& ctx) {
    switch (ctx.focus_target) {
    case FocusTarget::Button: {
        auto desired = ctx.button_state;
        auto center = center_of(ctx.gallery.layout.button);
        PointerOverride pointer_override(ctx, center.first, center.second);
        if (dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::Activate, true)) {
            refresh_gallery(ctx);
        }
        break;
    }
    case FocusTarget::Toggle: {
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.checked = !desired.checked;
        auto center = center_of(ctx.gallery.layout.toggle);
        PointerOverride pointer_override(ctx, center.first, center.second);
        if (dispatch_toggle(ctx, desired, WidgetBindings::WidgetOpKind::Toggle, true)) {
            refresh_gallery(ctx);
        }
        break;
    }
    case FocusTarget::Slider: {
        // Toggle commit to reinforce state; no action when pressing space/enter.
        break;
    }
    case FocusTarget::List: {
        if (ctx.gallery.layout.list.item_bounds.empty()) {
            return;
        }
        int max_index = static_cast<int>(ctx.gallery.layout.list.item_bounds.size()) - 1;
        ctx.focus_list_index = std::clamp(ctx.focus_list_index, 0, max_index);
        Widgets::ListState desired = ctx.list_state;
        desired.hovered_index = ctx.focus_list_index;
        desired.selected_index = ctx.focus_list_index;
        auto center = list_item_center(ctx, ctx.focus_list_index);
        PointerOverride pointer_override(ctx, center.first, center.second);
        if (dispatch_list(ctx,
                          desired,
                          WidgetBindings::WidgetOpKind::ListActivate,
                          true,
                          ctx.focus_list_index,
                          0.0f)) {
            refresh_gallery(ctx);
        } else {
            ctx.list_state = desired;
        }
        break;
    }
    case FocusTarget::Tree: {
        tree_apply_op(ctx, WidgetBindings::WidgetOpKind::TreeToggle);
        break;
    }
    }
}

static void handle_local_keyboard(SP::UI::LocalKeyEvent const& ev, void* user_data) {
    auto* ctx = static_cast<WidgetsExampleContext*>(user_data);
    if (!ctx) {
        return;
    }
    widget_trace().record_key(ev);
    if (ev.type != SP::UI::LocalKeyEventType::KeyDown) {
        return;
    }

    if (ev.character == '[' || ev.character == '{') {
        adjust_stack_spacing(*ctx, -8.0f);
        return;
    }
    if (ev.character == ']' || ev.character == '}') {
        adjust_stack_spacing(*ctx, 8.0f);
        return;
    }
    if (ev.character == 'h' || ev.character == 'H') {
        toggle_stack_axis(*ctx);
        return;
    }

    switch (ev.keycode) {
    case kKeycodeTab:
        cycle_focus(*ctx, !has_modifier(ev.modifiers, LocalKeyModifierShift));
        break;
    case kKeycodeSpace:
    case kKeycodeReturn:
        activate_focused_widget(*ctx);
        break;
    case kKeycodeLeft:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, -slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, -1);
        } else if (ctx->focus_target == FocusTarget::Tree) {
            if (ensure_tree_focus_index(*ctx)) {
                auto const& rows = ctx->gallery.layout.tree.rows;
                auto const& row = rows[static_cast<std::size_t>(ctx->focus_tree_index)];
                if (row.expandable && row.expanded) {
                    tree_apply_op(*ctx, WidgetBindings::WidgetOpKind::TreeCollapse);
                } else {
                    int parent = tree_parent_index(*ctx, ctx->focus_tree_index);
                    if (parent >= 0) {
                        ctx->focus_tree_index = parent;
                        move_tree_focus(*ctx, 0);
                    }
                }
            }
        } else {
            cycle_focus(*ctx, false);
        }
        break;
    case kKeycodeUp:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, -1);
        } else if (ctx->focus_target == FocusTarget::Tree) {
            move_tree_focus(*ctx, -1);
        } else {
            cycle_focus(*ctx, false);
        }
        break;
    case kKeycodeRight:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, 1);
        } else if (ctx->focus_target == FocusTarget::Tree) {
            if (ensure_tree_focus_index(*ctx)) {
                auto const& rows = ctx->gallery.layout.tree.rows;
                auto const& row = rows[static_cast<std::size_t>(ctx->focus_tree_index)];
                if (row.expandable && !row.expanded) {
                    tree_apply_op(*ctx, WidgetBindings::WidgetOpKind::TreeExpand);
                } else {
                    int child = tree_first_child_index(*ctx, ctx->focus_tree_index);
                    if (child >= 0) {
                        ctx->focus_tree_index = child;
                        move_tree_focus(*ctx, 0);
                    }
                }
            }
        } else {
            cycle_focus(*ctx, true);
        }
        break;
    case kKeycodeDown:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, -slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, 1);
        } else if (ctx->focus_target == FocusTarget::Tree) {
            move_tree_focus(*ctx, 1);
        } else {
            cycle_focus(*ctx, true);
        }
        break;
    default:
        break;
    }
}

static void apply_trace_event(WidgetsExampleContext& ctx, TraceEvent const& event) {
    switch (event.kind) {
    case TraceEventKind::MouseAbsolute:
        handle_pointer_move(ctx,
                            static_cast<float>(event.x),
                            static_cast<float>(event.y));
        break;
    case TraceEventKind::MouseRelative:
        handle_pointer_move(ctx,
                            ctx.pointer_x + static_cast<float>(event.dx),
                            ctx.pointer_y + static_cast<float>(event.dy));
        break;
    case TraceEventKind::MouseDown:
        if (event.x >= 0 && event.y >= 0) {
            handle_pointer_move(ctx,
                                static_cast<float>(event.x),
                                static_cast<float>(event.y));
        }
        handle_pointer_down(ctx);
        break;
    case TraceEventKind::MouseUp:
        if (event.x >= 0 && event.y >= 0) {
            handle_pointer_move(ctx,
                                static_cast<float>(event.x),
                                static_cast<float>(event.y));
        }
        handle_pointer_up(ctx);
        break;
    case TraceEventKind::MouseWheel:
        handle_pointer_wheel(ctx, event.wheel);
        break;
    case TraceEventKind::KeyDown: {
        SP::UI::LocalKeyEvent key{};
        key.type = SP::UI::LocalKeyEventType::KeyDown;
        key.keycode = event.keycode;
        key.modifiers = event.modifiers;
        key.character = event.character;
        key.repeat = event.repeat;
        handle_local_keyboard(key, &ctx);
        break;
    }
    case TraceEventKind::KeyUp: {
        SP::UI::LocalKeyEvent key{};
        key.type = SP::UI::LocalKeyEventType::KeyUp;
        key.keycode = event.keycode;
        key.modifiers = event.modifiers;
        key.character = event.character;
        key.repeat = event.repeat;
        handle_local_keyboard(key, &ctx);
        break;
    }
    }
}

static void run_replay_session(WidgetsExampleContext& ctx,
                               std::vector<TraceEvent> const& events) {
    std::cout << "widgets_example: replaying " << events.size() << " recorded events\n";
    for (auto const& event : events) {
        apply_trace_event(ctx, event);
        process_widget_actions(ctx);
    }
    refresh_gallery(ctx);
    std::cout << "widgets_example: replay complete\n";
}

struct PresentTelemetry {
    bool presented = false;
    bool skipped = false;
    bool used_iosurface = false;
    std::size_t framebuffer_bytes = 0;
    std::size_t stride_bytes = 0;
    double render_ms = 0.0;
    double present_ms = 0.0;
    std::uint64_t frame_index = 0;
};

auto present_frame(PathSpace& space,
                   WindowPath const& windowPath,
                   std::string_view viewName,
                   int width,
                   int height) -> std::optional<PresentTelemetry> {
    auto present_result = Window::Present(space, windowPath, viewName);
    if (!present_result) {
        std::cerr << "present failed";
        if (present_result.error().message) {
            std::cerr << ": " << *present_result.error().message;
        }
        std::cerr << std::endl;
        return std::nullopt;
    }

    PresentTelemetry telemetry{};
    telemetry.skipped = present_result->stats.skipped;
    telemetry.render_ms = present_result->stats.frame.render_ms;
    telemetry.present_ms = present_result->stats.present_ms;
    telemetry.frame_index = present_result->stats.frame.frame_index;
    auto dispatched = Builders::App::PresentToLocalWindow(*present_result,
                                                          width,
                                                          height);
    telemetry.presented = dispatched.presented;
    telemetry.used_iosurface = dispatched.used_iosurface;
    telemetry.framebuffer_bytes = dispatched.framebuffer_bytes;
    telemetry.stride_bytes = dispatched.row_stride_bytes;

    return telemetry;
}

} // namespace

int main(int argc, char** argv) {
    using namespace SP;
    using namespace SP::UI::Builders;
    PathSpace space;
    AppRootPath appRoot{"/system/applications/widgets_example"};
    AppRootPathView appRootView{appRoot.getPath()};

    auto options = parse_command_line(argc, argv);
    if (!options) {
        return 1;
    }

    if (options->show_help) {
        print_usage();
        return 0;
    }

    set_debug_capture_enabled(options->debug_capture);

    auto screenshot_path = std::move(options->screenshot_path);
    auto theme_name = std::move(options->theme_name);

    auto theme_selection = Widgets::SetTheme(theme_name);
    if (theme_name && !theme_selection.recognized) {
        std::cerr << "widgets_example: unknown theme '" << *theme_name
                  << "', falling back to " << theme_selection.canonical_name << "\n";
    }
    auto theme = std::move(theme_selection.theme);

    auto& trace = widget_trace();
    trace.init_from_env();
    if (trace.replaying() && !trace.replay_path().empty()) {
        std::cout << "widgets_example: replay trace '" << trace.replay_path() << "'\n";
    } else if (trace.recording() && !trace.record_path().empty()) {
        std::cout << "widgets_example: tracing pointer/key events to '" << trace.record_path() << "'\n";
    }

    Widgets::ButtonParams button_params{};
    button_params.name = "primary_button";
    button_params.label = "Primary";
    Widgets::ApplyTheme(theme, button_params);
    button_params.style.width = 180.0f;
    button_params.style.height = 44.0f;
    auto button = unwrap_or_exit(Widgets::CreateButton(space, appRootView, button_params),
                                 "create button widget");

    auto button_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, button.scene),
                                          "read button revision");
    std::cout << "widgets_example published button widget:\n"
              << "  scene: " << button.scene.getPath() << " (revision "
              << button_revision.revision << ")\n"
              << "  state path: " << button.state.getPath() << "\n"
              << "  label path: " << button.label.getPath() << "\n";

    Widgets::ToggleParams toggle_params{};
    toggle_params.name = "primary_toggle";
    Widgets::ApplyTheme(theme, toggle_params);
    toggle_params.style.width = 60.0f;
    toggle_params.style.height = 32.0f;
    auto toggle = unwrap_or_exit(Widgets::CreateToggle(space, appRootView, toggle_params),
                                 "create toggle widget");

    Widgets::ToggleState toggle_state{};
    toggle_state.checked = true;
    unwrap_or_exit(Widgets::UpdateToggleState(space, toggle, toggle_state),
                   "update toggle state");

    auto toggle_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, toggle.scene),
                                          "read toggle revision");
    std::cout << "widgets_example published toggle widget:\n"
              << "  scene: " << toggle.scene.getPath() << " (revision "
              << toggle_revision.revision << ")\n"
              << "  state path: " << toggle.state.getPath() << "\n";

    Widgets::SliderParams slider_params{};
    slider_params.name = "volume_slider";
    slider_params.minimum = 0.0f;
    slider_params.maximum = 100.0f;
    slider_params.value = 25.0f;
    slider_params.step = 5.0f;
    Widgets::ApplyTheme(theme, slider_params);
    auto slider = unwrap_or_exit(Widgets::CreateSlider(space, appRootView, slider_params),
                                 "create slider widget");

    Widgets::SliderState slider_state{};
    slider_state.value = 45.0f;
    unwrap_or_exit(Widgets::UpdateSliderState(space, slider, slider_state),
                   "update slider state");

    auto slider_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, slider.scene),
                                          "read slider revision");
    std::cout << "widgets_example published slider widget:\n"
              << "  scene: " << slider.scene.getPath() << " (revision "
              << slider_revision.revision << ")\n"
              << "  state path: " << slider.state.getPath() << "\n"
              << "  range path: " << slider.range.getPath() << "\n";

    Widgets::ListParams list_params{};
    list_params.name = "inventory_list";
    list_params.items = {
        Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
        Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
        Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = true},
    };
    Widgets::ApplyTheme(theme, list_params);
    list_params.style.width = 240.0f;
    list_params.style.item_height = 36.0f;
    auto list = unwrap_or_exit(Widgets::CreateList(space, appRootView, list_params),
                               "create list widget");

    Widgets::ListState list_state{};
    list_state.selected_index = 1;
    unwrap_or_exit(Widgets::UpdateListState(space, list, list_state),
                   "update list state");

    auto list_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, list.scene),
                                        "read list revision");
    std::cout << "widgets_example published list widget:\n"
              << "  scene: " << list.scene.getPath() << " (revision "
              << list_revision.revision << ")\n"
              << "  state path: " << list.state.getPath() << "\n"
              << "  items path: " << list.items.getPath() << "\n";

    Widgets::TreeParams tree_params{};
    tree_params.name = "workspace_tree";
    tree_params.nodes = {
        Widgets::TreeNode{.id = "workspace", .parent_id = "", .label = "workspace/", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "docs", .parent_id = "workspace", .label = "docs/", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "src", .parent_id = "workspace", .label = "src/", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "src_builders", .parent_id = "src", .label = "ui/builders.cpp", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "src_renderer", .parent_id = "src", .label = "ui/renderer.cpp", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "tests", .parent_id = "workspace", .label = "tests/", .enabled = true, .expandable = false, .loaded = false},
    };
    Widgets::ApplyTheme(theme, tree_params);
    auto tree = unwrap_or_exit(Widgets::CreateTree(space, appRootView, tree_params),
                               "create tree widget");

    Widgets::TreeState tree_state{};
    tree_state.enabled = true;
    tree_state.selected_id = "workspace";
    tree_state.expanded_ids = {"workspace", "src"};
    unwrap_or_exit(Widgets::UpdateTreeState(space, tree, tree_state),
                   "initialize tree state");

    auto tree_state_live = unwrap_or_exit(space.read<Widgets::TreeState, std::string>(std::string(tree.state.getPath())),
                                          "read tree state");
    auto tree_style_live = unwrap_or_exit(space.read<Widgets::TreeStyle, std::string>(std::string(tree.root.getPath()) + "/meta/style"),
                                          "read tree style");
    auto tree_nodes_live = unwrap_or_exit(space.read<std::vector<Widgets::TreeNode>, std::string>(std::string(tree.nodes.getPath())),
                                          "read tree nodes");

    Widgets::StackLayoutParams stack_params{};
    stack_params.name = "widget_stack";
    stack_params.style.axis = Widgets::StackAxis::Vertical;
    stack_params.style.spacing = 24.0f;
    stack_params.style.padding_main_start = 16.0f;
    stack_params.style.padding_main_end = 16.0f;
    stack_params.style.padding_cross_start = 20.0f;
    stack_params.style.padding_cross_end = 20.0f;
    stack_params.children = {
        Widgets::StackChildSpec{.id = "stack_button", .widget_path = button.root.getPath(), .scene_path = button.scene.getPath()},
        Widgets::StackChildSpec{.id = "stack_toggle", .widget_path = toggle.root.getPath(), .scene_path = toggle.scene.getPath()},
        Widgets::StackChildSpec{.id = "stack_slider", .widget_path = slider.root.getPath(), .scene_path = slider.scene.getPath()},
    };

    auto stack = unwrap_or_exit(Widgets::CreateStack(space, appRootView, stack_params),
                                "create stack layout");
    auto stack_desc = unwrap_or_exit(Widgets::DescribeStack(space, stack),
                                     "describe stack layout");
    auto stack_layout_live = unwrap_or_exit(Widgets::ReadStackLayout(space, stack),
                                            "read stack layout");

    std::cout << "widgets_example published tree widget:\n"
              << "  scene: " << tree.scene.getPath() << "\n"
              << "  state path: " << tree.state.getPath() << "\n"
              << "  nodes path: " << tree.nodes.getPath() << "\n";

    std::cout << "widgets_example published stack layout:\n"
              << "  scene: " << stack.scene.getPath() << "\n"
              << "  style path: " << stack.style.getPath() << "\n";

    bool headless = false;
    if (const char* headless_env = std::getenv("WIDGETS_EXAMPLE_HEADLESS")) {
        if (headless_env[0] != '\0' && headless_env[0] != '0') {
            headless = true;
        }
    }

    if (screenshot_path) {
        headless = false;
    }

    auto slider_range_live = unwrap_or_exit(space.read<Widgets::SliderRange, std::string>(std::string(slider.range.getPath())),
                                            "read slider range");
    auto button_state_live = unwrap_or_exit(space.read<Widgets::ButtonState, std::string>(std::string(button.state.getPath())),
                                            "read button state");
    auto toggle_state_live = unwrap_or_exit(space.read<Widgets::ToggleState, std::string>(std::string(toggle.state.getPath())),
                                            "read toggle state");
    auto slider_state_live = unwrap_or_exit(space.read<Widgets::SliderState, std::string>(std::string(slider.state.getPath())),
                                            "read slider state");
    auto list_state_live = unwrap_or_exit(space.read<Widgets::ListState, std::string>(std::string(list.state.getPath())),
                                          "read list state");
    auto list_style_live = unwrap_or_exit(space.read<Widgets::ListStyle, std::string>(std::string(list.root.getPath()) + "/meta/style"),
                                          "read list style");
    auto list_items_live = unwrap_or_exit(space.read<std::vector<Widgets::ListItem>, std::string>(std::string(list.items.getPath())),
                                          "read list items");

    auto gallery = publish_gallery_scene(space,
                                         appRootView,
                                         button,
                                         button_params.style,
                                         button_state_live,
                                         button_params.label,
                                         toggle,
                                         toggle_params.style,
                                         toggle_state_live,
                                         slider,
                                         slider_params.style,
                                         slider_state_live,
                                         slider_range_live,
                                         list,
                                         list_style_live,
                                         list_state_live,
                                         list_items_live,
                                         stack_desc,
                                         stack_layout_live,
                                         tree,
                                         tree_style_live,
                                         tree_state_live,
                                         tree_nodes_live,
                                         theme);

    std::cout << "widgets_example gallery scene:\n"
              << "  scene: " << gallery.scene.getPath() << "\n"
              << "  size : " << gallery.width << "x" << gallery.height << " pixels\n";

    if (headless && !trace.replaying()) {
        std::cout << "widgets_example exiting headless mode (WIDGETS_EXAMPLE_HEADLESS set).\n";
        trace.flush();
        return 0;
    }

    Builders::App::BootstrapParams bootstrap_params{};
    bootstrap_params.renderer.name = "gallery_renderer";
    bootstrap_params.renderer.kind = RendererKind::Software2D;
    bootstrap_params.renderer.description = "widgets gallery renderer";
    bootstrap_params.surface.name = "gallery_surface";
    bootstrap_params.surface.desc.size_px.width = gallery.width;
    bootstrap_params.surface.desc.size_px.height = gallery.height;
    bootstrap_params.surface.desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    bootstrap_params.surface.desc.color_space = ColorSpace::sRGB;
    bootstrap_params.surface.desc.premultiplied_alpha = true;
    bootstrap_params.window.name = "gallery_window";
    bootstrap_params.window.title = "PathSpace Widgets Gallery";
    bootstrap_params.window.width = gallery.width;
    bootstrap_params.window.height = gallery.height;
    bootstrap_params.window.scale = 1.0f;
    bootstrap_params.window.background = "#1f232b";
    bootstrap_params.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    bootstrap_params.present_policy.vsync_align = false;
    bootstrap_params.present_policy.auto_render_on_present = true;
    bootstrap_params.present_policy.capture_framebuffer = screenshot_path.has_value();
    bootstrap_params.view_name = "main";
    RenderSettings bootstrap_settings{};
    bootstrap_settings.clear_color = {0.11f, 0.12f, 0.15f, 1.0f};
    bootstrap_settings.surface.size_px.width = gallery.width;
    bootstrap_settings.surface.size_px.height = gallery.height;
    bootstrap_settings.surface.dpi_scale = 1.0f;
    bootstrap_params.renderer_settings_override = bootstrap_settings;

    auto bootstrap = unwrap_or_exit(Builders::App::Bootstrap(space,
                                                             appRootView,
                                                             gallery.scene,
                                                             bootstrap_params),
                                    "bootstrap gallery renderer");

    WidgetsExampleContext ctx{};
    ctx.space = &space;
    ctx.app_root = appRoot;
    ctx.button_paths = button;
    ctx.toggle_paths = toggle;
    ctx.slider_paths = slider;
    ctx.list_paths = list;
    ctx.stack_paths = stack;
    ctx.tree_paths = tree;
    ctx.theme = theme;
    ctx.button_style = button_params.style;
    ctx.button_label = button_params.label;
    ctx.toggle_style = toggle_params.style;
    ctx.slider_style = slider_params.style;
    ctx.list_style = list_style_live;
    ctx.slider_range = slider_range_live;
    ctx.list_items = list_items_live;
    ctx.stack_params = stack_desc;
    ctx.stack_layout = stack_layout_live;
    ctx.tree_style = tree_style_live;
    ctx.tree_state = tree_state_live;
    ctx.tree_nodes = tree_nodes_live;
    ctx.button_state = button_state_live;
    ctx.toggle_state = toggle_state_live;
    ctx.slider_state = slider_state_live;
    ctx.list_state = list_state_live;
    ctx.gallery = gallery;
    ctx.target_path = bootstrap.target.getPath();
    ctx.focus_config = WidgetFocus::MakeConfig(appRootView, bootstrap.target);

    auto target_view = SP::ConcretePathStringView{ctx.target_path};

    ctx.button_binding = unwrap_or_exit(WidgetBindings::CreateButtonBinding(space,
                                                                            appRootView,
                                                                            ctx.button_paths,
                                                                            target_view,
                                                                            make_dirty_hint(ctx.gallery.layout.button_footprint)),
                                        "create button binding");

    ctx.toggle_binding = unwrap_or_exit(WidgetBindings::CreateToggleBinding(space,
                                                                            appRootView,
                                                                            ctx.toggle_paths,
                                                                            target_view,
                                                                            make_dirty_hint(ctx.gallery.layout.toggle_footprint)),
                                        "create toggle binding");

    auto slider_dirty_hint = make_dirty_hint(ctx.gallery.layout.slider_footprint);
    ctx.slider_binding = unwrap_or_exit(WidgetBindings::CreateSliderBinding(space,
                                                                            appRootView,
                                                                            ctx.slider_paths,
                                                                            target_view,
                                                                            slider_dirty_hint),
                                        "create slider binding");

    ctx.list_binding = unwrap_or_exit(WidgetBindings::CreateListBinding(space,
                                                                        appRootView,
                                                                        ctx.list_paths,
                                                                        target_view,
                                                                        make_dirty_hint(ctx.gallery.layout.list_footprint)),
                                      "create list binding");

    ctx.stack_binding = unwrap_or_exit(WidgetBindings::CreateStackBinding(space,
                                                                          appRootView,
                                                                          ctx.stack_paths,
                                                                          target_view,
                                                                          make_dirty_hint(ctx.gallery.layout.stack_footprint)),
                                       "create stack binding");

    ctx.tree_binding = unwrap_or_exit(WidgetBindings::CreateTreeBinding(space,
                                                                        appRootView,
                                                                        ctx.tree_paths,
                                                                        target_view,
                                                                        make_dirty_hint(ctx.gallery.layout.tree_footprint)),
                                      "create tree binding");

    if (set_focus_target(ctx, FocusTarget::Button)) {
        refresh_gallery(ctx);
    }

    if (screenshot_path) {
        if (trace.replaying()) {
            run_replay_session(ctx, trace.events());
            auto const output_path = std::filesystem::path{*screenshot_path};
            capture_headless_screenshot(space, ctx, bootstrap, output_path);
            trace.flush();
            return 0;
        }

        auto const range = ctx.slider_range.maximum - ctx.slider_range.minimum;
        float target_value = ctx.slider_state.value + std::max(range * 0.4f, 5.0f);
        target_value = std::min(ctx.slider_range.maximum, target_value);

        simulate_slider_drag_for_screenshot(ctx, target_value);
        auto const output_path = std::filesystem::path{*screenshot_path};
        capture_headless_screenshot(space, ctx, bootstrap, output_path);
        trace.flush();
        return 0;
    }

    ScreenshotScriptState screenshot_script{};
    if (screenshot_path && !trace.replaying()) {
        screenshot_script.initialize(ctx.gallery.layout);
    }

    if (trace.replaying()) {
        run_replay_session(ctx, trace.events());
        trace.flush();
        return 0;
    }

    std::cout << "widgets_example keyboard controls:\n"
              << "  Tab / Shift+Tab : cycle widget focus\n"
              << "  Arrow keys       : adjust slider, move list selection, or navigate the tree\n"
              << "  Space / Return   : activate focused widget (toggle tree expansion)\n"
              << "  [ / ]            : decrease / increase stack spacing\n"
              << "  H                : toggle stack axis (vertical / horizontal)\n";

    SP::UI::SetLocalWindowCallbacks({&handle_local_mouse, &clear_local_mouse, &ctx, &handle_local_keyboard});
    SP::UI::InitLocalWindowWithSize(ctx.gallery.width,
                                    ctx.gallery.height,
                                    "PathSpace Widgets Gallery");

    auto last_report = std::chrono::steady_clock::now();
    std::uint64_t frames_presented = 0;
    double total_render_ms = 0.0;
    double total_present_ms = 0.0;
    int window_width = bootstrap.surface_desc.size_px.width;
    int window_height = bootstrap.surface_desc.size_px.height;
    bool pending_screenshot = screenshot_path.has_value();
    std::uint64_t screenshot_present_count = 0;
    constexpr std::uint64_t kScreenshotWarmupFrames = 2;

    while (true) {
        if (screenshot_script.active()) {
            screenshot_script.advance(ctx);
        }

        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            break;
        }

        int requested_width = window_width;
        int requested_height = window_height;
        SP::UI::GetLocalWindowContentSize(&requested_width, &requested_height);
        if (requested_width <= 0 || requested_height <= 0) {
            break;
        }

        if (requested_width != window_width || requested_height != window_height) {
            window_width = requested_width;
            window_height = requested_height;
            unwrap_or_exit(Builders::App::UpdateSurfaceSize(space,
                                                            bootstrap,
                                                            window_width,
                                                            window_height),
                           "refresh surface after resize");
        }

        auto telemetry = present_frame(space, bootstrap.window, bootstrap.view_name, window_width, window_height);
        if (telemetry && telemetry->presented && !telemetry->skipped) {
            ++frames_presented;
            total_render_ms += telemetry->render_ms;
            total_present_ms += telemetry->present_ms;
            if (pending_screenshot) {
                if (screenshot_script.wants_capture()) {
                    ++screenshot_present_count;
                    if (screenshot_present_count >= kScreenshotWarmupFrames) {
                        std::filesystem::path path = *screenshot_path;
                        std::error_code mkdir_ec;
                        if (path.has_parent_path()) {
                            std::filesystem::create_directories(path.parent_path(), mkdir_ec);
                        }
                        bool saved = SP::UI::SaveLocalWindowScreenshot(path.string().c_str());
                        if (saved) {
                            std::cout << "widgets_example saved screenshot to '" << path.string() << "'\n";
                        } else {
                            std::cerr << "widgets_example: failed to save screenshot to '" << path.string() << "'\n";
                        }
                        pending_screenshot = false;
                        screenshot_present_count = 0;
                        screenshot_script.mark_capture_complete();
                        SP::UI::RequestLocalWindowQuit();
                    }
                } else {
                    screenshot_present_count = 0;
                }
            }
        }

        process_widget_actions(ctx);

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            if (frames_presented > 0) {
                double seconds = std::chrono::duration<double>(now - last_report).count();
                double fps = frames_presented / std::max(seconds, 1e-6);
                double avg_render = total_render_ms / static_cast<double>(frames_presented);
                double avg_present = total_present_ms / static_cast<double>(frames_presented);
                std::cout << "[widgets_example] fps=" << std::fixed << std::setprecision(1) << fps
                          << " render_ms=" << std::setprecision(2) << avg_render
                          << " present_ms=" << avg_present
                          << std::defaultfloat << std::endl;
            }
            frames_presented = 0;
            total_render_ms = 0.0;
            total_present_ms = 0.0;
            last_report = now;
        }

        if (ctx.debug_capture_pending) {
            std::ostringstream base_stream;
            base_stream << "widgets_example_debug_" << std::setw(3) << std::setfill('0')
                        << ctx.debug_capture_index;
            auto base_name = base_stream.str();

            std::filesystem::path txt_path = base_name + ".txt";
            write_debug_dump(ctx, txt_path);

            std::filesystem::path png_path = base_name + ".png";
            bool saved = SP::UI::SaveLocalWindowScreenshot(png_path.string().c_str());
            if (saved) {
                std::cout << "widgets_example saved debug screenshot to '"
                          << png_path.string() << "'\n";
            } else {
                std::cerr << "widgets_example: failed to save debug screenshot to '"
                          << png_path.string() << "'\n";
            }
            ++ctx.debug_capture_index;
            ctx.debug_capture_pending = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    trace.flush();
    return 0;
}

#endif // PATHSPACE_ENABLE_UI
