#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/BuildersDetail.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/WidgetTrace.hpp>
#include <pathspace/ui/FontManager.hpp>
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
namespace WidgetInput = SP::UI::Builders::Widgets::Input;
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

constexpr std::string_view kControlsStackChildButton = "controls_button";
constexpr std::string_view kControlsStackChildToggle = "controls_toggle";
constexpr std::string_view kGalleryStackChildControls = "gallery_controls";
constexpr std::string_view kGalleryStackChildSlider = "gallery_slider";
constexpr std::string_view kGalleryStackChildList = "gallery_list";
constexpr std::string_view kGalleryStackChildStack = "gallery_stack_preview";
constexpr std::string_view kGalleryStackChildTree = "gallery_tree";

static bool g_debug_capture_enabled = [] {
    const char* env = std::getenv("WIDGETS_EXAMPLE_DEBUG_CAPTURE");
    return env && env[0] != '\0' && env[0] != '0';
}();

template <typename T>
auto unwrap_or_exit(SP::Expected<T> value, std::string const& context) -> T;

auto unwrap_or_exit(SP::Expected<void> value, std::string const& context) -> void;

struct DemoFontConfig {
    std::string family;
    std::string style;
    std::string weight;
    std::vector<std::string> fallback;
    std::string manifest_digest;
    std::uint64_t revision = 0;
};

auto make_font_manifest(DemoFontConfig const& config) -> std::string {
    std::ostringstream json;
    json << "{\"family\":\"" << config.family << "\",";
    json << "\"style\":\"" << config.style << "\",";
    json << "\"weight\":\"" << config.weight << "\",";
    json << "\"fallback\":[";
    for (std::size_t index = 0; index < config.fallback.size(); ++index) {
        if (index > 0) {
            json << ',';
        }
        json << '\"' << config.fallback[index] << '\"';
    }
    json << "]}";
    return json.str();
}

auto register_font_or_exit(FontManager& manager,
                            AppRootPathView appRoot,
                            DemoFontConfig const& config)
    -> Builders::Resources::Fonts::FontResourcePaths {
    Builders::Resources::Fonts::RegisterFontParams params{
        .family = config.family,
        .style = config.style,
    };
    params.manifest_json = make_font_manifest(config);
    if (!config.manifest_digest.empty()) {
        params.manifest_digest = config.manifest_digest;
    }
    params.initial_revision = config.revision;
    auto context = std::string{"register font "} + config.family + "/" + config.style;
    return unwrap_or_exit(manager.register_font(appRoot, params), context);
}

void apply_font_to_typography(Widgets::TypographyStyle& typography,
                              DemoFontConfig const& config,
                              Builders::Resources::Fonts::FontResourcePaths const& paths) {
    typography.font_family = config.family;
    typography.font_style = config.style;
    typography.font_weight = config.weight;
    typography.font_resource_root = paths.root.getPath();
    typography.font_active_revision = config.revision;
    typography.fallback_families = config.fallback;
    typography.font_features = {"kern", "liga"};
    typography.font_asset_fingerprint = 0;
}

void attach_demo_fonts(PathSpace& space,
                       AppRootPathView appRoot,
                       Widgets::WidgetTheme& theme) {
    FontManager manager(space);

    DemoFontConfig regular{
        .family = "PathSpaceSans",
        .style = "Regular",
        .weight = "400",
        .fallback = {"system-ui"},
        .manifest_digest = "sha256:pathspacesans-regular",
        .revision = 1ull,
    };

    DemoFontConfig semibold{
        .family = "PathSpaceSans",
        .style = "SemiBold",
        .weight = "600",
        .fallback = {"PathSpaceSans", "system-ui"},
        .manifest_digest = "sha256:pathspacesans-semibold",
        .revision = 2ull,
    };

    auto regular_paths = register_font_or_exit(manager, appRoot, regular);
    auto semibold_paths = register_font_or_exit(manager, appRoot, semibold);

    auto apply_regular = [&](Widgets::TypographyStyle& typography) {
        apply_font_to_typography(typography, regular, regular_paths);
    };
    auto apply_semibold = [&](Widgets::TypographyStyle& typography) {
        apply_font_to_typography(typography, semibold, semibold_paths);
    };

    apply_semibold(theme.heading);
    apply_regular(theme.caption);
    apply_semibold(theme.button.typography);
    apply_regular(theme.slider.label_typography);
    apply_regular(theme.list.item_typography);
    apply_regular(theme.tree.label_typography);
}

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

using WidgetBounds = Widgets::Input::WidgetBounds;
using ListLayout = Widgets::Input::ListLayout;

struct StackPreviewLayout {
    WidgetBounds bounds;
    std::vector<WidgetBounds> child_bounds;
};

using TreeRowLayout = Widgets::Input::TreeRowLayout;
using TreeLayout = Widgets::Input::TreeLayout;

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

auto append_bucket(SceneData::DrawableBucketSnapshot& dest,
                   SceneData::DrawableBucketSnapshot const& src) -> void;

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

    layout_info.bounds = WidgetInput::BoundsFromRect(preview_result.layout.bounds);
    layout_info.content_top = preview_result.layout.content_top;
    layout_info.row_height = preview_result.layout.row_height;
    layout_info.rows.clear();
    layout_info.rows.reserve(preview_result.layout.rows.size());

    auto bucket = std::move(preview_result.bucket);

    for (std::size_t index = 0; index < preview_result.layout.rows.size(); ++index) {
        auto const& row = preview_result.layout.rows[index];
        layout_info.rows.push_back(TreeRowLayout{
            .bounds = WidgetInput::BoundsFromRect(row.row_bounds),
            .node_id = row.id,
            .label = row.label,
            .toggle = WidgetInput::BoundsFromRect(row.toggle_bounds),
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
        auto label = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make(layout_row.label.empty() ? std::string("(node)") : layout_row.label,
                                             style.label_typography)
                .WithOrigin(label_x, baseline)
                .WithColor(text_color)
                .WithDrawable(0x41A30000ull + static_cast<std::uint64_t>(index), std::move(authoring_id), 0.2f));
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

static auto find_stack_child(Widgets::StackLayoutState const& layout,
                             std::string_view id) -> Widgets::StackLayoutComputedChild const* {
    for (auto const& child : layout.children) {
        if (child.id == id) {
            return &child;
        }
    }
    return nullptr;
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
                          Widgets::StackLayoutState const& controls_stack_layout,
                          Widgets::StackLayoutState const& gallery_stack_layout,
                          Widgets::TreePaths const& tree,
                          Widgets::TreeStyle const& tree_style,
                          Widgets::TreeState const& tree_state,
                          std::vector<Widgets::TreeNode> const& tree_nodes,
                          Widgets::WidgetTheme const& theme,
                          std::optional<std::string_view> focused_widget) -> GalleryBuildResult {
    (void)space;
    (void)appRoot;
    (void)focused_widget;

    std::vector<SceneData::DrawableBucketSnapshot> pending;
    pending.reserve(24);

    float max_width = 0.0f;
    float max_height = 0.0f;
    std::uint64_t next_drawable_id = 0xA1000000ull;
    GalleryLayout layout{};

    float title_left = kDefaultMargin;
    float cursor_y = kDefaultMargin;

    Widgets::TypographyStyle heading_typography = theme.heading;
    float heading_line_height = heading_typography.line_height;
    auto title_text = Widgets::BuildLabel(
        Widgets::LabelBuildParams::Make("PathSpace Widgets", heading_typography)
            .WithOrigin(title_left, cursor_y + heading_typography.baseline_shift)
            .WithColor(theme.heading_color)
            .WithDrawable(next_drawable_id++, std::string("widget/gallery/title"), 0.4f));
    if (title_text) {
        pending.emplace_back(std::move(title_text->bucket));
        max_width = std::max(max_width, title_left + title_text->width);
        max_height = std::max(max_height, cursor_y + heading_line_height);
    }
    cursor_y += heading_line_height + 24.0f;

    float content_left = kDefaultMargin;
    float content_top = cursor_y;

    auto update_extents = [&](float x, float y) {
        max_width = std::max(max_width, x);
        max_height = std::max(max_height, y);
    };

    auto make_bounds = [](float left, float top, float width, float height) -> WidgetBounds {
        return WidgetBounds{left, top, left + width, top + height};
    };

    if (auto controls_child = find_stack_child(gallery_stack_layout, kGalleryStackChildControls)) {
        float controls_left = content_left + controls_child->x;
        float controls_top = content_top + controls_child->y;

        if (auto button_child = find_stack_child(controls_stack_layout, kControlsStackChildButton)) {
            float button_left = controls_left + button_child->x;
            float button_top = controls_top + button_child->y;

            auto button_bucket = Widgets::BuildButtonPreview(
                button_style,
                button_state,
                Widgets::ButtonPreviewOptions{
                    .authoring_root = "widget/gallery/button",
                    .pulsing_highlight = true,
                });
            translate_bucket(button_bucket, button_left, button_top);
            pending.emplace_back(std::move(button_bucket));

            layout.button = make_bounds(button_left, button_top, button_style.width, button_style.height);

            float label_width = TextBuilder::MeasureTextWidth(button_label, button_style.typography);
            float label_line_height = button_style.typography.line_height;
            float label_x = button_left + std::max(0.0f, (button_style.width - label_width) * 0.5f);
            float label_top = button_top + std::max(0.0f, (button_style.height - label_line_height) * 0.5f);
            float label_baseline = label_top + button_style.typography.baseline_shift;

            auto label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make(button_label, button_style.typography)
                    .WithOrigin(label_x, label_baseline)
                    .WithColor(button_style.text_color)
                    .WithDrawable(next_drawable_id++, std::string("widget/gallery/button/label"), 0.6f));

            std::optional<WidgetBounds> button_label_bounds;
            if (label) {
                button_label_bounds = Widgets::LabelBounds(*label);
                if (!button_label_bounds) {
                    button_label_bounds = SP::UI::Builders::MakeWidgetBounds(label_x,
                                                                             label_top,
                                                                             label_x + label->width,
                                                                             label_top + label_line_height);
                }
                pending.emplace_back(std::move(label->bucket));
                update_extents(label_x + label->width, label_top + label_line_height);
            }
            layout.button_footprint = layout.button;
            if (button_label_bounds) {
                layout.button_footprint.include(*button_label_bounds);
            }
            layout.button_footprint.normalize();
            WidgetInput::ExpandForFocusHighlight(layout.button_footprint);
            update_extents(layout.button.max_x, layout.button.max_y);
        }

        if (auto toggle_child = find_stack_child(controls_stack_layout, kControlsStackChildToggle)) {
            float toggle_left = controls_left + toggle_child->x;
            float toggle_top = controls_top + toggle_child->y;

            auto toggle_bucket = Widgets::BuildTogglePreview(
                toggle_style,
                toggle_state,
                Widgets::TogglePreviewOptions{
                    .authoring_root = "widget/gallery/toggle",
                    .pulsing_highlight = true,
                });
            translate_bucket(toggle_bucket, toggle_left, toggle_top);
            pending.emplace_back(std::move(toggle_bucket));

            layout.toggle = make_bounds(toggle_left, toggle_top, toggle_style.width, toggle_style.height);

            Widgets::TypographyStyle toggle_label_typography = theme.caption;
            float toggle_label_line = toggle_label_typography.line_height;
            float toggle_label_x = toggle_left + toggle_style.width + 24.0f;
            float toggle_label_top = toggle_top + std::max(0.0f, (toggle_style.height - toggle_label_line) * 0.5f);

            auto toggle_label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make("Toggle", toggle_label_typography)
                    .WithOrigin(toggle_label_x, toggle_label_top + toggle_label_typography.baseline_shift)
                    .WithColor(theme.caption_color)
                    .WithDrawable(next_drawable_id++, std::string("widget/gallery/toggle/label"), 0.6f));
            std::optional<WidgetBounds> toggle_label_bounds;
            if (toggle_label) {
                toggle_label_bounds = Widgets::LabelBounds(*toggle_label);
                if (!toggle_label_bounds) {
                    toggle_label_bounds = SP::UI::Builders::MakeWidgetBounds(toggle_label_x,
                                                                             toggle_label_top,
                                                                             toggle_label_x + toggle_label->width,
                                                                             toggle_label_top + toggle_label_line);
                }
                pending.emplace_back(std::move(toggle_label->bucket));
                update_extents(toggle_label_x + toggle_label->width, toggle_label_top + toggle_label_line);
            }

            layout.toggle_footprint = layout.toggle;
            if (toggle_label_bounds) {
                layout.toggle_footprint.include(*toggle_label_bounds);
            }
            layout.toggle_footprint.normalize();
            WidgetInput::ExpandForFocusHighlight(layout.toggle_footprint);
            update_extents(layout.toggle.max_x, layout.toggle.max_y);
        }
    }

    if (auto slider_child = find_stack_child(gallery_stack_layout, kGalleryStackChildSlider)) {
        float slider_left = content_left + slider_child->x;
        float slider_top = content_top + slider_child->y;

        std::optional<SceneData::DrawableBucketSnapshot> caption_bucket;
        std::string slider_caption = "Volume " + std::to_string(static_cast<int>(std::round(slider_state.value)));
        Widgets::TypographyStyle slider_caption_typography = slider_style.label_typography;
        float slider_caption_line = slider_caption_typography.line_height;
        float caption_top = slider_top - (slider_caption_line + 12.0f);
        if (caption_top + slider_caption_line > slider_top) {
            caption_top = slider_top - slider_caption_line;
        }
        auto caption = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make(slider_caption, slider_caption_typography)
                .WithOrigin(slider_left, caption_top + slider_caption_typography.baseline_shift)
                .WithColor(slider_style.label_color)
                .WithDrawable(next_drawable_id++, std::string("widget/gallery/slider/caption"), 0.6f));
        if (caption) {
            auto caption_bounds = Widgets::LabelBounds(*caption);
            caption_bucket = std::move(caption->bucket);
            if (caption_bounds) {
                layout.slider_caption = *caption_bounds;
            } else {
                layout.slider_caption = SP::UI::Builders::MakeWidgetBounds(slider_left,
                                                                           caption_top,
                                                                           slider_left + caption->width,
                                                                           caption_top + slider_caption_line);
            }
            update_extents(slider_left + caption->width, caption_top + slider_caption_line);
        } else {
            layout.slider_caption.reset();
        }

        auto slider_bucket = Widgets::BuildSliderPreview(
            slider_style,
            slider_range,
            slider_state,
            Widgets::SliderPreviewOptions{
                .authoring_root = "widget/gallery/slider",
                .pulsing_highlight = true,
            });
        translate_bucket(slider_bucket, slider_left, slider_top);
        pending.emplace_back(std::move(slider_bucket));

        layout.slider = make_bounds(slider_left, slider_top, slider_style.width, slider_style.height);
        float slider_center_y = slider_top + slider_style.height * 0.5f;
        float slider_half_track = slider_style.track_height * 0.5f;
        layout.slider_track = WidgetBounds{
            slider_left,
            slider_center_y - slider_half_track,
            slider_left + slider_style.width,
            slider_center_y + slider_half_track,
        };
        layout.slider_footprint = layout.slider;
        layout.slider_footprint.include(layout.slider_track);
        if (layout.slider_caption) {
            layout.slider_footprint.include(*layout.slider_caption);
        }
        layout.slider_footprint.normalize();
        WidgetInput::ExpandForFocusHighlight(layout.slider_footprint);

        if (caption_bucket) {
            pending.emplace_back(std::move(*caption_bucket));
        }

        update_extents(layout.slider.max_x, layout.slider.max_y);
    }

    if (auto list_child = find_stack_child(gallery_stack_layout, kGalleryStackChildList)) {
        float list_left = content_left + list_child->x;
        float list_top = content_top + list_child->y;

        Widgets::TypographyStyle list_caption_typography = theme.caption;
        float list_caption_line = list_caption_typography.line_height;
        float list_caption_top = list_top - (list_caption_line + 12.0f);
        auto list_caption = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make("Inventory", list_caption_typography)
                .WithOrigin(list_left, list_caption_top + list_caption_typography.baseline_shift)
                .WithColor(theme.caption_color)
                .WithDrawable(next_drawable_id++, std::string("widget/gallery/list/caption"), 0.6f));
        if (list_caption) {
            layout.list_caption = Widgets::LabelBounds(*list_caption);
            if (!layout.list_caption) {
                layout.list_caption = SP::UI::Builders::MakeWidgetBounds(list_left,
                                                                         list_caption_top,
                                                                         list_left + list_caption->width,
                                                                         list_caption_top + list_caption_line);
            }
            pending.emplace_back(std::move(list_caption->bucket));
            update_extents(list_left + list_caption->width, list_caption_top + list_caption_line);
        } else {
            layout.list_caption.reset();
        }

        auto list_preview = Widgets::BuildListPreview(
            list_style,
            list_items,
            list_state,
            Widgets::ListPreviewOptions{
                .authoring_root = "widget/gallery/list",
                .label_inset = 16.0f,
                .pulsing_highlight = true,
            });
        translate_bucket(list_preview.bucket, list_left, list_top);
        pending.emplace_back(std::move(list_preview.bucket));

        layout.list.bounds = WidgetBounds{
            list_preview.layout.bounds.min_x + list_left,
            list_preview.layout.bounds.min_y + list_top,
            list_preview.layout.bounds.max_x + list_left,
            list_preview.layout.bounds.max_y + list_top,
        };
        layout.list.item_height = list_preview.layout.item_height;
        layout.list.content_top = list_preview.layout.content_top;
        layout.list.item_bounds.clear();
        layout.list.item_bounds.reserve(list_preview.layout.rows.size());
        for (auto const& row : list_preview.layout.rows) {
            layout.list.item_bounds.push_back(WidgetBounds{
                row.row_bounds.min_x + list_left,
                row.row_bounds.min_y + list_top,
                row.row_bounds.max_x + list_left,
                row.row_bounds.max_y + list_top,
            });
        }
        layout.list_footprint = layout.list.bounds;
        if (layout.list_caption) {
            layout.list_footprint.include(*layout.list_caption);
        }
        layout.list_footprint.normalize();
        WidgetInput::ExpandForFocusHighlight(layout.list_footprint);

        auto const& sanitized_style = list_preview.layout.style;
        std::size_t label_count = std::min(list_preview.layout.rows.size(), list_items.size());
        for (std::size_t index = 0; index < label_count; ++index) {
            auto const& row = list_preview.layout.rows[index];
            auto const& item = list_items[index];
            float label_x = list_left + row.label_bounds.min_x;
            float label_top = list_top + row.label_bounds.min_y;
            float label_baseline = list_top + row.label_baseline;
            auto label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make(item.label, sanitized_style.item_typography)
                    .WithOrigin(label_x, label_baseline)
                    .WithColor(sanitized_style.item_text_color)
                    .WithDrawable(next_drawable_id++, "widget/gallery/list/item/" + row.id, 0.65f));
            if (label) {
                pending.emplace_back(std::move(label->bucket));
                update_extents(label_x + label->width,
                               label_top + sanitized_style.item_typography.line_height);
            }
        }

        update_extents(layout.list.bounds.max_x, layout.list.bounds.max_y);
    }

    if (auto stack_child = find_stack_child(gallery_stack_layout, kGalleryStackChildStack)) {
        float stack_left = content_left + stack_child->x;
        float stack_top = content_top + stack_child->y;

        Widgets::TypographyStyle caption_typography = theme.caption;
        float caption_line = caption_typography.line_height;
        float stack_caption_top = stack_top - (caption_line + 12.0f);
        auto caption = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make("Stack layout preview", caption_typography)
                .WithOrigin(stack_left, stack_caption_top + caption_typography.baseline_shift)
                .WithColor(theme.caption_color)
                .WithDrawable(next_drawable_id++, std::string("widget/gallery/stack/caption"), 0.6f));
        if (caption) {
            layout.stack_caption = Widgets::LabelBounds(*caption);
            if (!layout.stack_caption) {
                layout.stack_caption = SP::UI::Builders::MakeWidgetBounds(stack_left,
                                                                          stack_caption_top,
                                                                          stack_left + caption->width,
                                                                          stack_caption_top + caption_line);
            }
            pending.emplace_back(std::move(caption->bucket));
            update_extents(stack_left + caption->width, stack_caption_top + caption_line);
        } else {
            layout.stack_caption.reset();
        }

        StackPreviewLayout stack_preview{};
        auto stack_bucket = build_stack_preview(stack_params.style, stack_layout, theme, stack_preview);
        translate_bucket(stack_bucket, stack_left, stack_top);
        pending.emplace_back(std::move(stack_bucket));

        layout.stack.bounds = WidgetBounds{
            stack_preview.bounds.min_x + stack_left,
            stack_preview.bounds.min_y + stack_top,
            stack_preview.bounds.max_x + stack_left,
            stack_preview.bounds.max_y + stack_top,
        };
        layout.stack.child_bounds.clear();
        layout.stack.child_bounds.reserve(stack_preview.child_bounds.size());
        for (auto const& child : stack_preview.child_bounds) {
            layout.stack.child_bounds.push_back(WidgetBounds{
                child.min_x + stack_left,
                child.min_y + stack_top,
                child.max_x + stack_left,
                child.max_y + stack_top,
            });
        }

        layout.stack_footprint = layout.stack.bounds;
        if (layout.stack_caption) {
            layout.stack_footprint.include(*layout.stack_caption);
        }
        layout.stack_footprint.normalize();
        WidgetInput::ExpandForFocusHighlight(layout.stack_footprint);
        update_extents(layout.stack.bounds.max_x, layout.stack.bounds.max_y);
    }

    if (auto tree_child = find_stack_child(gallery_stack_layout, kGalleryStackChildTree)) {
        float tree_left = content_left + tree_child->x;
        float tree_top = content_top + tree_child->y;

        Widgets::TypographyStyle caption_typography = theme.caption;
        float caption_line = caption_typography.line_height;
        float tree_caption_top = tree_top - (caption_line + 12.0f);
        auto caption = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make("Tree view preview", caption_typography)
                .WithOrigin(tree_left, tree_caption_top + caption_typography.baseline_shift)
                .WithColor(theme.caption_color)
                .WithDrawable(next_drawable_id++, std::string("widget/gallery/tree/caption"), 0.6f));
        if (caption) {
            layout.tree_caption = Widgets::LabelBounds(*caption);
            if (!layout.tree_caption) {
                layout.tree_caption = SP::UI::Builders::MakeWidgetBounds(tree_left,
                                                                         tree_caption_top,
                                                                         tree_left + caption->width,
                                                                         tree_caption_top + caption_line);
            }
            pending.emplace_back(std::move(caption->bucket));
            update_extents(tree_left + caption->width, tree_caption_top + caption_line);
        } else {
            layout.tree_caption.reset();
        }

        TreeLayout tree_preview{};
        auto tree_bucket = build_tree_preview(tree_style, tree_nodes, tree_state, theme, tree_preview);
        translate_bucket(tree_bucket, tree_left, tree_top);
        pending.emplace_back(std::move(tree_bucket));

        layout.tree = tree_preview;
        WidgetInput::TranslateTreeLayout(layout.tree, tree_left, tree_top);

        layout.tree_footprint = layout.tree.bounds;
        if (layout.tree_caption) {
            layout.tree_footprint.include(*layout.tree_caption);
        }
        layout.tree_footprint.normalize();
        WidgetInput::ExpandForFocusHighlight(layout.tree_footprint);
        update_extents(layout.tree.bounds.max_x, layout.tree.bounds.max_y);
    }

    float baseline_width = content_left + gallery_stack_layout.width;
    float baseline_height = content_top + gallery_stack_layout.height;
    update_extents(baseline_width, baseline_height);

    Widgets::TypographyStyle footer_typography = theme.caption;
    float footer_line_height = footer_typography.line_height;
    float footer_y = baseline_height + 32.0f;
    auto footer = Widgets::BuildLabel(
        Widgets::LabelBuildParams::Make("Close window to exit", footer_typography)
            .WithOrigin(kDefaultMargin, footer_y + footer_typography.baseline_shift)
            .WithColor(theme.muted_text_color)
            .WithDrawable(next_drawable_id++, std::string("widget/gallery/footer"), 0.6f));
    if (footer) {
        pending.emplace_back(std::move(footer->bucket));
        update_extents(kDefaultMargin + footer->width, footer_y + footer_line_height);
    }

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
                           Widgets::StackLayoutState const& controls_stack_layout,
                           Widgets::StackLayoutState const& gallery_stack_layout,
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
                                      controls_stack_layout,
                                      gallery_stack_layout,
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

using FocusTarget = WidgetInput::FocusTarget;

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
    Widgets::StackPaths controls_stack_paths;
    Widgets::StackPaths gallery_stack_paths;
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
    Widgets::StackLayoutParams controls_stack_params{};
    Widgets::StackLayoutState controls_stack_layout{};
    Widgets::StackLayoutParams gallery_stack_params{};
    Widgets::StackLayoutState gallery_stack_layout{};
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

static void rebuild_bindings(WidgetsExampleContext& ctx) {
    auto app_view = SP::App::AppRootPathView{ctx.app_root.getPath()};
    auto target_view = SP::ConcretePathStringView{ctx.target_path};

    ctx.button_binding = unwrap_or_exit(WidgetBindings::CreateButtonBinding(*ctx.space,
                                                                            app_view,
                                                                            ctx.button_paths,
                                                                            target_view,
                                                                            WidgetInput::MakeDirtyHint(ctx.gallery.layout.button_footprint)),
                                        "create button binding");
    WidgetBindings::AddActionCallback(ctx.button_binding, [](WidgetReducers::WidgetAction const& action) {
        if (action.kind == WidgetBindings::WidgetOpKind::Press
            || action.kind == WidgetBindings::WidgetOpKind::Activate) {
            std::cout << "widgets_example: Hello from PathSpace!" << std::endl;
        }
    });

    ctx.toggle_binding = unwrap_or_exit(WidgetBindings::CreateToggleBinding(*ctx.space,
                                                                            app_view,
                                                                            ctx.toggle_paths,
                                                                            target_view,
                                                                            WidgetInput::MakeDirtyHint(ctx.gallery.layout.toggle_footprint)),
                                        "create toggle binding");

    auto slider_dirty_hint = WidgetInput::MakeDirtyHint(ctx.gallery.layout.slider_footprint);
    ctx.slider_binding = unwrap_or_exit(WidgetBindings::CreateSliderBinding(*ctx.space,
                                                                            app_view,
                                                                            ctx.slider_paths,
                                                                            target_view,
                                                                            slider_dirty_hint),
                                        "create slider binding");

    ctx.list_binding = unwrap_or_exit(WidgetBindings::CreateListBinding(*ctx.space,
                                                                        app_view,
                                                                        ctx.list_paths,
                                                                        target_view,
                                                                        WidgetInput::MakeDirtyHint(ctx.gallery.layout.list_footprint)),
                                      "create list binding");

    ctx.stack_binding = unwrap_or_exit(WidgetBindings::CreateStackBinding(*ctx.space,
                                                                          app_view,
                                                                          ctx.stack_paths,
                                                                          target_view,
                                                                          WidgetInput::MakeDirtyHint(ctx.gallery.layout.stack_footprint)),
                                       "create stack binding");

    ctx.tree_binding = unwrap_or_exit(WidgetBindings::CreateTreeBinding(*ctx.space,
                                                                        app_view,
                                                                        ctx.tree_paths,
                                                                        target_view,
                                                                        WidgetInput::MakeDirtyHint(ctx.gallery.layout.tree_footprint)),
                                      "create tree binding");
}

static void recompute_gallery_layout(WidgetsExampleContext& ctx, int window_width) {
    if (!ctx.space) {
        return;
    }

    float available_width = static_cast<float>(window_width) - 2.0f * kDefaultMargin;
    if (available_width < 320.0f) {
        available_width = 320.0f;
    }

    ctx.gallery_stack_params.style.width = available_width;
    unwrap_or_exit(Widgets::UpdateStackLayout(*ctx.space, ctx.gallery_stack_paths, ctx.gallery_stack_params),
                   "update gallery stack layout");
    ctx.gallery_stack_layout = unwrap_or_exit(Widgets::ReadStackLayout(*ctx.space, ctx.gallery_stack_paths),
                                              "read gallery stack layout");

    ctx.controls_stack_layout = unwrap_or_exit(Widgets::ReadStackLayout(*ctx.space, ctx.controls_stack_paths),
                                               "read gallery controls stack layout");
}

static auto make_input_context(WidgetsExampleContext& ctx) -> WidgetInput::WidgetInputContext {
    WidgetInput::WidgetInputContext input{};

    WidgetInput::LayoutSnapshot layout{};
    layout.button = ctx.gallery.layout.button;
    layout.button_footprint = ctx.gallery.layout.button_footprint;
    layout.toggle = ctx.gallery.layout.toggle;
    layout.toggle_footprint = ctx.gallery.layout.toggle_footprint;
    layout.slider_footprint = ctx.gallery.layout.slider_footprint;

    if (ctx.gallery.layout.slider.width() > 0.0f || ctx.gallery.layout.slider.height() > 0.0f) {
        WidgetInput::SliderLayout slider_layout{};
        slider_layout.bounds = ctx.gallery.layout.slider;
        slider_layout.track = ctx.gallery.layout.slider_track;
        layout.slider = slider_layout;
    } else {
        layout.slider.reset();
    }

    layout.list_footprint = ctx.gallery.layout.list_footprint;
    if (!ctx.gallery.layout.list.item_bounds.empty()) {
        WidgetInput::ListLayout list_layout{};
        list_layout.bounds = ctx.gallery.layout.list.bounds;
        list_layout.item_bounds = ctx.gallery.layout.list.item_bounds;
        list_layout.content_top = ctx.gallery.layout.list.content_top;
        list_layout.item_height = ctx.gallery.layout.list.item_height;
        layout.list = list_layout;
    } else {
        layout.list.reset();
    }

    layout.tree_footprint = ctx.gallery.layout.tree_footprint;
    if (!ctx.gallery.layout.tree.rows.empty()) {
        WidgetInput::TreeLayout tree_layout{};
        tree_layout.bounds = ctx.gallery.layout.tree.bounds;
        tree_layout.content_top = ctx.gallery.layout.tree.content_top;
        tree_layout.row_height = ctx.gallery.layout.tree.row_height;
        tree_layout.rows = ctx.gallery.layout.tree.rows;
        layout.tree = tree_layout;
    } else {
        layout.tree.reset();
    }

    input.layout = std::move(layout);

    static constexpr std::array<WidgetInput::FocusTarget, 5> kFocusOrder{
        WidgetInput::FocusTarget::Button,
        WidgetInput::FocusTarget::Toggle,
        WidgetInput::FocusTarget::Slider,
        WidgetInput::FocusTarget::List,
        WidgetInput::FocusTarget::Tree,
    };

    input.space = ctx.space;
    input.focus.config = &ctx.focus_config;
    input.focus.current = &ctx.focus_target;
    input.focus.order = std::span<const WidgetInput::FocusTarget>{kFocusOrder};
    input.focus.button = ctx.button_paths.root;
    input.focus.toggle = ctx.toggle_paths.root;
    input.focus.slider = ctx.slider_paths.root;
    input.focus.list = ctx.list_paths.root;
    input.focus.tree = ctx.tree_paths.root;
    input.focus.focus_list_index = &ctx.focus_list_index;
    input.focus.focus_tree_index = &ctx.focus_tree_index;

    input.button_binding = &ctx.button_binding;
    input.button_paths = &ctx.button_paths;
    input.button_state = &ctx.button_state;

    input.toggle_binding = &ctx.toggle_binding;
    input.toggle_paths = &ctx.toggle_paths;
    input.toggle_state = &ctx.toggle_state;

    input.slider_binding = &ctx.slider_binding;
    input.slider_paths = &ctx.slider_paths;
    input.slider_state = &ctx.slider_state;
    input.slider_style = &ctx.slider_style;
    input.slider_range = &ctx.slider_range;

    input.list_binding = &ctx.list_binding;
    input.list_paths = &ctx.list_paths;
    input.list_state = &ctx.list_state;
    input.list_style = &ctx.list_style;
    input.list_items = &ctx.list_items;

    input.tree_binding = &ctx.tree_binding;
    input.tree_paths = &ctx.tree_paths;
    input.tree_state = &ctx.tree_state;
    input.tree_style = &ctx.tree_style;
    input.tree_nodes = &ctx.tree_nodes;

    input.pointer_x = &ctx.pointer_x;
    input.pointer_y = &ctx.pointer_y;
    input.pointer_down = &ctx.pointer_down;
    input.slider_dragging = &ctx.slider_dragging;
    input.tree_pointer_down_id = &ctx.tree_pointer_down_id;
    input.tree_pointer_toggle = &ctx.tree_pointer_toggle;

    return input;
}

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

using WidgetTraceEvent = SP::UI::WidgetTraceEvent;
using WidgetTraceEventKind = SP::UI::WidgetTraceEventKind;

auto widget_trace() -> SP::UI::WidgetTrace& {
    static auto trace = [] {
        SP::UI::WidgetTraceOptions options;
        options.record_env = "WIDGETS_EXAMPLE_TRACE_RECORD";
        options.replay_env = "WIDGETS_EXAMPLE_TRACE_REPLAY";
        options.log_prefix = "widgets_example";
        return SP::UI::WidgetTrace{options};
    }();
    return trace;
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

static auto make_pointer_info(WidgetsExampleContext const& ctx, bool inside) -> WidgetBindings::PointerInfo {
    return WidgetBindings::PointerInfo::Make(ctx.pointer_x, ctx.pointer_y)
        .WithInside(inside);
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
    float relative = y - layout.bounds.min_y - layout.content_top + ctx.list_state.scroll_offset;
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
                                        ctx.controls_stack_layout,
                                        ctx.gallery_stack_layout,
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
        auto processed = WidgetReducers::ProcessPendingActions(*ctx.space, root);
        if (!processed) {
            std::cerr << "widgets_example: failed to process widget actions for " << root.getPath()
                      << ": " << processed.error().message.value_or("unknown error") << "\n";
            return;
        }
        if (processed->actions.empty()) {
            return;
        }

        for (auto const& action : processed->actions) {
            std::cout << "[widgets_example] action widget=" << action.widget_path
                      << " kind=" << static_cast<int>(action.kind)
                      << " value=" << action.analog_value << std::endl;
        }

        while (true) {
            auto action = ctx.space->take<WidgetReducers::WidgetAction, std::string>(processed->actions_queue.getPath());
            if (!action) {
                break;
            }
        }
    };

    process_root(ctx.button_paths.root);
    process_root(ctx.toggle_paths.root);
    process_root(ctx.slider_paths.root);
    process_root(ctx.list_paths.root);
    process_root(ctx.stack_paths.root);
    process_root(ctx.tree_paths.root);
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
    auto apply_update = [&](WidgetInput::InputUpdate const& update) {
        if (update.state_changed || update.focus_changed) {
            reload_widget_states(*ctx);
            refresh_gallery(*ctx);
        }
    };

    auto with_input = [&](auto&& callable) {
        WidgetInput::WidgetInputContext input = make_input_context(*ctx);
        apply_update(callable(input));
    };

    switch (ev.type) {
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        if (ev.x >= 0 && ev.y >= 0) {
            auto input = make_input_context(*ctx);
            auto result = WidgetInput::HandlePointerMove(input,
                                                        static_cast<float>(ev.x),
                                                        static_cast<float>(ev.y));
            apply_update(result);
        }
        break;
    case SP::UI::LocalMouseEventType::Move: {
        float target_x = ctx->pointer_x + static_cast<float>(ev.dx);
        float target_y = ctx->pointer_y + static_cast<float>(ev.dy);
        auto input = make_input_context(*ctx);
        auto result = WidgetInput::HandlePointerMove(input, target_x, target_y);
        apply_update(result);
        break;
    }
    case SP::UI::LocalMouseEventType::ButtonDown:
        if (ev.x >= 0 && ev.y >= 0) {
            auto input_move = make_input_context(*ctx);
            auto move_result = WidgetInput::HandlePointerMove(input_move,
                                                              static_cast<float>(ev.x),
                                                              static_cast<float>(ev.y));
            apply_update(move_result);
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            auto input = make_input_context(*ctx);
            auto result = WidgetInput::HandlePointerDown(input);
            apply_update(result);
        }
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        if (ev.x >= 0 && ev.y >= 0) {
            auto input_move = make_input_context(*ctx);
            auto move_result = WidgetInput::HandlePointerMove(input_move,
                                                              static_cast<float>(ev.x),
                                                              static_cast<float>(ev.y));
            apply_update(move_result);
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            auto input = make_input_context(*ctx);
            auto result = WidgetInput::HandlePointerUp(input);
            apply_update(result);
        }
        break;
    case SP::UI::LocalMouseEventType::Wheel: {
        auto input = make_input_context(*ctx);
        auto result = WidgetInput::HandlePointerWheel(input, ev.wheel);
        apply_update(result);
        break;
    }
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
    auto [start_x, start_y] = WidgetInput::SliderPointerForValue(make_input_context(ctx), start_value);
    auto [target_x, target_y] = WidgetInput::SliderPointerForValue(make_input_context(ctx), clamped_target);

    auto send_absolute_move = [&](float x, float y) {
        dispatch_simulated_mouse_event(
            ctx,
            SP::UI::Builders::Widgets::MakeMouseEvent(SP::MouseEventType::AbsoluteMove,
                                                      static_cast<int>(std::lround(x)),
                                                      static_cast<int>(std::lround(y))));
        process_widget_actions(ctx);
    };

    auto send_button_down = [&](float x, float y) {
        dispatch_simulated_mouse_event(
            ctx,
            SP::UI::Builders::Widgets::MakeMouseEvent(SP::MouseEventType::ButtonDown,
                                                      static_cast<int>(std::lround(x)),
                                                      static_cast<int>(std::lround(y))));
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

    auto apply_update = [&](WidgetInput::InputUpdate const& update) {
        if (update.state_changed || update.focus_changed) {
            reload_widget_states(*ctx);
            refresh_gallery(*ctx);
        }
    };

    auto with_input = [&](auto&& callable) {
        WidgetInput::WidgetInputContext input = make_input_context(*ctx);
        apply_update(callable(input));
    };

    switch (ev.keycode) {
    case kKeycodeTab:
        with_input([&](WidgetInput::WidgetInputContext& input) {
            return WidgetInput::CycleFocus(input, !has_modifier(ev.modifiers, LocalKeyModifierShift));
        });
        break;
    case kKeycodeSpace:
    case kKeycodeReturn:
        with_input([&](WidgetInput::WidgetInputContext& input) {
            return WidgetInput::ActivateFocusedWidget(input);
        });
        break;
    case kKeycodeLeft:
        if (ctx->focus_target == FocusTarget::Slider) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::AdjustSliderByStep(input, -1);
            });
        } else if (ctx->focus_target == FocusTarget::List) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveListFocus(input, -1);
            });
        } else if (ctx->focus_target == FocusTarget::Tree) {
            if (ctx->focus_tree_index >= 0
                && ctx->focus_tree_index < static_cast<int>(ctx->gallery.layout.tree.rows.size())) {
                auto const& row = ctx->gallery.layout.tree.rows[static_cast<std::size_t>(ctx->focus_tree_index)];
                if (row.expandable && row.expanded) {
                    with_input([&](WidgetInput::WidgetInputContext& input) {
                        return WidgetInput::TreeApplyOp(input, WidgetBindings::WidgetOpKind::TreeCollapse);
                    });
                } else {
                    int parent = WidgetInput::TreeParentIndex(make_input_context(*ctx), ctx->focus_tree_index);
                    if (parent >= 0) {
                        ctx->focus_tree_index = parent;
                        with_input([&](WidgetInput::WidgetInputContext& input) {
                            return WidgetInput::MoveTreeFocus(input, 0);
                        });
                    }
                }
            }
        } else {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::CycleFocus(input, false);
            });
        }
        break;
    case kKeycodeUp:
        if (ctx->focus_target == FocusTarget::Slider) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::AdjustSliderByStep(input, 1);
            });
        } else if (ctx->focus_target == FocusTarget::List) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveListFocus(input, -1);
            });
        } else if (ctx->focus_target == FocusTarget::Tree) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveTreeFocus(input, -1);
            });
        } else {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::CycleFocus(input, false);
            });
        }
        break;
    case kKeycodeRight:
        if (ctx->focus_target == FocusTarget::Slider) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::AdjustSliderByStep(input, 1);
            });
        } else if (ctx->focus_target == FocusTarget::List) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveListFocus(input, 1);
            });
        } else if (ctx->focus_target == FocusTarget::Tree) {
            if (ctx->focus_tree_index >= 0
                && ctx->focus_tree_index < static_cast<int>(ctx->gallery.layout.tree.rows.size())) {
                auto const& row = ctx->gallery.layout.tree.rows[static_cast<std::size_t>(ctx->focus_tree_index)];
                if (row.expandable && !row.expanded) {
                    with_input([&](WidgetInput::WidgetInputContext& input) {
                        return WidgetInput::TreeApplyOp(input, WidgetBindings::WidgetOpKind::TreeExpand);
                    });
                } else {
                    int child = tree_first_child_index(*ctx, ctx->focus_tree_index);
                    if (child >= 0) {
                        ctx->focus_tree_index = child;
                        with_input([&](WidgetInput::WidgetInputContext& input) {
                            return WidgetInput::MoveTreeFocus(input, 0);
                        });
                    }
                }
            }
        } else {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::CycleFocus(input, true);
            });
        }
        break;
    case kKeycodeDown:
        if (ctx->focus_target == FocusTarget::Slider) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::AdjustSliderByStep(input, -1);
            });
        } else if (ctx->focus_target == FocusTarget::List) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveListFocus(input, 1);
            });
        } else if (ctx->focus_target == FocusTarget::Tree) {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::MoveTreeFocus(input, 1);
            });
        } else {
            with_input([&](WidgetInput::WidgetInputContext& input) {
                return WidgetInput::CycleFocus(input, true);
            });
        }
        break;
    default:
        break;
    }
}

static void apply_trace_event(WidgetsExampleContext& ctx, WidgetTraceEvent const& event) {
    switch (event.kind) {
    case WidgetTraceEventKind::MouseAbsolute:
        handle_pointer_move(ctx,
                            static_cast<float>(event.x),
                            static_cast<float>(event.y));
        break;
    case WidgetTraceEventKind::MouseRelative:
        handle_pointer_move(ctx,
                            ctx.pointer_x + static_cast<float>(event.dx),
                            ctx.pointer_y + static_cast<float>(event.dy));
        break;
    case WidgetTraceEventKind::MouseDown:
        if (event.x >= 0 && event.y >= 0) {
            handle_pointer_move(ctx,
                                static_cast<float>(event.x),
                                static_cast<float>(event.y));
        }
        handle_pointer_down(ctx);
        break;
    case WidgetTraceEventKind::MouseUp:
        if (event.x >= 0 && event.y >= 0) {
            handle_pointer_move(ctx,
                                static_cast<float>(event.x),
                                static_cast<float>(event.y));
        }
        handle_pointer_up(ctx);
        break;
    case WidgetTraceEventKind::MouseWheel:
        handle_pointer_wheel(ctx, event.wheel);
        break;
    case WidgetTraceEventKind::KeyDown: {
        auto key = SP::UI::Builders::Widgets::MakeLocalKeyEvent(SP::UI::LocalKeyEventType::KeyDown,
                                                                event.keycode,
                                                                event.modifiers,
                                                                event.character,
                                                                event.repeat);
        handle_local_keyboard(key, &ctx);
        break;
    }
    case WidgetTraceEventKind::KeyUp: {
        auto key = SP::UI::Builders::Widgets::MakeLocalKeyEvent(SP::UI::LocalKeyEventType::KeyUp,
                                                                event.keycode,
                                                                event.modifiers,
                                                                event.character,
                                                                event.repeat);
        handle_local_keyboard(key, &ctx);
        break;
    }
    }
}

static void run_replay_session(WidgetsExampleContext& ctx,
                               std::vector<WidgetTraceEvent> const& events) {
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
    attach_demo_fonts(space, appRootView, theme);

    auto& trace = widget_trace();
    trace.init_from_env();
    if (trace.replaying() && !trace.replay_path().empty()) {
        std::cout << "widgets_example: replay trace '" << trace.replay_path() << "'\n";
    } else if (trace.recording() && !trace.record_path().empty()) {
        std::cout << "widgets_example: tracing pointer/key events to '" << trace.record_path() << "'\n";
    }

    auto button_params = Widgets::MakeButtonParams("primary_button", "Primary")
                             .WithTheme(theme)
                             .ModifyStyle([](Widgets::ButtonStyle& style) {
                                 style.width = 180.0f;
                                 style.height = 44.0f;
                             })
                             .Build();
    auto button = unwrap_or_exit(Widgets::CreateButton(space, appRootView, button_params),
                                 "create button widget");

    auto button_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, button.scene),
                                          "read button revision");
    std::cout << "widgets_example published button widget:\n"
              << "  scene: " << button.scene.getPath() << " (revision "
              << button_revision.revision << ")\n"
              << "  state path: " << button.state.getPath() << "\n"
              << "  label path: " << button.label.getPath() << "\n";

    auto toggle_params = Widgets::MakeToggleParams("primary_toggle")
                             .WithTheme(theme)
                             .ModifyStyle([](Widgets::ToggleStyle& style) {
                                 style.width = 60.0f;
                                 style.height = 32.0f;
                             })
                             .Build();
    auto toggle = unwrap_or_exit(Widgets::CreateToggle(space, appRootView, toggle_params),
                                 "create toggle widget");

    auto toggle_state = Widgets::MakeToggleState()
                            .WithChecked(true)
                            .Build();
    unwrap_or_exit(Widgets::UpdateToggleState(space, toggle, toggle_state),
                   "update toggle state");

    auto toggle_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, toggle.scene),
                                          "read toggle revision");
    std::cout << "widgets_example published toggle widget:\n"
              << "  scene: " << toggle.scene.getPath() << " (revision "
              << toggle_revision.revision << ")\n"
              << "  state path: " << toggle.state.getPath() << "\n";

    auto slider_params = Widgets::MakeSliderParams("volume_slider")
                             .WithRange(0.0f, 100.0f)
                             .WithValue(25.0f)
                             .WithStep(5.0f)
                             .WithTheme(theme)
                             .Build();
    auto slider = unwrap_or_exit(Widgets::CreateSlider(space, appRootView, slider_params),
                                 "create slider widget");

    auto slider_state = Widgets::MakeSliderState()
                            .WithValue(45.0f)
                            .Build();
    unwrap_or_exit(Widgets::UpdateSliderState(space, slider, slider_state),
                   "update slider state");

    auto slider_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, slider.scene),
                                          "read slider revision");
    std::cout << "widgets_example published slider widget:\n"
              << "  scene: " << slider.scene.getPath() << " (revision "
              << slider_revision.revision << ")\n"
              << "  state path: " << slider.state.getPath() << "\n"
              << "  range path: " << slider.range.getPath() << "\n";

    auto list_params = Widgets::MakeListParams("inventory_list")
                           .WithItems({
                               Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
                               Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
                               Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = true},
                           })
                           .WithTheme(theme)
                           .ModifyStyle([](Widgets::ListStyle& style) {
                               style.width = 240.0f;
                               style.item_height = 36.0f;
                           })
                           .Build();
    auto list = unwrap_or_exit(Widgets::CreateList(space, appRootView, list_params),
                               "create list widget");

    auto list_state = Widgets::MakeListState()
                          .WithSelectedIndex(1)
                          .Build();
    unwrap_or_exit(Widgets::UpdateListState(space, list, list_state),
                   "update list state");

    auto list_revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, list.scene),
                                        "read list revision");
    std::cout << "widgets_example published list widget:\n"
              << "  scene: " << list.scene.getPath() << " (revision "
              << list_revision.revision << ")\n"
              << "  state path: " << list.state.getPath() << "\n"
              << "  items path: " << list.items.getPath() << "\n";

    auto tree_params = Widgets::MakeTreeParams("workspace_tree")
                           .WithNodes({
                               Widgets::TreeNode{.id = "workspace", .parent_id = "", .label = "workspace/", .enabled = true, .expandable = true, .loaded = true},
                               Widgets::TreeNode{.id = "docs", .parent_id = "workspace", .label = "docs/", .enabled = true, .expandable = false, .loaded = false},
                               Widgets::TreeNode{.id = "src", .parent_id = "workspace", .label = "src/", .enabled = true, .expandable = true, .loaded = true},
                               Widgets::TreeNode{.id = "src_builders", .parent_id = "src", .label = "ui/builders.cpp", .enabled = true, .expandable = false, .loaded = false},
                               Widgets::TreeNode{.id = "src_renderer", .parent_id = "src", .label = "ui/renderer.cpp", .enabled = true, .expandable = false, .loaded = false},
                               Widgets::TreeNode{.id = "tests", .parent_id = "workspace", .label = "tests/", .enabled = true, .expandable = false, .loaded = false},
                           })
                           .WithTheme(theme)
                           .Build();
    auto tree = unwrap_or_exit(Widgets::CreateTree(space, appRootView, tree_params),
                               "create tree widget");

    auto tree_state = Widgets::MakeTreeState()
                          .WithEnabled(true)
                          .WithSelectedId("workspace")
                          .WithExpandedIds({"workspace", "src"})
                          .Build();
    unwrap_or_exit(Widgets::UpdateTreeState(space, tree, tree_state),
                   "initialize tree state");

    auto tree_state_live = unwrap_or_exit(space.read<Widgets::TreeState, std::string>(std::string(tree.state.getPath())),
                                          "read tree state");
    auto tree_style_live = unwrap_or_exit(space.read<Widgets::TreeStyle, std::string>(std::string(tree.root.getPath()) + "/meta/style"),
                                          "read tree style");
    auto tree_nodes_live = unwrap_or_exit(space.read<std::vector<Widgets::TreeNode>, std::string>(std::string(tree.nodes.getPath())),
                                          "read tree nodes");

    auto stack_params = Widgets::MakeStackLayoutParams("widget_stack")
                            .ModifyStyle([](Widgets::StackLayoutStyle& style) {
                                style.axis = Widgets::StackAxis::Vertical;
                                style.spacing = 24.0f;
                                style.padding_main_start = 16.0f;
                                style.padding_main_end = 16.0f;
                                style.padding_cross_start = 20.0f;
                                style.padding_cross_end = 20.0f;
                            })
                            .WithChildren({
                                Widgets::StackChildSpec{.id = "stack_button", .widget_path = button.root.getPath(), .scene_path = button.scene.getPath()},
                                Widgets::StackChildSpec{.id = "stack_toggle", .widget_path = toggle.root.getPath(), .scene_path = toggle.scene.getPath()},
                                Widgets::StackChildSpec{.id = "stack_slider", .widget_path = slider.root.getPath(), .scene_path = slider.scene.getPath()},
                            })
                            .Build();

    auto stack = unwrap_or_exit(Widgets::CreateStack(space, appRootView, stack_params),
                                "create stack layout");
    auto stack_desc = unwrap_or_exit(Widgets::DescribeStack(space, stack),
                                     "describe stack layout");
    auto stack_layout_live = unwrap_or_exit(Widgets::ReadStackLayout(space, stack),
                                            "read stack layout");

    auto controls_stack_params = Widgets::MakeStackLayoutParams("gallery_controls")
                                     .ModifyStyle([](Widgets::StackLayoutStyle& style) {
                                         style.axis = Widgets::StackAxis::Horizontal;
                                         style.spacing = 32.0f;
                                         style.align_cross = Widgets::StackAlignCross::Center;
                                     })
                                     .WithChildren({
                                         Widgets::StackChildSpec{
                                             .id = std::string{kControlsStackChildButton},
                                             .widget_path = button.root.getPath(),
                                             .scene_path = button.scene.getPath(),
                                         },
                                         Widgets::StackChildSpec{
                                             .id = std::string{kControlsStackChildToggle},
                                             .widget_path = toggle.root.getPath(),
                                             .scene_path = toggle.scene.getPath(),
                                         },
                                     })
                                     .Build();

    auto controls_stack = unwrap_or_exit(Widgets::CreateStack(space, appRootView, controls_stack_params),
                                         "create gallery controls stack");
    auto controls_stack_layout = unwrap_or_exit(Widgets::ReadStackLayout(space, controls_stack),
                                                "read gallery controls stack layout");

    auto gallery_stack_params = Widgets::MakeStackLayoutParams("gallery_layout")
                                    .ModifyStyle([](Widgets::StackLayoutStyle& style) {
                                        style.axis = Widgets::StackAxis::Vertical;
                                        style.spacing = 48.0f;
                                        style.align_cross = Widgets::StackAlignCross::Stretch;
                                    })
                                    .WithChildren({
                                        Widgets::StackChildSpec{
                                            .id = std::string{kGalleryStackChildControls},
                                            .widget_path = controls_stack.root.getPath(),
                                            .scene_path = controls_stack.scene.getPath(),
                                        },
                                        Widgets::StackChildSpec{
                                            .id = std::string{kGalleryStackChildSlider},
                                            .widget_path = slider.root.getPath(),
                                            .scene_path = slider.scene.getPath(),
                                        },
                                        Widgets::StackChildSpec{
                                            .id = std::string{kGalleryStackChildList},
                                            .widget_path = list.root.getPath(),
                                            .scene_path = list.scene.getPath(),
                                        },
                                        Widgets::StackChildSpec{
                                            .id = std::string{kGalleryStackChildStack},
                                            .widget_path = stack.root.getPath(),
                                            .scene_path = stack.scene.getPath(),
                                        },
                                        Widgets::StackChildSpec{
                                            .id = std::string{kGalleryStackChildTree},
                                            .widget_path = tree.root.getPath(),
                                            .scene_path = tree.scene.getPath(),
                                        },
                                    })
                                    .Build();

    auto gallery_stack = unwrap_or_exit(Widgets::CreateStack(space, appRootView, gallery_stack_params),
                                        "create gallery layout stack");
    auto gallery_stack_layout = unwrap_or_exit(Widgets::ReadStackLayout(space, gallery_stack),
                                               "read gallery layout stack");

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
                                         controls_stack_layout,
                                         gallery_stack_layout,
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
    ctx.controls_stack_paths = controls_stack;
    ctx.gallery_stack_paths = gallery_stack;
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
    ctx.controls_stack_params = controls_stack_params;
    ctx.controls_stack_layout = controls_stack_layout;
    ctx.gallery_stack_params = gallery_stack_params;
    ctx.gallery_stack_layout = gallery_stack_layout;
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

    rebuild_bindings(ctx);

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
            recompute_gallery_layout(ctx, window_width);
            refresh_gallery(ctx);
            rebuild_bindings(ctx);
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
