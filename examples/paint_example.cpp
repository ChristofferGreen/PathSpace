#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/TextBuilder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

using namespace SP;
using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

namespace PaintInput {

enum class MouseButton : int {
    Left = 1,
    Right = 2,
    Middle = 3,
    Button4 = 4,
    Button5 = 5,
};

enum class MouseEventType {
    Move,
    AbsoluteMove,
    ButtonDown,
    ButtonUp,
    Wheel,
};

struct MouseEvent {
    MouseEventType type = MouseEventType::Move;
    MouseButton button = MouseButton::Left;
    int dx = 0;
    int dy = 0;
    int x = -1;
    int y = -1;
    int wheel = 0;
};

std::mutex gMouseMutex;
std::deque<MouseEvent> gMouseQueue;

void enqueue_mouse(MouseEvent const& ev) {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    gMouseQueue.push_back(ev);
}

auto try_pop_mouse() -> std::optional<MouseEvent> {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    if (gMouseQueue.empty()) {
        return std::nullopt;
    }
    MouseEvent ev = gMouseQueue.front();
    gMouseQueue.pop_front();
    return ev;
}

void clear_mouse() {
    std::lock_guard<std::mutex> lock(gMouseMutex);
    gMouseQueue.clear();
}

} // namespace PaintInput

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <pathspace/ui/LocalWindowBridge.hpp>
#endif

namespace {

using DirtyRectHint = Builders::DirtyRectHint;
namespace Widgets = SP::UI::Builders::Widgets;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;
namespace WidgetInput = SP::UI::Builders::Widgets::Input;

struct PaletteEntry {
    std::string id;
    std::string label;
    std::array<float, 4> color;
};

struct PaletteButton {
    PaletteEntry entry{};
    Widgets::ButtonPaths paths{};
    WidgetBindings::ButtonBinding binding{};
    Widgets::ButtonStyle style{};
    Widgets::ButtonState state{};
    WidgetInput::WidgetBounds bounds{};
};

struct SliderControl {
    Widgets::SliderPaths paths{};
    WidgetBindings::SliderBinding binding{};
    Widgets::SliderStyle style{};
    Widgets::SliderState state{};
    Widgets::SliderRange range{};
    WidgetInput::WidgetBounds bounds{};
    float label_top = 0.0f;
    float label_baseline = 0.0f;
};

struct PaintControls {
    Widgets::WidgetTheme theme{};
    std::vector<PaletteButton> buttons;
    SliderControl slider{};
    WidgetInput::WidgetBounds panel_bounds{};
    DirtyRectHint dirty_hint{};
    bool dirty = false;
    int selected_index = 0;
    bool slider_dragging = false;
    int active_button = -1;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_valid = false;
    int brush_size_value = 8;
    UIScene::DrawableBucketSnapshot bucket{};
    float origin_x = 24.0f;
    float origin_y = 24.0f;
    float button_width = 68.0f;
    float button_height = 36.0f;
    float button_spacing = 8.0f;
    float row_spacing = 8.0f;
    float slider_spacing = 20.0f;
    int buttons_per_row = 3;
};

auto mix_color(std::array<float, 4> base,
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

auto lighten_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

auto relative_luminance(std::array<float, 4> const& color) -> float {
    return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
}

auto choose_text_color(std::array<float, 4> const& background) -> std::array<float, 4> {
    float lum = relative_luminance(background);
    if (lum > 0.65f) {
        return {0.12f, 0.14f, 0.18f, 1.0f};
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
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

auto translate_bucket(UIScene::DrawableBucketSnapshot& bucket, float dx, float dy) -> void {
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
        auto kind = static_cast<UIScene::DrawCommandKind>(kind_value);
        switch (kind) {
        case UIScene::DrawCommandKind::Rect: {
            auto cmd = read_command<UIScene::RectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case UIScene::DrawCommandKind::RoundedRect: {
            auto cmd = read_command<UIScene::RoundedRectCommand>(bucket.command_payload, offset);
            cmd.min_x += dx;
            cmd.max_x += dx;
            cmd.min_y += dy;
            cmd.max_y += dy;
            write_command(bucket.command_payload, offset, cmd);
            break;
        }
        case UIScene::DrawCommandKind::TextGlyphs: {
            auto cmd = read_command<UIScene::TextGlyphsCommand>(bucket.command_payload, offset);
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
        offset += UIScene::payload_size_bytes(kind);
    }
}

auto append_bucket(UIScene::DrawableBucketSnapshot& dest,
                   UIScene::DrawableBucketSnapshot const& src) -> void {
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
        UIScene::LayerIndices adjusted{entry.layer, {}};
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

auto default_palette_entries() -> std::vector<PaletteEntry>;
auto find_palette_index(std::vector<PaletteEntry> const& entries,
                        std::array<float, 4> const& color) -> int;
auto slider_value_from_position(SliderControl const& slider,
                                float scene_x) -> float;
auto refresh_button_state(PathSpace& space, PaletteButton& button) -> void;
auto refresh_slider_state(PathSpace& space, SliderControl& slider) -> void;
auto build_controls_bucket(PaintControls const& controls) -> UIScene::DrawableBucketSnapshot;
auto initialize_controls(PathSpace& space,
                         SP::App::AppRootPathView app_root,
                         SP::ConcretePathStringView target_path,
                         PaintControls& controls,
                         std::array<float, 4>& brush_color,
                         int initial_brush_size,
                         std::string const& brush_color_path,
                         std::string const& brush_size_path) -> void;
auto handle_controls_event(PaintControls& controls,
                           PathSpace& space,
                           PaintInput::MouseEvent const& event,
                           std::string const& brush_color_path,
                           std::string const& brush_size_path,
                           std::array<float, 4>& brush_color) -> bool;

auto default_palette_entries() -> std::vector<PaletteEntry> {
    return {
        {"paint_palette_red", "Red", {0.905f, 0.173f, 0.247f, 1.0f}},
        {"paint_palette_orange", "Orange", {0.972f, 0.545f, 0.192f, 1.0f}},
        {"paint_palette_yellow", "Yellow", {0.995f, 0.847f, 0.207f, 1.0f}},
        {"paint_palette_green", "Green", {0.172f, 0.701f, 0.368f, 1.0f}},
        {"paint_palette_blue", "Blue", {0.157f, 0.407f, 0.933f, 1.0f}},
        {"paint_palette_purple", "Purple", {0.560f, 0.247f, 0.835f, 1.0f}},
    };
}

auto find_palette_index(std::vector<PaletteEntry> const& entries,
                        std::array<float, 4> const& color) -> int {
    auto matches = [&](PaletteEntry const& entry) {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(entry.color[i] - color[i]) > 0.05f) {
                return false;
            }
        }
        return true;
    };

    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (matches(entries[index])) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

auto slider_value_from_position(SliderControl const& slider,
                                float scene_x) -> float {
    if (slider.style.width <= 0.0f) {
        return slider.range.minimum;
    }
    float local_x = std::clamp(scene_x - slider.bounds.min_x, 0.0f, slider.style.width);
    float t = local_x / slider.style.width;
    float value = slider.range.minimum + t * (slider.range.maximum - slider.range.minimum);
    if (slider.range.step > 0.0f) {
        float steps = std::round((value - slider.range.minimum) / slider.range.step);
        value = slider.range.minimum + steps * slider.range.step;
    }
    return std::clamp(value, slider.range.minimum, slider.range.maximum);
}

auto refresh_button_state(PathSpace& space, PaletteButton& button) -> void {
    auto state = space.read<Widgets::ButtonState, std::string>(std::string(button.paths.state.getPath()));
    if (state) {
        button.state = *state;
    }
}

auto refresh_slider_state(PathSpace& space, SliderControl& slider) -> void {
    auto state = space.read<Widgets::SliderState, std::string>(std::string(slider.paths.state.getPath()));
    if (state) {
        slider.state = *state;
    }
}

auto build_controls_bucket(PaintControls const& controls) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    std::uint64_t next_drawable_id = 1'000'000ull;

    for (std::size_t index = 0; index < controls.buttons.size(); ++index) {
        auto const& button = controls.buttons[index];
        Widgets::ButtonStyle style = button.style;
        if (static_cast<int>(index) == controls.selected_index) {
            style.background_color = lighten_color(style.background_color, 0.20f);
            style.text_color = choose_text_color(style.background_color);
        }

        auto preview = Widgets::BuildButtonPreview(
            style,
            button.state,
            Widgets::ButtonPreviewOptions{
                .authoring_root = std::string(button.paths.root.getPath()) + "/authoring",
                .pulsing_highlight = button.state.focused,
            });
        translate_bucket(preview,
                         button.bounds.min_x,
                         button.bounds.min_y);
        append_bucket(bucket, preview);
    }

    std::string slider_caption = "Brush Size: " + std::to_string(controls.brush_size_value) + " px";
    auto caption = Widgets::BuildLabel(
        Widgets::LabelBuildParams::Make(slider_caption, controls.theme.caption)
            .WithOrigin(controls.slider.bounds.min_x,
                        controls.slider.label_baseline)
            .WithColor(controls.theme.caption_color)
            .WithDrawable(next_drawable_id++, std::string("widgets/paint/slider/label"), 0.5f));
    if (caption) {
        append_bucket(bucket, caption->bucket);
    }

    auto slider_preview = Widgets::BuildSliderPreview(
        controls.slider.style,
        controls.slider.range,
        controls.slider.state,
        Widgets::SliderPreviewOptions{
            .authoring_root = std::string(controls.slider.paths.root.getPath()) + "/authoring",
            .pulsing_highlight = controls.slider.state.focused,
        });
    translate_bucket(slider_preview,
                     controls.slider.bounds.min_x,
                     controls.slider.bounds.min_y);
    append_bucket(bucket, slider_preview);

    return bucket;
}

#if defined(__APPLE__)
void handle_local_mouse(SP::UI::LocalMouseEvent const& ev, void*) {
    PaintInput::MouseEvent out{};
    switch (ev.type) {
    case SP::UI::LocalMouseEventType::Move:
        out.type = PaintInput::MouseEventType::Move;
        out.dx = ev.dx;
        out.dy = ev.dy;
        break;
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        out.type = PaintInput::MouseEventType::AbsoluteMove;
        break;
    case SP::UI::LocalMouseEventType::ButtonDown:
        out.type = PaintInput::MouseEventType::ButtonDown;
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        out.type = PaintInput::MouseEventType::ButtonUp;
        break;
    case SP::UI::LocalMouseEventType::Wheel:
        out.type = PaintInput::MouseEventType::Wheel;
        out.wheel = ev.wheel;
        break;
    }

    switch (ev.button) {
    case SP::UI::LocalMouseButton::Left:
        out.button = PaintInput::MouseButton::Left;
        break;
    case SP::UI::LocalMouseButton::Right:
        out.button = PaintInput::MouseButton::Right;
        break;
    case SP::UI::LocalMouseButton::Middle:
        out.button = PaintInput::MouseButton::Middle;
        break;
    case SP::UI::LocalMouseButton::Button4:
        out.button = PaintInput::MouseButton::Button4;
        break;
    case SP::UI::LocalMouseButton::Button5:
        out.button = PaintInput::MouseButton::Button5;
        break;
    }

    out.x = ev.x;
    out.y = ev.y;
    PaintInput::enqueue_mouse(out);
}

void clear_local_mouse(void*) {
    PaintInput::clear_mouse();
}
#endif

auto align_down_to_tile(float value, int tileSizePx) -> float {
    auto const tile = static_cast<float>(std::max(1, tileSizePx));
    return std::floor(value / tile) * tile;
}

auto align_up_to_tile(float value, int tileSizePx) -> float {
    auto const tile = static_cast<float>(std::max(1, tileSizePx));
    return std::ceil(value / tile) * tile;
}

auto clamp_and_align_hint(DirtyRectHint const& hint,
                          int canvasWidth,
                          int canvasHeight,
                          int tileSizePx) -> std::optional<DirtyRectHint> {
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return std::nullopt;
    }

    auto const maxX = static_cast<float>(canvasWidth);
    auto const maxY = static_cast<float>(canvasHeight);
    auto const minX = std::clamp(hint.min_x, 0.0f, maxX);
    auto const minY = std::clamp(hint.min_y, 0.0f, maxY);
    auto const alignedMaxX = std::clamp(align_up_to_tile(std::clamp(hint.max_x, 0.0f, maxX), tileSizePx), 0.0f, maxX);
    auto const alignedMaxY = std::clamp(align_up_to_tile(std::clamp(hint.max_y, 0.0f, maxY), tileSizePx), 0.0f, maxY);
    auto const alignedMinX = std::clamp(align_down_to_tile(minX, tileSizePx), 0.0f, maxX);
    auto const alignedMinY = std::clamp(align_down_to_tile(minY, tileSizePx), 0.0f, maxY);

    if (alignedMaxX <= alignedMinX || alignedMaxY <= alignedMinY) {
        return std::nullopt;
    }

return DirtyRectHint{
        .min_x = alignedMinX,
        .min_y = alignedMinY,
        .max_x = alignedMaxX,
        .max_y = alignedMaxY,
    };
}

template <typename T>
auto replace_value(PathSpace& space, std::string const& path, T const& value) -> bool {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& err = taken.error();
        if (err.code == SP::Error::Code::NoObjectFound
            || err.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        std::cerr << "failed clearing '" << path << "': ";
        if (err.message.has_value()) {
            std::cerr << *err.message;
        } else {
            std::cerr << static_cast<int>(err.code);
        }
        std::cerr << std::endl;
        return false;
    }

    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        auto const& err = result.errors.front();
        std::cerr << "failed writing '" << path << "': ";
        if (err.message.has_value()) {
            std::cerr << *err.message;
        } else {
            std::cerr << static_cast<int>(err.code);
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

void ensure_config_value(PathSpace& space,
                         std::string const& path,
                         int defaultValue) {
    auto value = space.read<int>(path);
    if (value) {
        return;
    }
    auto const& err = value.error();
    if (err.code == SP::Error::Code::NoObjectFound
        || err.code == SP::Error::Code::NoSuchPath) {
        replace_value(space, path, defaultValue);
    }
}

auto read_config_value(PathSpace& space,
                       std::string const& path,
                       int fallback) -> int {
    auto value = space.read<int>(path);
    if (value) {
        return std::max(1, *value);
    }
    return std::max(1, fallback);
}

struct RuntimeOptions {
    bool debug = false;
    bool metal = false;
    double uncapped_present_hz = 60.0;
};

auto parse_runtime_options(int argc, char** argv) -> RuntimeOptions {
    RuntimeOptions opts{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--metal") {
            opts.metal = true;
        } else if (arg.rfind("--present-hz=", 0) == 0) {
            auto value = std::string(arg.substr(std::string_view("--present-hz=").size()));
            char* end = nullptr;
            double parsed = std::strtod(value.c_str(), &end);
            if (end && *end == '\0' && std::isfinite(parsed)) {
                opts.uncapped_present_hz = parsed;
            }
        } else if (arg == "--present-hz") {
            if (i + 1 < argc) {
                ++i;
                char* end = nullptr;
                double parsed = std::strtod(argv[i], &end);
                if (end && *end == '\0' && std::isfinite(parsed)) {
                    opts.uncapped_present_hz = parsed;
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: paint_example [--debug] [--metal] [--present-hz=<value|0>]\n";
            std::exit(0);
        }
    }
    if (!(opts.uncapped_present_hz > 0.0)) {
        opts.uncapped_present_hz = 0.0;
    }
    return opts;
}

struct Stroke {
    std::uint64_t drawable_id = 0;
    std::vector<UIScene::StrokePoint> points;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
    float thickness = 1.0f;
    DirtyRectHint bounds{0.0f, 0.0f, 0.0f, 0.0f};
    std::string authoring_id;
};

struct CanvasState {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
    std::uint64_t fingerprint = 0;
    bool dirty = false;
};

struct CanvasDrawable {
    std::uint64_t drawable_id = 0;
    std::uint64_t fingerprint = 0;
    int width = 0;
    int height = 0;
};

constexpr std::uint64_t kCanvasDrawableId = 1;
constexpr std::uint64_t kInitialCanvasFingerprint = 0xC001000000000000ULL;

auto clamp_dimension(int value) -> int {
    return value < 0 ? 0 : value;
}

auto reset_canvas(CanvasState& canvas, int width, int height) -> void {
    canvas.width = clamp_dimension(width);
    canvas.height = clamp_dimension(height);
    std::size_t pixel_count = static_cast<std::size_t>(canvas.width) * static_cast<std::size_t>(canvas.height);
    canvas.pixels.resize(pixel_count * 4u);
    std::fill(canvas.pixels.begin(), canvas.pixels.end(), static_cast<std::uint8_t>(255));
    canvas.dirty = false;
    canvas.fingerprint = 0;
}

auto ensure_canvas(CanvasState& canvas, int width, int height) -> void {
    if (canvas.width != width || canvas.height != height || canvas.pixels.empty()) {
        reset_canvas(canvas, width, height);
    }
}

auto encode_canvas_png(CanvasState const& canvas) -> std::vector<std::uint8_t> {
    if (canvas.width <= 0 || canvas.height <= 0 || canvas.pixels.empty()) {
        return {};
    }
    int out_len = 0;
    unsigned char* encoded = stbi_write_png_to_mem(
        canvas.pixels.data(),
        canvas.width * 4,
        canvas.width,
        canvas.height,
        4,
        &out_len);
    if (!encoded || out_len <= 0) {
        if (encoded) {
            STBIW_FREE(encoded);
        }
        return {};
    }
    std::vector<std::uint8_t> png_bytes(encoded, encoded + out_len);
    STBIW_FREE(encoded);
    return png_bytes;
}

auto to_uint8(float value) -> std::uint8_t {
    auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::round(clamped * 255.0f));
}

auto composite_stroke(CanvasState& canvas,
                      Stroke const& stroke) -> void {
    if (canvas.width <= 0 || canvas.height <= 0 || canvas.pixels.empty() || stroke.points.empty()) {
        return;
    }

    auto radius = std::max(1.0f, stroke.thickness * 0.5f);
    auto radius_sq = radius * radius;
    auto color_r = to_uint8(stroke.color[0]);
    auto color_g = to_uint8(stroke.color[1]);
    auto color_b = to_uint8(stroke.color[2]);
    auto color_a = to_uint8(stroke.color[3]);

    auto draw_disc = [&](float cx, float cy) {
        int min_x = std::max(0, static_cast<int>(std::floor(cx - radius)));
        int max_x = std::min(canvas.width - 1, static_cast<int>(std::ceil(cx + radius)));
        int min_y = std::max(0, static_cast<int>(std::floor(cy - radius)));
        int max_y = std::min(canvas.height - 1, static_cast<int>(std::ceil(cy + radius)));
        for (int y = min_y; y <= max_y; ++y) {
            float dy = (static_cast<float>(y) + 0.5f) - cy;
            for (int x = min_x; x <= max_x; ++x) {
                float dx = (static_cast<float>(x) + 0.5f) - cx;
                float dist_sq = dx * dx + dy * dy;
                if (dist_sq > radius_sq) {
                    continue;
                }
                std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width)
                                     + static_cast<std::size_t>(x)) * 4u;
                canvas.pixels[index + 0] = color_r;
                canvas.pixels[index + 1] = color_g;
                canvas.pixels[index + 2] = color_b;
                canvas.pixels[index + 3] = color_a;
            }
        }
    };

    auto draw_segment = [&](UIScene::StrokePoint const& a,
                            UIScene::StrokePoint const& b) {
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float dist = std::hypot(dx, dy);
        int steps = dist > 0.0f ? std::max(1, static_cast<int>(std::ceil(dist / std::max(1.0f, radius * 0.5f)))) : 1;
        for (int i = 0; i <= steps; ++i) {
            float t = steps == 0 ? 0.0f : static_cast<float>(i) / static_cast<float>(steps);
            float px = a.x + dx * t;
            float py = a.y + dy * t;
            draw_disc(px, py);
        }
    };

    auto prev = stroke.points.front();
    draw_disc(prev.x, prev.y);
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        auto const& current = stroke.points[i];
        draw_segment(prev, current);
        prev = current;
    }

    canvas.dirty = true;
}

auto make_canvas_drawable(CanvasState const& canvas,
                          std::uint64_t drawable_id) -> std::optional<CanvasDrawable> {
    if (canvas.width <= 0 || canvas.height <= 0 || canvas.pixels.empty() || canvas.fingerprint == 0) {
        return std::nullopt;
    }
    CanvasDrawable drawable{};
    drawable.drawable_id = drawable_id;
    drawable.fingerprint = canvas.fingerprint;
    drawable.width = canvas.width;
    drawable.height = canvas.height;
    return drawable;
}

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto fingerprint_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (int i = 0; i < 16; ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
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

auto initialize_controls(PathSpace& space,
                         SP::App::AppRootPathView app_root,
                         SP::ConcretePathStringView target_path,
                         PaintControls& controls,
                         std::array<float, 4>& brush_color,
                         int initial_brush_size,
                         std::string const& brush_color_path,
                         std::string const& brush_size_path) -> void {
    controls.theme = Widgets::MakeDefaultWidgetTheme();
    controls.buttons.clear();

    auto palette = default_palette_entries();
    controls.buttons.reserve(palette.size());

    controls.brush_size_value = std::max(1, initial_brush_size);
    controls.selected_index = find_palette_index(palette, brush_color);
    if (controls.selected_index < 0
        || controls.selected_index >= static_cast<int>(palette.size())) {
        controls.selected_index = 0;
        if (!palette.empty()) {
            brush_color = palette.front().color;
            replace_value(space, brush_color_path, brush_color);
        }
    }

    float max_x = controls.origin_x;
    float max_y = controls.origin_y;

    auto target_view = target_path;

    for (std::size_t index = 0; index < palette.size(); ++index) {
        auto const& entry = palette[index];

        Widgets::ButtonParams params{};
        params.name = entry.id;
        params.label = entry.label;
        params.style = controls.theme.button;
        params.style.width = controls.button_width;
        params.style.height = controls.button_height;
        params.style.background_color = entry.color;
        params.style.text_color = choose_text_color(entry.color);

        auto paths = unwrap_or_exit(Widgets::CreateButton(space, app_root, params),
                                    "create paint palette button");

        int row = static_cast<int>(index) / std::max(1, controls.buttons_per_row);
        int col = static_cast<int>(index) % std::max(1, controls.buttons_per_row);

        float x = controls.origin_x + static_cast<float>(col) * (controls.button_width + controls.button_spacing);
        float y = controls.origin_y + static_cast<float>(row) * (controls.button_height + controls.row_spacing);

        PaletteButton button{};
        button.entry = entry;
        button.paths = std::move(paths);
        button.style = params.style;
        button.bounds = WidgetInput::WidgetBounds{x, y, x + controls.button_width, y + controls.button_height};
        WidgetInput::ExpandForFocusHighlight(button.bounds);

        auto hint = WidgetInput::MakeDirtyHint(button.bounds);
        button.binding = unwrap_or_exit(WidgetBindings::CreateButtonBinding(space,
                                                                            app_root,
                                                                            button.paths,
                                                                            target_view,
                                                                            hint),
                                        "create paint palette button binding");

        refresh_button_state(space, button);

        auto controls_ptr = &controls;
        auto space_ptr = &space;
        auto brush_color_ptr = &brush_color;
        auto color_path_copy = brush_color_path;
        auto index_int = static_cast<int>(index);

        WidgetBindings::AddActionCallback(button.binding,
            [controls_ptr, space_ptr, brush_color_ptr, color_path_copy, index_int](WidgetReducers::WidgetAction const& action) {
                if (action.kind != WidgetBindings::WidgetOpKind::Activate) {
                    return;
                }
                if (index_int < 0 || index_int >= static_cast<int>(controls_ptr->buttons.size())) {
                    return;
                }
                controls_ptr->selected_index = index_int;
                *brush_color_ptr = controls_ptr->buttons[index_int].entry.color;
                controls_ptr->dirty = true;
                replace_value(*space_ptr, color_path_copy, *brush_color_ptr);
            });

        max_x = std::max(max_x, button.bounds.max_x);
        max_y = std::max(max_y, button.bounds.max_y);

        controls.buttons.push_back(std::move(button));
    }

    int rows = palette.empty() ? 0 : static_cast<int>((palette.size() + controls.buttons_per_row - 1) / controls.buttons_per_row);
    float buttons_height = rows > 0
        ? static_cast<float>(rows) * controls.button_height
          + static_cast<float>(std::max(0, rows - 1)) * controls.row_spacing
        : 0.0f;

    controls.slider.style = controls.theme.slider;
    controls.slider.style.label_color = controls.theme.caption_color;
    controls.slider.style.label_typography = controls.theme.caption;
    float buttons_row_width = controls.button_width * std::max(1, controls.buttons_per_row)
                              + controls.button_spacing * static_cast<float>(std::max(0, controls.buttons_per_row - 1));
    controls.slider.style.width = std::max(controls.slider.style.width, buttons_row_width);
    controls.slider.style.height = std::max(controls.slider.style.height, 28.0f);

    controls.slider.range = Widgets::SliderRange{
        .minimum = 1.0f,
        .maximum = 64.0f,
        .step = 1.0f,
    };

    Widgets::SliderParams slider_params{};
    slider_params.name = "paint_brush_size";
    slider_params.minimum = controls.slider.range.minimum;
    slider_params.maximum = controls.slider.range.maximum;
    slider_params.value = static_cast<float>(controls.brush_size_value);
    slider_params.step = controls.slider.range.step;
    slider_params.style = controls.slider.style;

    controls.slider.paths = unwrap_or_exit(Widgets::CreateSlider(space, app_root, slider_params),
                                           "create paint brush size slider");
    controls.slider.style = slider_params.style;
    controls.slider.range = Widgets::SliderRange{
        slider_params.minimum,
        slider_params.maximum,
        slider_params.step,
    };

    refresh_slider_state(space, controls.slider);
    controls.slider.state.value = static_cast<float>(controls.brush_size_value);

    float slider_label_top = controls.origin_y + buttons_height + controls.slider_spacing;
    controls.slider.label_top = slider_label_top;
    controls.slider.label_baseline = slider_label_top + controls.theme.caption.baseline_shift;
    float label_height = controls.theme.caption.line_height;
    float slider_top = slider_label_top + label_height + 6.0f;

    controls.slider.bounds = WidgetInput::WidgetBounds{
        controls.origin_x,
        slider_top,
        controls.origin_x + controls.slider.style.width,
        slider_top + controls.slider.style.height,
    };
    WidgetInput::ExpandForFocusHighlight(controls.slider.bounds);

    auto slider_hint = WidgetInput::MakeDirtyHint(controls.slider.bounds);
    controls.slider.binding = unwrap_or_exit(WidgetBindings::CreateSliderBinding(space,
                                                                                app_root,
                                                                                controls.slider.paths,
                                                                                target_view,
                                                                                slider_hint),
                                             "create paint brush size slider binding");

    auto controls_ptr = &controls;
    auto space_ptr = &space;
    auto brush_size_path_copy = brush_size_path;

    WidgetBindings::AddActionCallback(controls.slider.binding,
        [controls_ptr, space_ptr, brush_size_path_copy](WidgetReducers::WidgetAction const& action) {
            switch (action.kind) {
            case WidgetBindings::WidgetOpKind::SliderBegin:
            case WidgetBindings::WidgetOpKind::SliderUpdate:
            case WidgetBindings::WidgetOpKind::SliderCommit: {
                int value = std::max(1, static_cast<int>(std::round(action.analog_value)));
                if (controls_ptr->brush_size_value != value) {
                    controls_ptr->brush_size_value = value;
                    controls_ptr->dirty = true;
                    replace_value(*space_ptr, brush_size_path_copy, value);
                }
                break;
            }
            default:
                break;
            }
        });

    max_x = std::max(max_x, controls.slider.bounds.max_x);
    max_y = std::max(max_y, controls.slider.bounds.max_y);

    controls.panel_bounds = WidgetInput::WidgetBounds{
        controls.origin_x - 12.0f,
        controls.origin_y - 12.0f,
        max_x + 12.0f,
        max_y + 12.0f,
    };
    controls.panel_bounds.normalize();
    controls.dirty_hint = WidgetInput::MakeDirtyHint(controls.panel_bounds);

    controls.bucket = build_controls_bucket(controls);
    controls.dirty = false;
    controls.slider_dragging = false;
    controls.active_button = -1;
    controls.pointer_valid = false;

    replace_value(space, brush_size_path, controls.brush_size_value);
}

auto handle_controls_event(PaintControls& controls,
                           PathSpace& space,
                           PaintInput::MouseEvent const& event,
                           std::string const& brush_color_path,
                           std::string const& brush_size_path,
                           std::array<float, 4>& brush_color) -> bool {
    auto update_pointer = [&](PaintInput::MouseEvent const& ev) -> std::optional<std::pair<float, float>> {
        switch (ev.type) {
        case PaintInput::MouseEventType::AbsoluteMove:
            if (ev.x >= 0 && ev.y >= 0) {
                controls.pointer_x = static_cast<float>(ev.x);
                controls.pointer_y = static_cast<float>(ev.y);
                controls.pointer_valid = true;
                return std::pair<float, float>{controls.pointer_x, controls.pointer_y};
            }
            break;
        case PaintInput::MouseEventType::Move:
            if (controls.pointer_valid) {
                controls.pointer_x += static_cast<float>(ev.dx);
                controls.pointer_y += static_cast<float>(ev.dy);
                return std::pair<float, float>{controls.pointer_x, controls.pointer_y};
            }
            break;
        default:
            break;
        }

        if (ev.x >= 0 && ev.y >= 0) {
            controls.pointer_x = static_cast<float>(ev.x);
            controls.pointer_y = static_cast<float>(ev.y);
            controls.pointer_valid = true;
            return std::pair<float, float>{controls.pointer_x, controls.pointer_y};
        }

        if (controls.pointer_valid) {
            return std::pair<float, float>{controls.pointer_x, controls.pointer_y};
        }
        return std::nullopt;
    };

    auto pointer = update_pointer(event);

    auto make_pointer_info = [&](bool inside, bool primary) {
        return WidgetBindings::PointerInfo::Make(controls.pointer_x, controls.pointer_y)
            .WithInside(inside)
            .WithPrimary(primary);
    };

    auto dispatch_button = [&](int index,
                               WidgetBindings::WidgetOpKind kind,
                               Widgets::ButtonState desired,
                               bool inside) {
        auto pointer_info = make_pointer_info(inside, true);
        auto result = WidgetBindings::DispatchButton(space,
                                                    controls.buttons[index].binding,
                                                    desired,
                                                    kind,
                                                    pointer_info);
        if (!result) {
            std::cerr << "paint_example: button dispatch failed: "
                      << result.error().message.value_or("unknown error") << std::endl;
            return false;
        }
        if (*result) {
            refresh_button_state(space, controls.buttons[index]);
            controls.dirty = true;
        }
        return true;
    };

    auto dispatch_slider = [&](WidgetBindings::WidgetOpKind kind,
                               float value,
                               bool inside) {
        Widgets::SliderState desired = controls.slider.state;
        desired.value = value;
        desired.dragging = (kind != WidgetBindings::WidgetOpKind::SliderCommit);
        auto pointer_info = make_pointer_info(inside, true);
        auto result = WidgetBindings::DispatchSlider(space,
                                                     controls.slider.binding,
                                                     desired,
                                                     kind,
                                                     pointer_info);
        if (!result) {
            std::cerr << "paint_example: slider dispatch failed: "
                      << result.error().message.value_or("unknown error") << std::endl;
            return false;
        }
        if (*result) {
            refresh_slider_state(space, controls.slider);
            controls.brush_size_value = std::max(1, static_cast<int>(std::round(controls.slider.state.value)));
            controls.dirty = true;
        }
        return true;
    };

    switch (event.type) {
    case PaintInput::MouseEventType::AbsoluteMove:
    case PaintInput::MouseEventType::Move: {
        if (!pointer) {
            return false;
        }
        bool inside_slider = controls.slider.bounds.contains(pointer->first, pointer->second);
        if (controls.slider_dragging) {
            float value = slider_value_from_position(controls.slider, pointer->first);
            dispatch_slider(WidgetBindings::WidgetOpKind::SliderUpdate, value, inside_slider);
            return true;
        }
        return false;
    }
    case PaintInput::MouseEventType::ButtonDown: {
        if (event.button != PaintInput::MouseButton::Left || !pointer) {
            return false;
        }

        bool inside_slider = controls.slider.bounds.contains(pointer->first, pointer->second);
        if (inside_slider) {
            controls.slider_dragging = true;
            float value = slider_value_from_position(controls.slider, pointer->first);
            dispatch_slider(WidgetBindings::WidgetOpKind::SliderBegin, value, true);
            return true;
        }

        for (std::size_t index = 0; index < controls.buttons.size(); ++index) {
            auto const& button = controls.buttons[index];
            if (!button.bounds.contains(pointer->first, pointer->second)) {
                continue;
            }
            controls.active_button = static_cast<int>(index);
            Widgets::ButtonState desired = button.state;
            desired.pressed = true;
            desired.hovered = true;
            dispatch_button(controls.active_button,
                            WidgetBindings::WidgetOpKind::Press,
                            desired,
                            true);
            return true;
        }

        return controls.panel_bounds.contains(pointer->first, pointer->second);
    }
    case PaintInput::MouseEventType::ButtonUp: {
        if (event.button != PaintInput::MouseButton::Left) {
            return false;
        }

        bool consumed = false;
        bool inside_slider = pointer ? controls.slider.bounds.contains(pointer->first, pointer->second) : false;
        if (controls.slider_dragging) {
            controls.slider_dragging = false;
            if (!pointer) {
                pointer = std::pair<float, float>{controls.pointer_x, controls.pointer_y};
                inside_slider = controls.slider.bounds.contains(controls.pointer_x, controls.pointer_y);
            }
            float value = slider_value_from_position(controls.slider, pointer ? pointer->first : controls.pointer_x);
            dispatch_slider(WidgetBindings::WidgetOpKind::SliderCommit, value, inside_slider);
            consumed = true;
        }

        if (controls.active_button >= 0 && controls.active_button < static_cast<int>(controls.buttons.size())) {
            auto& button = controls.buttons[controls.active_button];
            bool inside = pointer ? button.bounds.contains(pointer->first, pointer->second) : false;
            Widgets::ButtonState desired = button.state;
            desired.pressed = false;
            desired.hovered = inside;
            dispatch_button(controls.active_button,
                            WidgetBindings::WidgetOpKind::Release,
                            desired,
                            inside);
            if (inside) {
                desired = controls.buttons[controls.active_button].state;
                desired.pressed = false;
                desired.hovered = true;
                dispatch_button(controls.active_button,
                                WidgetBindings::WidgetOpKind::Activate,
                                desired,
                                true);
                brush_color = controls.buttons[controls.active_button].entry.color;
                replace_value(space, brush_color_path, brush_color);
            }
            controls.active_button = -1;
            consumed = true;
        }

        if (!consumed && pointer) {
            consumed = controls.panel_bounds.contains(pointer->first, pointer->second);
        }
        return consumed;
    }
    case PaintInput::MouseEventType::Wheel: {
        if (!pointer) {
            return false;
        }
        return controls.panel_bounds.contains(pointer->first, pointer->second);
    }
    default:
        break;
    }

    return false;
}

auto encode_image_command(UIScene::ImageCommand const& image,
                          UIScene::DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::ImageCommand));
    std::memcpy(bucket.command_payload.data() + offset, &image, sizeof(UIScene::ImageCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Image));
}

auto build_bucket(std::optional<CanvasDrawable> const& canvasDrawable,
                  std::vector<Stroke> const& strokes,
                  UIScene::DrawableBucketSnapshot const* controls_bucket) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};

    std::size_t drawable_count = canvasDrawable.has_value() ? 1 : 0;
    std::size_t total_points = 0;
    for (auto const& stroke : strokes) {
        if (!stroke.points.empty()) {
            ++drawable_count;
            total_points += stroke.points.size();
        }
    }

    bucket.drawable_ids.reserve(drawable_count);
    bucket.world_transforms.reserve(drawable_count);
    bucket.bounds_spheres.reserve(drawable_count);
    bucket.bounds_boxes.reserve(drawable_count);
    bucket.bounds_box_valid.reserve(drawable_count);
    bucket.layers.reserve(drawable_count);
    bucket.z_values.reserve(drawable_count);
    bucket.material_ids.reserve(drawable_count);
    bucket.pipeline_flags.reserve(drawable_count);
    bucket.visibility.reserve(drawable_count);
    bucket.command_offsets.reserve(drawable_count);
    bucket.command_counts.reserve(drawable_count);
    bucket.command_kinds.reserve(drawable_count);
    bucket.clip_head_indices.reserve(drawable_count);
    bucket.authoring_map.reserve(drawable_count);
    bucket.drawable_fingerprints.reserve(drawable_count);
    bucket.stroke_points.reserve(total_points);

    if (canvasDrawable) {
        auto drawable_index = bucket.drawable_ids.size();
        bucket.drawable_ids.push_back(canvasDrawable->drawable_id);
        bucket.world_transforms.push_back(identity_transform());

        UIScene::BoundingBox box{};
        box.min = {0.0f, 0.0f, 0.0f};
        box.max = {static_cast<float>(canvasDrawable->width),
                   static_cast<float>(canvasDrawable->height),
                   0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto half_width = static_cast<float>(canvasDrawable->width) * 0.5f;
        auto half_height = static_cast<float>(canvasDrawable->height) * 0.5f;
        UIScene::BoundingSphere sphere{};
        sphere.center = {half_width, half_height, 0.0f};
        sphere.radius = std::sqrt(half_width * half_width + half_height * half_height);
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(drawable_index));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);

        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_counts.push_back(1);
        bucket.clip_head_indices.push_back(-1);

        UIScene::ImageCommand image{};
        image.min_x = 0.0f;
        image.min_y = 0.0f;
        image.max_x = static_cast<float>(canvasDrawable->width);
        image.max_y = static_cast<float>(canvasDrawable->height);
        image.uv_min_x = 0.0f;
        image.uv_min_y = 0.0f;
        image.uv_max_x = 1.0f;
        image.uv_max_y = 1.0f;
        image.image_fingerprint = canvasDrawable->fingerprint;
        image.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        encode_image_command(image, bucket);

        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            canvasDrawable->drawable_id,
            "nodes/paint/canvas_image",
            0,
            0});
        bucket.drawable_fingerprints.push_back(canvasDrawable->fingerprint);
    }

    for (auto const& stroke : strokes) {
        if (stroke.points.empty()) {
            continue;
        }

        auto drawable_index = bucket.drawable_ids.size();
        bucket.drawable_ids.push_back(stroke.drawable_id);
        bucket.world_transforms.push_back(identity_transform());

        DirtyRectHint bounds = stroke.bounds;
        bounds.min_x = std::max(0.0f, bounds.min_x);
        bounds.min_y = std::max(0.0f, bounds.min_y);
        bounds.max_x = std::max(bounds.min_x, bounds.max_x);
        bounds.max_y = std::max(bounds.min_y, bounds.max_y);

        UIScene::BoundingBox box{};
        box.min = {bounds.min_x, bounds.min_y, 0.0f};
        box.max = {bounds.max_x, bounds.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto half_width = std::max(0.0f, bounds.max_x - bounds.min_x) * 0.5f;
        auto half_height = std::max(0.0f, bounds.max_y - bounds.min_y) * 0.5f;
        UIScene::BoundingSphere sphere{};
        sphere.center = {bounds.min_x + half_width, bounds.min_y + half_height, 0.0f};
        sphere.radius = std::sqrt(half_width * half_width + half_height * half_height);
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(drawable_index));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);

        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_counts.push_back(1);
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Stroke));
        bucket.clip_head_indices.push_back(-1);

        UIScene::StrokeCommand stroke_cmd{};
        stroke_cmd.min_x = bounds.min_x;
        stroke_cmd.min_y = bounds.min_y;
        stroke_cmd.max_x = bounds.max_x;
        stroke_cmd.max_y = bounds.max_y;
        stroke_cmd.thickness = stroke.thickness;
        stroke_cmd.point_offset = static_cast<std::uint32_t>(bucket.stroke_points.size());
        stroke_cmd.point_count = static_cast<std::uint32_t>(stroke.points.size());
        stroke_cmd.color = stroke.color;

        auto payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(UIScene::StrokeCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset, &stroke_cmd, sizeof(UIScene::StrokeCommand));

        bucket.stroke_points.insert(bucket.stroke_points.end(), stroke.points.begin(), stroke.points.end());

        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            stroke.drawable_id,
            stroke.authoring_id,
            0,
            0});
        bucket.drawable_fingerprints.push_back(stroke.drawable_id);
    }

    auto final_count = bucket.drawable_ids.size();
    bucket.opaque_indices.resize(final_count);
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0u);
    bucket.alpha_indices.clear();

    if (controls_bucket) {
        append_bucket(bucket, *controls_bucket);
    }

    return bucket;
}

auto publish_snapshot(PathSpace& space,
                      UIScene::SceneSnapshotBuilder& builder,
                      UIScene::ScenePath const& scenePath,
                      UIScene::DrawableBucketSnapshot const& bucket) -> std::uint64_t {
    UIScene::SnapshotPublishOptions opts{};
    opts.metadata.author = "paint_example";
    opts.metadata.tool_version = "paint_example";
    opts.metadata.created_at = std::chrono::system_clock::now();
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = unwrap_or_exit(builder.publish(opts, bucket), "failed to publish paint scene snapshot");
    return revision;
}

struct PresentOutcome {
    bool used_iosurface = false;
    std::size_t framebuffer_bytes = 0;
    std::size_t stride_bytes = 0;
    bool skipped = false;
};

auto present_frame(PathSpace& space,
                   Builders::WindowPath const& windowPath,
                   std::string_view viewName,
                   int width,
                   int height,
                   bool debug,
                   double uncapped_present_hz) -> std::optional<PresentOutcome> {
    auto presentResult = Builders::Window::Present(space, windowPath, viewName);
    if (!presentResult) {
        std::cerr << "present failed";
        if (presentResult.error().message.has_value()) {
            std::cerr << ": " << *presentResult.error().message;
        }
        std::cerr << std::endl;
        return std::nullopt;
    }
#if defined(__APPLE__)
    static std::chrono::steady_clock::time_point last_present_time{};
    static bool last_present_time_valid = false;
    std::size_t computed_stride = 0;
    bool allow_present = true;
    auto decision_time = std::chrono::steady_clock::time_point{};
    bool decision_time_valid = false;
    if (!presentResult->stats.vsync_aligned && uncapped_present_hz > 0.0) {
        decision_time = std::chrono::steady_clock::now();
        decision_time_valid = true;
        auto min_interval = std::chrono::duration<double>(1.0 / uncapped_present_hz);
        if (last_present_time_valid && (decision_time - last_present_time) < min_interval) {
            allow_present = false;
        }
    } else if (presentResult->stats.vsync_aligned) {
        last_present_time_valid = false;
    }

    Builders::App::PresentToLocalWindowResult dispatched{};
    if (allow_present) {
        Builders::App::PresentToLocalWindowOptions options{};
        options.allow_framebuffer = !presentResult->stats.used_metal_texture;
        dispatched = Builders::App::PresentToLocalWindow(*presentResult,
                                                         width,
                                                         height,
                                                         options);
        computed_stride = dispatched.row_stride_bytes;
        if (presentResult->stats.used_metal_texture && !dispatched.presented) {
            presentResult->framebuffer.clear();
        }
    } else {
        dispatched.skipped = presentResult->stats.skipped;
    }

    if (!presentResult->stats.vsync_aligned && dispatched.presented) {
        if (decision_time_valid) {
            last_present_time = decision_time;
        } else {
            last_present_time = std::chrono::steady_clock::now();
        }
        last_present_time_valid = true;
    } else if (presentResult->stats.vsync_aligned) {
        last_present_time_valid = false;
    }

    PresentOutcome outcome{};
    outcome.skipped = presentResult->stats.skipped;
    outcome.used_iosurface = dispatched.used_iosurface;
    outcome.framebuffer_bytes = dispatched.framebuffer_bytes;
    if (computed_stride == 0) {
        computed_stride = static_cast<std::size_t>(width) * 4;
    }
    outcome.stride_bytes = computed_stride;
#else
    (void)uncapped_present_hz;
    auto dispatched = Builders::App::PresentToLocalWindow(*presentResult,
                                                          width,
                                                          height);
    PresentOutcome outcome{};
    outcome.skipped = presentResult->stats.skipped;
    outcome.used_iosurface = dispatched.used_iosurface;
    outcome.framebuffer_bytes = dispatched.framebuffer_bytes;
    if (dispatched.row_stride_bytes == 0) {
        outcome.stride_bytes = static_cast<std::size_t>(width) * 4;
    } else {
        outcome.stride_bytes = dispatched.row_stride_bytes;
    }
#endif

    if (debug) {
        auto const& stats = presentResult->stats;
        std::cout << "[present] frame=" << stats.frame.frame_index
                  << " render_ms=" << stats.frame.render_ms
                  << " present_ms=" << stats.present_ms
                  << " tiles=" << stats.progressive_tiles_copied
                  << " rects=" << stats.progressive_rects_coalesced
                  << " skipped=" << stats.skipped
                  << " buffered=" << stats.buffered_frame_consumed
                  << " dirty_bytes=" << outcome.framebuffer_bytes
                  << " stride=" << outcome.stride_bytes
                  << std::endl;
    }
    return outcome;
}

auto to_canvas_y(int viewY, int canvasHeight) -> int {
    return std::clamp(viewY, 0, std::max(canvasHeight - 1, 0));
}

auto start_stroke(std::vector<Stroke>& strokes,
                  std::uint64_t& nextId,
                  int canvasWidth,
                  int canvasHeight,
                  int x,
                  int y,
                  std::array<float, 4> const& color,
                  int brushSizePx) -> std::optional<DirtyRectHint> {
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return std::nullopt;
    }
    if (brushSizePx <= 0) {
        brushSizePx = 1;
    }

    int canvasX = std::clamp(x, 0, canvasWidth - 1);
    int canvasY = to_canvas_y(y, canvasHeight);
    float point_x = static_cast<float>(canvasX);
    float point_y = static_cast<float>(canvasY);
    float thickness = static_cast<float>(std::max(1, brushSizePx));
    float half = thickness * 0.5f;

    auto clamp_extent = [&](float value, float delta, float limit) {
        return std::clamp(value + delta, 0.0f, limit);
    };

    DirtyRectHint bounds{};
    bounds.min_x = clamp_extent(point_x, -half, static_cast<float>(canvasWidth));
    bounds.min_y = clamp_extent(point_y, -half, static_cast<float>(canvasHeight));
    bounds.max_x = clamp_extent(point_x, half, static_cast<float>(canvasWidth));
    bounds.max_y = clamp_extent(point_y, half, static_cast<float>(canvasHeight));
    if (bounds.max_x <= bounds.min_x) {
        bounds.max_x = std::min(static_cast<float>(canvasWidth), bounds.min_x + thickness);
    }
    if (bounds.max_y <= bounds.min_y) {
        bounds.max_y = std::min(static_cast<float>(canvasHeight), bounds.min_y + thickness);
    }

    Stroke stroke{};
    stroke.drawable_id = nextId++;
    stroke.points.push_back(UIScene::StrokePoint{point_x, point_y});
    stroke.color = color;
    stroke.thickness = thickness;
    stroke.bounds = bounds;
    stroke.authoring_id = "nodes/paint/stroke_" + std::to_string(strokes.size());
    strokes.push_back(std::move(stroke));

    return bounds;
}

auto extend_stroke(Stroke& stroke,
                   int canvasWidth,
                   int canvasHeight,
                   std::pair<int, int> const& from,
                   std::pair<int, int> const& to,
                   int brushSizePx) -> bool {
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return false;
    }
    if (stroke.points.empty()) {
        return false;
    }

    if (brushSizePx <= 0) {
        brushSizePx = 1;
    }

    auto apply_bounds_growth = [&](float new_thickness) {
        if (new_thickness <= stroke.thickness) {
            return;
        }
        float delta = (new_thickness - stroke.thickness) * 0.5f;
        stroke.bounds.min_x = std::max(0.0f, stroke.bounds.min_x - delta);
        stroke.bounds.min_y = std::max(0.0f, stroke.bounds.min_y - delta);
        stroke.bounds.max_x = std::min(static_cast<float>(canvasWidth), stroke.bounds.max_x + delta);
        stroke.bounds.max_y = std::min(static_cast<float>(canvasHeight), stroke.bounds.max_y + delta);
        stroke.thickness = new_thickness;
    };

    auto append_point = [&](int xi, int yi) -> bool {
        int clamped_x = std::clamp(xi, 0, canvasWidth - 1);
        int clamped_y = to_canvas_y(yi, canvasHeight);
        float px = static_cast<float>(clamped_x);
        float py = static_cast<float>(clamped_y);
        if (!stroke.points.empty()) {
            auto const& last = stroke.points.back();
            if (std::fabs(last.x - px) < 0.1f && std::fabs(last.y - py) < 0.1f) {
                return false;
            }
        }

        float half = stroke.thickness * 0.5f;
        DirtyRectHint hint{};
        hint.min_x = std::clamp(px - half, 0.0f, static_cast<float>(canvasWidth));
        hint.min_y = std::clamp(py - half, 0.0f, static_cast<float>(canvasHeight));
        hint.max_x = std::clamp(px + half, 0.0f, static_cast<float>(canvasWidth));
        hint.max_y = std::clamp(py + half, 0.0f, static_cast<float>(canvasHeight));
        if (hint.max_x <= hint.min_x) {
            hint.max_x = std::min(static_cast<float>(canvasWidth), hint.min_x + stroke.thickness);
        }
        if (hint.max_y <= hint.min_y) {
            hint.max_y = std::min(static_cast<float>(canvasHeight), hint.min_y + stroke.thickness);
        }

        stroke.points.push_back(UIScene::StrokePoint{px, py});
        stroke.bounds.min_x = std::min(stroke.bounds.min_x, hint.min_x);
        stroke.bounds.min_y = std::min(stroke.bounds.min_y, hint.min_y);
        stroke.bounds.max_x = std::max(stroke.bounds.max_x, hint.max_x);
        stroke.bounds.max_y = std::max(stroke.bounds.max_y, hint.max_y);
        return true;
    };

    float new_thickness = static_cast<float>(std::max(1, brushSizePx));
    apply_bounds_growth(new_thickness);

    double x0 = static_cast<double>(from.first);
    double y0 = static_cast<double>(from.second);
    double x1 = static_cast<double>(to.first);
    double y1 = static_cast<double>(to.second);
    double dx = x1 - x0;
    double dy = y1 - y0;
    double dist = std::hypot(dx, dy);
    double spacing = std::max(1.0, static_cast<double>(brushSizePx) * 0.5);
    int steps = (dist > spacing) ? static_cast<int>(std::floor(dist / spacing)) : 0;

    bool wrote = false;
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(steps + 1);
        int xi = static_cast<int>(std::round(x0 + dx * t));
        int yi = static_cast<int>(std::round(y0 + dy * t));
        wrote |= append_point(xi, yi);
    }
    wrote |= append_point(to.first, to.second);

    return wrote;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(__APPLE__)
    std::cerr << "paint_example currently supports only macOS builds." << std::endl;
    return 1;
#else
    auto options = parse_runtime_options(argc, argv);
#if !defined(PATHSPACE_UI_METAL)
    if (options.metal) {
        std::cerr << "--metal requested, but this build was compiled without PATHSPACE_UI_METAL support." << std::endl;
        return 1;
    }
#else
    if (options.metal) {
        if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
            if (::setenv("PATHSPACE_ENABLE_METAL_UPLOADS", "1", 1) != 0) {
                std::cerr << "warning: failed to set PATHSPACE_ENABLE_METAL_UPLOADS=1; Metal uploads may remain disabled." << std::endl;
            }
        }
    }
#endif
    PathSpace space;
    SP::App::AppRootPath appRoot{"/system/applications/paint"};
    auto rootView = SP::App::AppRootPathView{appRoot.getPath()};

    const std::string configBasePath = std::string(rootView.getPath()) + "/config";
    const std::string canvasWidthPath = configBasePath + "/canvasWidthPx";
    const std::string canvasHeightPath = configBasePath + "/canvasHeightPx";
    const std::string brushSizePath = configBasePath + "/brushSizePx";
    const std::string tileSizePath = configBasePath + "/progressiveTileSizePx";
    const std::string brushColorPath = configBasePath + "/brushColorRgba";

    ensure_config_value(space, canvasWidthPath, 320);
    ensure_config_value(space, canvasHeightPath, 240);
    ensure_config_value(space, brushSizePath, 8);
    ensure_config_value(space, tileSizePath, 64);

    int canvasWidth = read_config_value(space, canvasWidthPath, 320);
    int canvasHeight = read_config_value(space, canvasHeightPath, 240);
    std::array<float, 4> brushColor{0.9f, 0.1f, 0.3f, 1.0f};
    auto storedColor = space.read<std::array<float, 4>, std::string>(brushColorPath);
    if (storedColor) {
        brushColor = *storedColor;
    } else {
        replace_value(space, brushColorPath, brushColor);
    }

    SP::UI::SetLocalWindowCallbacks({&handle_local_mouse, &clear_local_mouse, nullptr});
    SP::UI::InitLocalWindowWithSize(canvasWidth, canvasHeight, "PathSpace Paint");

    Builders::SceneParams sceneParams{
        .name = "canvas",
        .description = "paint example canvas",
    };
    auto scenePath = unwrap_or_exit(Builders::Scene::Create(space, rootView, sceneParams),
                                    "failed to create paint scene");

    Builders::App::BootstrapParams bootstrapParams{};
    bootstrapParams.renderer.name = options.metal ? "metal2d" : "software2d";
    bootstrapParams.renderer.kind = options.metal ? Builders::RendererKind::Metal2D : Builders::RendererKind::Software2D;
    bootstrapParams.renderer.description = options.metal ? "paint renderer (Metal2D)" : "paint renderer";
    bootstrapParams.surface.name = "canvas_surface";
    bootstrapParams.surface.desc.size_px.width = canvasWidth;
    bootstrapParams.surface.desc.size_px.height = canvasHeight;
    bootstrapParams.surface.desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    bootstrapParams.surface.desc.color_space = Builders::ColorSpace::sRGB;
    bootstrapParams.surface.desc.premultiplied_alpha = true;
#if defined(PATHSPACE_UI_METAL)
    if (options.metal) {
        bootstrapParams.surface.desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        bootstrapParams.surface.desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                                           | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);
        bootstrapParams.surface.desc.metal.iosurface_backing = true;
    }
#endif
    bootstrapParams.window.name = "window";
    bootstrapParams.window.title = "PathSpace Paint";
    bootstrapParams.window.width = canvasWidth;
    bootstrapParams.window.height = canvasHeight;
    bootstrapParams.window.scale = 1.0f;
    bootstrapParams.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    bootstrapParams.present_policy.vsync_align = false;
    bootstrapParams.present_policy.auto_render_on_present = true;
    bootstrapParams.present_policy.capture_framebuffer = false;
    bootstrapParams.view_name = "main";

    Builders::RenderSettings bootstrapSettings{};
    bootstrapSettings.clear_color = {1.0f, 1.0f, 1.0f, 1.0f};
    bootstrapSettings.surface.size_px.width = canvasWidth;
    bootstrapSettings.surface.size_px.height = canvasHeight;
    bootstrapSettings.surface.dpi_scale = 1.0f;
#if defined(PATHSPACE_UI_METAL)
    if (options.metal) {
        bootstrapSettings.renderer.backend_kind = Builders::RendererKind::Metal2D;
        bootstrapSettings.renderer.metal_uploads_enabled = true;
    }
#endif
    bootstrapParams.renderer_settings_override = bootstrapSettings;

    auto bootstrap = unwrap_or_exit(Builders::App::Bootstrap(space,
                                                             rootView,
                                                             scenePath,
                                                             bootstrapParams),
                                    "failed to bootstrap paint application");

    auto targetAbsolutePath = bootstrap.target.getPath();

    UIScene::SceneSnapshotBuilder builder{space, rootView, scenePath};

    CanvasState canvas{};
    ensure_canvas(canvas, canvasWidth, canvasHeight);
    bool canvas_has_image = false;
    std::uint64_t next_canvas_fingerprint = kInitialCanvasFingerprint;

    std::vector<Stroke> strokes;
    std::uint64_t nextStrokeId = 2;

    int initial_brush_size = read_config_value(space, brushSizePath, 8);
    PaintControls controls{};
    SP::ConcretePathStringView target_view{bootstrap.target.getPath()};
    initialize_controls(space,
                        rootView,
                        target_view,
                        controls,
                        brushColor,
                        initial_brush_size,
                        brushColorPath,
                        brushSizePath);

    auto bucket = build_bucket(std::nullopt, strokes, &controls.bucket);
    auto initial_revision = publish_snapshot(space, builder, scenePath, bucket);
    (void)initial_revision;
    (void)present_frame(space,
                       bootstrap.window,
                       bootstrap.view_name,
                       canvasWidth,
                       canvasHeight,
                       options.debug,
                       options.uncapped_present_hz);

    auto fps_last_report = std::chrono::steady_clock::now();
    std::uint64_t fps_frames = 0;
    std::uint64_t fps_iosurface_frames = 0;
    std::size_t fps_last_stride = 0;
    std::size_t fps_last_framebuffer_bytes = 0;

    bool drawing = false;
    std::optional<std::pair<int, int>> lastAbsolute;
    std::optional<std::pair<int, int>> lastPainted;
    std::vector<DirtyRectHint> dirtyHints;

    while (true) {
        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            break;
        }

        int requestedWidth = canvasWidth;
        int requestedHeight = canvasHeight;
        SP::UI::GetLocalWindowContentSize(&requestedWidth, &requestedHeight);
        if (requestedWidth <= 0 || requestedHeight <= 0) {
            break;
        }

        bool updated = false;
        dirtyHints.clear();

        int brushSizePx = read_config_value(space, brushSizePath, controls.brush_size_value);
        if (brushSizePx != controls.brush_size_value) {
            controls.brush_size_value = brushSizePx;
            controls.slider.state.value = static_cast<float>(brushSizePx);
            controls.dirty = true;
        } else {
            brushSizePx = controls.brush_size_value;
        }

        auto stored_brush = space.read<std::array<float, 4>, std::string>(brushColorPath);
        if (stored_brush && *stored_brush != brushColor) {
            brushColor = *stored_brush;
            int palette_index = find_palette_index(default_palette_entries(), brushColor);
            if (palette_index != controls.selected_index) {
                controls.selected_index = palette_index;
                controls.dirty = true;
            }
        }

        const int tileSizePx = read_config_value(space, tileSizePath, 64);

        bool sizeChanged = (requestedWidth != canvasWidth) || (requestedHeight != canvasHeight);
        if (sizeChanged) {
            canvasWidth = requestedWidth;
            canvasHeight = requestedHeight;
            unwrap_or_exit(Builders::App::UpdateSurfaceSize(space,
                                                            bootstrap,
                                                            canvasWidth,
                                                            canvasHeight),
                           "failed to refresh surface after resize");
            replace_value(space, canvasWidthPath, canvasWidth);
            replace_value(space, canvasHeightPath, canvasHeight);
            ensure_canvas(canvas, canvasWidth, canvasHeight);
            canvas_has_image = false;
            canvas.fingerprint = 0;
            canvas.dirty = false;
            lastPainted.reset();
            lastAbsolute.reset();
            dirtyHints.push_back(DirtyRectHint{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = static_cast<float>(canvasWidth),
                .max_y = static_cast<float>(canvasHeight),
            });
            updated = true;
        }
        while (auto evt = PaintInput::try_pop_mouse()) {
            auto const& e = *evt;

            if (handle_controls_event(controls,
                                      space,
                                      e,
                                      brushColorPath,
                                      brushSizePath,
                                      brushColor)) {
                if (controls.dirty) {
                    updated = true;
                }
                continue;
            }

            switch (e.type) {
            case PaintInput::MouseEventType::AbsoluteMove: {
                if (e.x < 0 || e.y < 0) {
                    break;
                }
                std::pair<int, int> current{e.x, e.y};
                lastAbsolute = current;
                if (drawing) {
                    if (controls.panel_bounds.contains(static_cast<float>(current.first),
                                                        static_cast<float>(current.second))) {
                        break;
                    }
                    if (!lastPainted) {
                        lastPainted = current;
                    }
                    if (!strokes.empty()) {
                        updated |= extend_stroke(strokes.back(),
                                                 canvasWidth,
                                                 canvasHeight,
                                                 *lastPainted,
                                                 current,
                                                 brushSizePx);
                    }
                    lastPainted = current;
                }
                break;
            }
            case PaintInput::MouseEventType::ButtonDown:
                if (e.button == PaintInput::MouseButton::Left) {
                    std::optional<std::pair<int, int>> point;
                    if (e.x >= 0 && e.y >= 0) {
                        point = std::pair<int, int>{e.x, e.y};
                    } else if (lastAbsolute) {
                        point = lastAbsolute;
                    }
                    if (point) {
                        if (controls.panel_bounds.contains(static_cast<float>(point->first),
                                                            static_cast<float>(point->second))) {
                            drawing = false;
                            break;
                        }
                        lastAbsolute = *point;
                        drawing = true;
                        if (auto hint = start_stroke(strokes,
                                                     nextStrokeId,
                                                     canvasWidth,
                                                     canvasHeight,
                                                     point->first,
                                                     point->second,
                                                     brushColor,
                                                     brushSizePx)) {
                            updated = true;
                        }
                        lastPainted = *point;
                    }
                }
                break;
            case PaintInput::MouseEventType::ButtonUp:
                if (e.button == PaintInput::MouseButton::Left) {
                    drawing = false;
                    if (!strokes.empty()) {
                        ensure_canvas(canvas, canvasWidth, canvasHeight);
                        Stroke finished = std::move(strokes.back());
                        strokes.pop_back();
                        composite_stroke(canvas, finished);
                        canvas_has_image = true;
                        canvas.fingerprint = next_canvas_fingerprint++;
                        dirtyHints.push_back(finished.bounds);
                        updated = true;
                    }
                    lastPainted.reset();
                }
                break;
            case PaintInput::MouseEventType::Move:
            case PaintInput::MouseEventType::Wheel:
                // Ignored for painting.
                break;
            }
        }

        if (controls.dirty) {
            controls.bucket = build_controls_bucket(controls);
            controls.dirty = false;
            dirtyHints.push_back(controls.dirty_hint);
            updated = true;
        }

        if (updated) {
            auto canvasDrawable = canvas_has_image ? make_canvas_drawable(canvas, kCanvasDrawableId) : std::optional<CanvasDrawable>{};
            bucket = build_bucket(canvasDrawable, strokes, &controls.bucket);
            auto revision = publish_snapshot(space, builder, scenePath, bucket);
            if (canvasDrawable && canvas.dirty) {
                auto png_bytes = encode_canvas_png(canvas);
                if (!png_bytes.empty()) {
                    auto revision_base = std::string(scenePath.getPath()) + "/builds/" + format_revision(revision);
                    auto image_path = revision_base + "/assets/images/" + fingerprint_hex(canvasDrawable->fingerprint) + ".png";
                    replace_value(space, image_path, png_bytes);
                }
                canvas.dirty = false;
            }
        }

        if (!dirtyHints.empty()) {
            std::vector<DirtyRectHint> aligned;
            aligned.reserve(dirtyHints.size());
            for (auto const& hint : dirtyHints) {
                if (auto aligned_hint = clamp_and_align_hint(hint, canvasWidth, canvasHeight, tileSizePx)) {
                    aligned.push_back(*aligned_hint);
                }
            }
            if (!aligned.empty()) {
                unwrap_or_exit(Builders::Renderer::SubmitDirtyRects(space,
                                                                    SP::ConcretePathStringView{targetAbsolutePath},
                                                                    std::span<const DirtyRectHint>{aligned}),
                               "failed to submit renderer dirty hints");
            }
        }

        if (auto outcome = present_frame(space,
                                         bootstrap.window,
                                         bootstrap.view_name,
                                         canvasWidth,
                                         canvasHeight,
                                         options.debug,
                                         options.uncapped_present_hz)) {
            if (!outcome->skipped) {
                ++fps_frames;
                if (outcome->used_iosurface) {
                    ++fps_iosurface_frames;
                }
                fps_last_stride = outcome->stride_bytes;
                fps_last_framebuffer_bytes = outcome->framebuffer_bytes;
            }
            auto report_now = std::chrono::steady_clock::now();
            auto elapsed = report_now - fps_last_report;
            if (elapsed >= std::chrono::seconds(1)) {
                double seconds = std::chrono::duration<double>(elapsed).count();
                if (seconds > 0.0 && fps_frames > 0) {
                    double fps = static_cast<double>(fps_frames) / seconds;
                    auto iosurface_frames = fps_iosurface_frames;
                    auto frames = fps_frames;
                    auto stride_bytes = fps_last_stride;
                    auto framebuffer_bytes = fps_last_framebuffer_bytes;
                    std::cout << "FPS: " << fps
                              << " (iosurface " << iosurface_frames << '/' << frames
                              << ", stride=" << stride_bytes
                              << ", frameBytes=" << framebuffer_bytes
                              << ')'
                              << std::endl;
                }
                fps_frames = 0;
                fps_iosurface_frames = 0;
                fps_last_report = report_now;
            }
        }

    }

    PaintInput::clear_mouse();
    return 0;
#endif
}
