#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/DrawCommands.hpp>

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
#include <limits>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(PATHSPACE_ENABLE_UI)
int main() {
    std::cerr << "widgets_example requires PATHSPACE_ENABLE_UI=ON.\n";
    return 1;
}
#elif !defined(__APPLE__)
int main() {
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

constexpr int kGlyphRows = 7;
constexpr float kDefaultMargin = 32.0f;
constexpr unsigned int kKeycodeTab = 0x30;        // macOS virtual key codes
constexpr unsigned int kKeycodeSpace = 0x31;
constexpr unsigned int kKeycodeReturn = 0x24;
constexpr unsigned int kKeycodeLeft = 0x7B;
constexpr unsigned int kKeycodeRight = 0x7C;
constexpr unsigned int kKeycodeDown = 0x7D;
constexpr unsigned int kKeycodeUp = 0x7E;

auto select_theme_from_env() -> Widgets::WidgetTheme {
    const char* env = std::getenv("WIDGETS_EXAMPLE_THEME");
    if (!env) {
        return Widgets::MakeDefaultWidgetTheme();
    }
    std::string value(env);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "sunset") {
        return Widgets::MakeSunsetWidgetTheme();
    }
    return Widgets::MakeDefaultWidgetTheme();
}

struct WidgetBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

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

struct ListLayout {
    WidgetBounds bounds;
    std::vector<WidgetBounds> item_bounds;
    float content_top = 0.0f;
    float item_height = 0.0f;
};

struct GalleryLayout {
    WidgetBounds button;
    WidgetBounds toggle;
    WidgetBounds slider;
    WidgetBounds slider_track;
    ListLayout list;
};

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

struct GlyphPattern {
    char ch = ' ';
    int width = 5;
    std::array<std::uint8_t, kGlyphRows> rows{};
};

constexpr std::array<GlyphPattern, 36> kGlyphPatterns = {{
    {'A', 5, {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', 5, {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111}},
    {'D', 5, {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', 5, {0b01111, 0b10000, 0b10000, 0b10011, 0b10001, 0b10001, 0b01111}},
    {'H', 5, {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', 5, {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', 5, {0b00111, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100}},
    {'K', 5, {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', 5, {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', 5, {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', 5, {0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', 5, {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', 5, {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', 5, {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', 5, {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', 5, {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', 5, {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', 5, {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', 5, {0b10010, 0b10010, 0b10010, 0b11111, 0b00010, 0b00010, 0b00010}},
    {'5', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110}},
    {'6', 5, {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', 5, {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', 5, {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110}},
}};

auto identity_transform() -> SceneData::Transform {
    SceneData::Transform t{};
    for (int i = 0; i < 16; ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
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
    return bucket;
}

auto build_list_preview(Widgets::ListStyle const& style,
                        std::vector<Widgets::ListItem> const& items,
                        Widgets::ListState const& state) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    std::size_t item_count = std::max<std::size_t>(items.size(), 1u);
    float width = std::max(style.width, 1.0f);
    float item_height = std::max(style.item_height, 1.0f);
    float border = std::max(style.border_thickness, 0.0f);
    float height = item_height * static_cast<float>(item_count) + border * 2.0f;

    auto push_drawable = [&](std::uint64_t drawable_id,
                             SceneData::BoundingBox const& box,
                             SceneData::BoundingSphere const& sphere,
                             int layer,
                             float z) {
        bucket.drawable_ids.push_back(drawable_id);
        bucket.world_transforms.push_back(identity_transform());
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);
        bucket.bounds_spheres.push_back(sphere);
        bucket.layers.push_back(layer);
        bucket.z_values.push_back(z);
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_counts.push_back(1);
        bucket.opaque_indices.push_back(static_cast<std::uint32_t>(bucket.opaque_indices.size()));
        bucket.clip_head_indices.push_back(-1);
    };

    // Background
    SceneData::BoundingBox background_box{};
    background_box.min = {0.0f, 0.0f, 0.0f};
    background_box.max = {width, height, 0.0f};

    SceneData::BoundingSphere background_sphere{};
    background_sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    background_sphere.radius = std::sqrt(background_sphere.center[0] * background_sphere.center[0]
                                         + background_sphere.center[1] * background_sphere.center[1]);

    push_drawable(0x11570001ull, background_box, background_sphere, 0, 0.0f);
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

    auto background_color = style.background_color;
    if (!state.enabled) {
        background_color = desaturate(background_color, 0.4f);
    }

    SceneData::RoundedRectCommand background{};
    background.min_x = 0.0f;
    background.min_y = 0.0f;
    background.max_x = width;
    background.max_y = height;
    background.radius_top_left = style.corner_radius;
    background.radius_top_right = style.corner_radius;
    background.radius_bottom_right = style.corner_radius;
    background.radius_bottom_left = style.corner_radius;
    background.color = background_color;

    auto payload_offset = bucket.command_payload.size();
    bucket.command_payload.resize(payload_offset + sizeof(SceneData::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + payload_offset, &background, sizeof(SceneData::RoundedRectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.back(), "widget/list/background", 0, 0});
    bucket.drawable_fingerprints.push_back(0x11570001ull);

    float content_top = border;
    for (std::size_t index = 0; index < item_count; ++index) {
        float top = content_top + item_height * static_cast<float>(index);
        float bottom = top + item_height;

        SceneData::BoundingBox row_box{};
        row_box.min = {border, top, 0.0f};
        row_box.max = {width - border, bottom, 0.0f};

        SceneData::BoundingSphere row_sphere{};
        row_sphere.center = {(row_box.min[0] + row_box.max[0]) * 0.5f,
                             (row_box.min[1] + row_box.max[1]) * 0.5f,
                             0.0f};
        row_sphere.radius = std::sqrt(std::pow(row_box.max[0] - row_sphere.center[0], 2.0f)
                                      + std::pow(row_box.max[1] - row_sphere.center[1], 2.0f));

        std::uint64_t drawable_id = 0x11570010ull + static_cast<std::uint64_t>(index);
        push_drawable(drawable_id, row_box, row_sphere, 1, 0.05f + static_cast<float>(index) * 0.001f);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));

        std::array<float, 4> color = style.item_color;
        bool item_enabled = items.size() > index ? items[index].enabled : true;
        if (!state.enabled || !item_enabled) {
            color = desaturate(color, 0.6f);
        }
        if (static_cast<int>(index) == state.selected_index) {
            color = style.item_selected_color;
        } else if (static_cast<int>(index) == state.hovered_index) {
            color = style.item_hover_color;
        }

        SceneData::RectCommand row_rect{};
        row_rect.min_x = row_box.min[0];
        row_rect.min_y = row_box.min[1];
        row_rect.max_x = row_box.max[0];
        row_rect.max_y = row_box.max[1];
        row_rect.color = color;

        auto row_payload = bucket.command_payload.size();
        bucket.command_payload.resize(row_payload + sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data() + row_payload, &row_rect, sizeof(SceneData::RectCommand));

        auto label = std::string("widget/list/item/") + std::to_string(index);
        bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
            drawable_id, label, 0, 0});
        bucket.drawable_fingerprints.push_back(drawable_id);
    }

    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
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

auto to_upper_ascii(char ch) -> char {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
}

auto uppercase_copy(std::string_view value) -> std::string {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

auto find_glyph(char ch) -> GlyphPattern const* {
    for (auto const& glyph : kGlyphPatterns) {
        if (glyph.ch == ch) {
            return &glyph;
        }
    }
    return nullptr;
}

auto glyph_scale(Widgets::TypographyStyle const& typography) -> float {
    return std::max(0.1f, typography.font_size / static_cast<float>(kGlyphRows));
}

auto measure_text_width(std::string_view text, Widgets::TypographyStyle const& typography) -> float {
    auto upper = uppercase_copy(text);
    float scale = glyph_scale(typography);
    float spacing = scale * std::max(0.0f, typography.letter_spacing);
    float width = 0.0f;
    for (char raw : upper) {
        if (raw == ' ') {
            width += scale * 4.0f + spacing;
            continue;
        }
        auto glyph = find_glyph(raw);
        if (!glyph) {
            width += scale * 4.0f + spacing;
            continue;
        }
        width += static_cast<float>(glyph->width) * scale + spacing;
    }
    if (width > 0.0f) {
        width -= spacing;
    }
    return width;
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

struct TextBuildResult {
    SceneData::DrawableBucketSnapshot bucket;
    float width = 0.0f;
    float height = 0.0f;
};

auto build_text_bucket(std::string_view text,
                       float origin_x,
                       float origin_y,
                       Widgets::TypographyStyle const& typography,
                       std::array<float, 4> color,
                       std::uint64_t drawable_id,
                       std::string authoring_id,
                       float z_value) -> std::optional<TextBuildResult> {
    std::vector<SceneData::RectCommand> commands;
    commands.reserve(text.size() * 8);

    float cursor_x = origin_x;
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    auto uppercase = uppercase_copy(text);
    float scale = glyph_scale(typography);
    float spacing = scale * std::max(0.0f, typography.letter_spacing);
    float space_advance = scale * 4.0f + spacing;

    for (char raw : uppercase) {
        if (raw == ' ') {
            cursor_x += space_advance;
            continue;
        }
        auto glyph = find_glyph(raw);
        if (!glyph) {
            cursor_x += space_advance;
            continue;
        }
        for (int row = 0; row < kGlyphRows; ++row) {
            auto mask = glyph->rows[static_cast<std::size_t>(row)];
            int col = 0;
            while (col < glyph->width) {
                bool filled = (mask & (1u << (glyph->width - 1 - col))) != 0;
                if (!filled) {
                    ++col;
                    continue;
                }
                int run_start = col;
                while (col < glyph->width && (mask & (1u << (glyph->width - 1 - col)))) {
                    ++col;
                }
                float local_min_x = cursor_x + static_cast<float>(run_start) * scale;
                float local_max_x = cursor_x + static_cast<float>(col) * scale;
                float local_min_y = origin_y + static_cast<float>(row) * scale;
                float local_max_y = local_min_y + scale;

                SceneData::RectCommand cmd{};
                cmd.min_x = local_min_x;
                cmd.max_x = local_max_x;
                cmd.min_y = local_min_y;
                cmd.max_y = local_max_y;
                cmd.color = color;
                commands.push_back(cmd);

                min_x = std::min(min_x, local_min_x);
                min_y = std::min(min_y, local_min_y);
                max_x = std::max(max_x, local_max_x);
                max_y = std::max(max_y, local_max_y);
            }
        }
        cursor_x += static_cast<float>(glyph->width) * scale + spacing;
    }

    if (commands.empty()) {
        return std::nullopt;
    }

    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(identity_transform());

    SceneData::BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    SceneData::BoundingSphere sphere{};
    sphere.center = {(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f, 0.0f};
    float dx = max_x - sphere.center[0];
    float dy = max_y - sphere.center[1];
    sphere.radius = std::sqrt(dx * dx + dy * dy);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(5);
    bucket.z_values.push_back(z_value);
    bucket.material_ids.push_back(0);
    bucket.pipeline_flags.push_back(0);
    bucket.visibility.push_back(1);
    bucket.command_offsets.push_back(0);
    bucket.command_counts.push_back(static_cast<std::uint32_t>(commands.size()));
    bucket.opaque_indices.push_back(0);
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_head_indices.push_back(-1);

    bucket.command_kinds.resize(commands.size(),
                                static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));
    bucket.command_payload.resize(commands.size() * sizeof(SceneData::RectCommand));
    std::uint8_t* payload = bucket.command_payload.data();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        std::memcpy(payload + i * sizeof(SceneData::RectCommand),
                    &commands[i],
                    sizeof(SceneData::RectCommand));
    }

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        drawable_id, std::move(authoring_id), 0, 0});
    bucket.drawable_fingerprints.push_back(drawable_id);

    TextBuildResult result{
        .bucket = std::move(bucket),
        .width = max_x - min_x,
        .height = max_y - min_y,
    };
    return result;
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
                          Widgets::WidgetTheme const& theme) -> GalleryBuildResult {
    (void)space;
    (void)appRoot;
    (void)button;
    (void)toggle;
    (void)slider;
    (void)list;
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

        float label_width = measure_text_width(button_label, button_style.typography);
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
        if (label) {
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, label_x + label->width);
            max_height = std::max(max_height, label_top + label_line_height);
        }
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
        if (label) {
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, toggle_label_x + label->width);
            max_height = std::max(max_height, toggle_label_top + toggle_label_line);
        }
        cursor_y += toggle_style.height + 40.0f;
    }

    // Slider widget with label
    {
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
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + slider_caption_line);
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
        cursor_y += slider_style.height + 48.0f;
    }

    // List widget with per-item labels
    {
        Widgets::TypographyStyle list_caption_typography = theme.caption;
        float list_caption_line = list_caption_typography.line_height;
        auto caption = build_text_bucket("Inventory",
                                         left,
                                         cursor_y + list_caption_typography.baseline_shift,
                                         list_caption_typography,
                                         theme.caption_color,
                                         next_drawable_id++,
                                         "widget/gallery/list/caption",
                                         0.6f);
        if (caption) {
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + list_caption_line);
        }
        cursor_y += list_caption_line + 12.0f;

        auto bucket = build_list_preview(list_style, list_items, list_state);
        translate_bucket(bucket, left, cursor_y);
        float list_height = list_style.item_height * static_cast<float>(std::max<std::size_t>(list_items.size(), 1u))
                            + list_style.border_thickness * 2.0f;
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + list_style.width);
        max_height = std::max(max_height, cursor_y + list_height);
        layout.list.bounds = WidgetBounds{
            left,
            cursor_y,
            left + list_style.width,
            cursor_y + list_height,
        };
        layout.list.item_height = list_style.item_height;
        layout.list.content_top = cursor_y + list_style.border_thickness;
        layout.list.item_bounds.clear();
        layout.list.item_bounds.reserve(list_items.size());

        float content_top = cursor_y + list_style.border_thickness;
        for (std::size_t index = 0; index < list_items.size(); ++index) {
            auto const& item = list_items[index];
            float row_top = content_top + list_style.item_height * static_cast<float>(index);
            float row_height = list_style.item_height;
            Widgets::TypographyStyle item_typography = list_style.item_typography;
            float text_height = item_typography.line_height;
            float text_x = left + list_style.border_thickness + 16.0f;
            float text_top = row_top + std::max(0.0f, (row_height - text_height) * 0.5f);
            auto label = build_text_bucket(item.label,
                                           text_x,
                                           text_top + item_typography.baseline_shift,
                                           item_typography,
                                           list_style.item_text_color,
                                           next_drawable_id++,
                                           "widget/gallery/list/item/" + item.id,
                                           0.65f);
            if (label) {
                pending.emplace_back(std::move(label->bucket));
                max_width = std::max(max_width, text_x + label->width);
                max_height = std::max(max_height, text_top + text_height);
            }

            float row_bottom = row_top + list_style.item_height;
            float row_min_x = left + list_style.border_thickness;
            float row_max_x = left + list_style.width - list_style.border_thickness;
            layout.list.item_bounds.push_back(WidgetBounds{
                row_min_x,
                row_top,
                row_max_x,
                row_bottom,
            });
        }
        cursor_y += list_height + 48.0f;
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
                           Widgets::WidgetTheme const& theme) -> GallerySceneResult {
    SceneParams gallery_params{
        .name = "gallery",
        .description = "widgets gallery composed scene",
    };
    auto gallery_scene = unwrap_or_exit(SceneBuilders::Create(space, appRoot, gallery_params),
                                        "create gallery scene");

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
                                      theme);

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
};

struct WidgetsExampleContext {
    PathSpace* space = nullptr;
    SP::App::AppRootPath app_root{std::string{}};
    Widgets::ButtonPaths button_paths;
    Widgets::TogglePaths toggle_paths;
    Widgets::SliderPaths slider_paths;
    Widgets::ListPaths list_paths;
    Widgets::WidgetTheme theme{};
    Widgets::ButtonStyle button_style{};
    std::string button_label;
    Widgets::ToggleStyle toggle_style{};
    Widgets::SliderStyle slider_style{};
    Widgets::ListStyle list_style{};
    Widgets::SliderRange slider_range{};
    std::vector<Widgets::ListItem> list_items;
    WidgetBindings::ButtonBinding button_binding{};
    WidgetBindings::ToggleBinding toggle_binding{};
    WidgetBindings::SliderBinding slider_binding{};
    WidgetBindings::ListBinding list_binding{};
    Widgets::ButtonState button_state{};
    Widgets::ToggleState toggle_state{};
    Widgets::SliderState slider_state{};
    Widgets::ListState list_state{};
    GallerySceneResult gallery{};
    std::string target_path;
    bool pointer_down = false;
    bool slider_dragging = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    FocusTarget focus_target = FocusTarget::Button;
    int focus_list_index = 0;
};

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

static auto slider_keyboard_step(WidgetsExampleContext const& ctx) -> float {
    float step = ctx.slider_range.step;
    if (step <= 0.0f) {
        float range = ctx.slider_range.maximum - ctx.slider_range.minimum;
        step = std::max(range * 0.05f, 0.01f);
    }
    return step;
}

static bool apply_focus_visuals(WidgetsExampleContext& ctx) {
    bool changed = false;
    auto update_button = [&](bool hovered) {
        if (ctx.button_state.hovered == hovered) {
            return;
        }
        Widgets::ButtonState desired = ctx.button_state;
        desired.hovered = hovered;
        auto status = Widgets::UpdateButtonState(*ctx.space, ctx.button_paths, desired);
        if (!status) {
            std::cerr << "widgets_example: failed to update button focus state: "
                      << status.error().message.value_or("unknown error") << "\n";
            return;
        }
        ctx.button_state = desired;
        changed |= *status;
    };

    auto update_toggle = [&](bool hovered) {
        if (ctx.toggle_state.hovered == hovered) {
            return;
        }
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.hovered = hovered;
        auto status = Widgets::UpdateToggleState(*ctx.space, ctx.toggle_paths, desired);
        if (!status) {
            std::cerr << "widgets_example: failed to update toggle focus state: "
                      << status.error().message.value_or("unknown error") << "\n";
            return;
        }
        ctx.toggle_state = desired;
        changed |= *status;
    };

    auto update_slider = [&](bool hovered) {
        if (ctx.slider_state.hovered == hovered) {
            return;
        }
        Widgets::SliderState desired = ctx.slider_state;
        desired.hovered = hovered;
        auto status = Widgets::UpdateSliderState(*ctx.space, ctx.slider_paths, desired);
        if (!status) {
            std::cerr << "widgets_example: failed to update slider focus state: "
                      << status.error().message.value_or("unknown error") << "\n";
            return;
        }
        ctx.slider_state = desired;
        changed |= *status;
    };

    auto update_list = [&](bool focused) {
        int desired_hover = -1;
        if (focused && !ctx.gallery.layout.list.item_bounds.empty()) {
            int max_index = static_cast<int>(ctx.gallery.layout.list.item_bounds.size()) - 1;
            if (ctx.focus_list_index < 0) {
                ctx.focus_list_index = std::clamp(ctx.list_state.selected_index, 0, max_index);
            }
            ctx.focus_list_index = std::clamp(ctx.focus_list_index, 0, max_index);
            desired_hover = ctx.focus_list_index;
        }
        if (ctx.list_state.hovered_index == desired_hover) {
            return;
        }
        Widgets::ListState desired = ctx.list_state;
        desired.hovered_index = desired_hover;
        auto status = Widgets::UpdateListState(*ctx.space, ctx.list_paths, desired);
        if (!status) {
            std::cerr << "widgets_example: failed to update list focus state: "
                      << status.error().message.value_or("unknown error") << "\n";
            return;
        }
        ctx.list_state = desired;
        changed |= *status;
    };

    update_button(ctx.focus_target == FocusTarget::Button);
    update_toggle(ctx.focus_target == FocusTarget::Toggle);
    update_slider(ctx.focus_target == FocusTarget::Slider);
    update_list(ctx.focus_target == FocusTarget::List);
    return changed;
}

static void set_focus_target(WidgetsExampleContext& ctx, FocusTarget target) {
    if (ctx.focus_target == target) {
        if (apply_focus_visuals(ctx)) {
            refresh_gallery(ctx);
        }
        return;
    }
    ctx.focus_target = target;
    if (ctx.focus_target == FocusTarget::List && ctx.list_state.selected_index >= 0) {
        ctx.focus_list_index = ctx.list_state.selected_index;
    }
    if (apply_focus_visuals(ctx)) {
        refresh_gallery(ctx);
    }
}

static void cycle_focus(WidgetsExampleContext& ctx, bool forward) {
    constexpr FocusTarget order[] = {
        FocusTarget::Button,
        FocusTarget::Toggle,
        FocusTarget::Slider,
        FocusTarget::List,
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
    set_focus_target(ctx, order[static_cast<std::size_t>(next)]);
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
    ctx.gallery = publish_gallery_scene(*ctx.space,
                                        view,
                                        ctx.button_paths,
                                        ctx.button_style,
                                        ctx.button_state,
                                        ctx.button_label,
                                        ctx.toggle_paths,
                                        ctx.toggle_style,
                                        ctx.toggle_state,
                                        ctx.slider_paths,
                                        ctx.slider_style,
                                        ctx.slider_state,
                                        ctx.slider_range,
                                        ctx.list_paths,
                                        ctx.list_style,
                                        ctx.list_state,
                                        ctx.list_items,
                                        ctx.theme);
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
        ctx.button_state = desired;
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
        ctx.toggle_state = desired;
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
        } else {
            ctx.list_state = desired;
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

    if (ctx.gallery.layout.button.contains(ctx.pointer_x, ctx.pointer_y)) {
        ctx.focus_target = FocusTarget::Button;
        Widgets::ButtonState desired = ctx.button_state;
        desired.hovered = true;
        desired.pressed = true;
        changed |= dispatch_button(ctx, desired, WidgetBindings::WidgetOpKind::Press, true);
    }

    if (ctx.gallery.layout.toggle.contains(ctx.pointer_x, ctx.pointer_y)) {
        ctx.focus_target = FocusTarget::Toggle;
        Widgets::ToggleState desired = ctx.toggle_state;
        desired.hovered = true;
        changed |= dispatch_toggle(ctx, desired, WidgetBindings::WidgetOpKind::Press, true);
    }

    if (ctx.gallery.layout.slider.contains(ctx.pointer_x, ctx.pointer_y)) {
        ctx.slider_dragging = true;
        ctx.focus_target = FocusTarget::Slider;
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
        ctx.focus_target = FocusTarget::List;
        ctx.focus_list_index = index;
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
        ctx.focus_target = FocusTarget::List;
        ctx.focus_list_index = index;
    }

    ctx.pointer_down = false;

    if (changed) {
        refresh_gallery(ctx);
    }
}

static void handle_pointer_wheel(WidgetsExampleContext& ctx, int wheel_delta) {
    if (wheel_delta == 0) {
        return;
    }
    if (!ctx.gallery.layout.list.bounds.contains(ctx.pointer_x, ctx.pointer_y)) {
        return;
    }
    float scroll_pixels = static_cast<float>(-wheel_delta) * (ctx.gallery.layout.list.item_height * 0.25f);
    Widgets::ListState desired = ctx.list_state;
    bool changed = dispatch_list(ctx,
                                 desired,
                                 WidgetBindings::WidgetOpKind::ListScroll,
                                 true,
                                 ctx.list_state.hovered_index,
                                 scroll_pixels);
    if (changed) {
        refresh_gallery(ctx);
    }
}

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
        } else {
            cycle_focus(*ctx, false);
        }
        break;
    case kKeycodeUp:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, -1);
        } else {
            cycle_focus(*ctx, false);
        }
        break;
    case kKeycodeRight:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, 1);
        } else {
            cycle_focus(*ctx, true);
        }
        break;
    case kKeycodeDown:
        if (ctx->focus_target == FocusTarget::Slider) {
            adjust_slider_value(*ctx, -slider_keyboard_step(*ctx));
        } else if (ctx->focus_target == FocusTarget::List) {
            move_list_focus(*ctx, 1);
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
    telemetry.framebuffer_bytes = present_result->framebuffer.size();

#if defined(__APPLE__)
    if (!telemetry.skipped
        && present_result->stats.iosurface
        && present_result->stats.iosurface->valid()) {
        auto iosurface_ref = present_result->stats.iosurface->retain_for_external_use();
        if (iosurface_ref) {
            SP::UI::PresentLocalWindowIOSurface(static_cast<void*>(iosurface_ref),
                                                width,
                                                height,
                                                static_cast<int>(present_result->stats.iosurface->row_bytes()));
            telemetry.used_iosurface = true;
            telemetry.presented = true;
            telemetry.stride_bytes = present_result->stats.iosurface->row_bytes();
            CFRelease(iosurface_ref);
        }
    }
#endif

    if (!telemetry.skipped && !telemetry.used_iosurface) {
        if (!present_result->framebuffer.empty()) {
            int row_stride_bytes = 0;
            if (height > 0) {
                auto rows = static_cast<std::size_t>(height);
                row_stride_bytes = static_cast<int>(present_result->framebuffer.size() / rows);
            }
            if (row_stride_bytes <= 0) {
                row_stride_bytes = width * 4;
            }
            telemetry.stride_bytes = static_cast<std::size_t>(row_stride_bytes);
            SP::UI::PresentLocalWindowFramebuffer(present_result->framebuffer.data(),
                                                  width,
                                                  height,
                                                  row_stride_bytes);
            telemetry.presented = true;
        } else if (present_result->stats.used_metal_texture) {
            static bool warned = false;
            if (!warned) {
                std::cerr << "warning: Metal texture presented without IOSurface fallback; "
                             "widgets_example cannot display the frame buffer.\n";
                warned = true;
            }
        }
    }

    return telemetry;
}

} // namespace

int main() {
    using namespace SP;
    using namespace SP::UI::Builders;
    PathSpace space;
    AppRootPath appRoot{"/system/applications/widgets_example"};
    AppRootPathView appRootView{appRoot.getPath()};

    auto theme = select_theme_from_env();

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

    bool headless = false;
    if (const char* headless_env = std::getenv("WIDGETS_EXAMPLE_HEADLESS")) {
        if (headless_env[0] != '\0' && headless_env[0] != '0') {
            headless = true;
        }
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
                                         list_params.style,
                                         list_state_live,
                                         list_params.items,
                                         theme);

    std::cout << "widgets_example gallery scene:\n"
              << "  scene: " << gallery.scene.getPath() << "\n"
              << "  size : " << gallery.width << "x" << gallery.height << " pixels\n";

    if (headless && !trace.replaying()) {
        std::cout << "widgets_example exiting headless mode (WIDGETS_EXAMPLE_HEADLESS set).\n";
        trace.flush();
        return 0;
    }

    RendererParams renderer_params{
        .name = "gallery_renderer",
        .kind = RendererKind::Software2D,
        .description = "widgets gallery renderer",
    };
    auto renderer = unwrap_or_exit(Renderer::Create(space, appRootView, renderer_params),
                                   "create gallery renderer");

    SurfaceDesc surface_desc{};
    surface_desc.size_px.width = gallery.width;
    surface_desc.size_px.height = gallery.height;
    surface_desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surface_desc.color_space = ColorSpace::sRGB;
    surface_desc.premultiplied_alpha = true;

    SurfaceParams surface_params{
        .name = "gallery_surface",
        .desc = surface_desc,
        .renderer = renderer.getPath(),
    };
    auto surface = unwrap_or_exit(Surface::Create(space, appRootView, surface_params),
                                  "create gallery surface");
    unwrap_or_exit(Surface::SetScene(space, surface, gallery.scene),
                   "bind gallery scene to surface");

    auto target_relative = unwrap_or_exit(space.read<std::string, std::string>(
                                              std::string(surface.getPath()) + "/target"),
                                          "read surface target binding");
    auto target_absolute = unwrap_or_exit(SP::App::resolve_app_relative(appRootView, target_relative),
                                          "resolve surface target path");

    WidgetsExampleContext ctx{};
    ctx.space = &space;
    ctx.app_root = appRoot;
    ctx.button_paths = button;
    ctx.toggle_paths = toggle;
    ctx.slider_paths = slider;
    ctx.list_paths = list;
    ctx.theme = theme;
    ctx.button_style = button_params.style;
    ctx.button_label = button_params.label;
    ctx.toggle_style = toggle_params.style;
    ctx.slider_style = slider_params.style;
    ctx.list_style = list_params.style;
    ctx.slider_range = slider_range_live;
    ctx.list_items = list_params.items;
    ctx.button_state = button_state_live;
    ctx.toggle_state = toggle_state_live;
    ctx.slider_state = slider_state_live;
    ctx.list_state = list_state_live;
    ctx.gallery = gallery;
    ctx.target_path = target_absolute.getPath();

    auto target_view = SP::ConcretePathStringView{ctx.target_path};
    auto make_dirty_hint = [](WidgetBounds const& bounds) {
        Builders::DirtyRectHint hint{};
        hint.min_x = bounds.min_x;
        hint.min_y = bounds.min_y;
        hint.max_x = bounds.max_x;
        hint.max_y = bounds.max_y;
        return hint;
    };

    ctx.button_binding = unwrap_or_exit(WidgetBindings::CreateButtonBinding(space,
                                                                            appRootView,
                                                                            ctx.button_paths,
                                                                            target_view,
                                                                            make_dirty_hint(ctx.gallery.layout.button)),
                                        "create button binding");

    ctx.toggle_binding = unwrap_or_exit(WidgetBindings::CreateToggleBinding(space,
                                                                            appRootView,
                                                                            ctx.toggle_paths,
                                                                            target_view,
                                                                            make_dirty_hint(ctx.gallery.layout.toggle)),
                                        "create toggle binding");

    ctx.slider_binding = unwrap_or_exit(WidgetBindings::CreateSliderBinding(space,
                                                                            appRootView,
                                                                            ctx.slider_paths,
                                                                            target_view,
                                                                            make_dirty_hint(ctx.gallery.layout.slider)),
                                        "create slider binding");

    ctx.list_binding = unwrap_or_exit(WidgetBindings::CreateListBinding(space,
                                                                        appRootView,
                                                                        ctx.list_paths,
                                                                        target_view,
                                                                        make_dirty_hint(ctx.gallery.layout.list.bounds)),
                                      "create list binding");

    set_focus_target(ctx, FocusTarget::Button);

    if (trace.replaying()) {
        run_replay_session(ctx, trace.events());
        trace.flush();
        return 0;
    }

    std::cout << "widgets_example keyboard controls:\n"
              << "  Tab / Shift+Tab : cycle widget focus\n"
              << "  Arrow keys       : adjust focused slider or list selection\n"
              << "  Space / Return   : activate the focused widget\n";

    SP::UI::SetLocalWindowCallbacks({&handle_local_mouse, &clear_local_mouse, &ctx, &handle_local_keyboard});
    SP::UI::InitLocalWindowWithSize(ctx.gallery.width,
                                    ctx.gallery.height,
                                    "PathSpace Widgets Gallery");

    WindowParams window_params{
        .name = "gallery_window",
        .title = "PathSpace Widgets Gallery",
        .width = ctx.gallery.width,
        .height = ctx.gallery.height,
        .scale = 1.0f,
        .background = "#1f232b",
    };
    auto window = unwrap_or_exit(Window::Create(space, appRootView, window_params),
                                 "create gallery window");
    unwrap_or_exit(Window::AttachSurface(space, window, "main", surface),
                   "attach surface to window");

    std::string view_base = std::string(window.getPath()) + "/views/main";
    replace_value(space, view_base + "/present/policy", std::string{"AlwaysLatestComplete"});
    replace_value(space, view_base + "/present/params/vsync_align", false);
    replace_value(space, view_base + "/present/params/frame_timeout_ms", 0.0);
    replace_value(space, view_base + "/present/params/staleness_budget_ms", 0.0);
    replace_value(space, view_base + "/present/params/max_age_frames", static_cast<std::uint64_t>(0));

    RenderSettings renderer_settings{};
    renderer_settings.clear_color = {0.11f, 0.12f, 0.15f, 1.0f};
    renderer_settings.surface.size_px.width = ctx.gallery.width;
    renderer_settings.surface.size_px.height = ctx.gallery.height;
    unwrap_or_exit(Renderer::UpdateSettings(space,
                                            ConcretePathStringView{target_absolute.getPath()},
                                            renderer_settings),
                   "update renderer settings");

    Builders::DirtyRectHint initial_hint{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(ctx.gallery.width),
        .max_y = static_cast<float>(ctx.gallery.height),
    };
    unwrap_or_exit(Renderer::SubmitDirtyRects(space,
                                              ConcretePathStringView{target_absolute.getPath()},
                                              std::span<const Builders::DirtyRectHint>{&initial_hint, 1}),
                   "submit initial dirty rect");

    std::string surface_desc_path = std::string(surface.getPath()) + "/desc";
    std::string target_desc_path = target_absolute.getPath() + "/desc";

    auto last_report = std::chrono::steady_clock::now();
    std::uint64_t frames_presented = 0;
    double total_render_ms = 0.0;
    double total_present_ms = 0.0;
    int window_width = ctx.gallery.width;
    int window_height = ctx.gallery.height;

    while (true) {
        SP::UI::PollLocalWindow();

        int requested_width = window_width;
        int requested_height = window_height;
        SP::UI::GetLocalWindowContentSize(&requested_width, &requested_height);
        if (requested_width <= 0 || requested_height <= 0) {
            break;
        }

        if (requested_width != window_width || requested_height != window_height) {
            window_width = requested_width;
            window_height = requested_height;
            surface_desc.size_px.width = window_width;
            surface_desc.size_px.height = window_height;
            replace_value(space, surface_desc_path, surface_desc);
            replace_value(space, target_desc_path, surface_desc);
            renderer_settings.surface.size_px.width = window_width;
            renderer_settings.surface.size_px.height = window_height;
            unwrap_or_exit(Renderer::UpdateSettings(space,
                                                    ConcretePathStringView{target_absolute.getPath()},
                                                    renderer_settings),
                           "refresh renderer settings after resize");
            Builders::DirtyRectHint hint{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = static_cast<float>(window_width),
                .max_y = static_cast<float>(window_height),
            };
            unwrap_or_exit(Renderer::SubmitDirtyRects(space,
                                                      ConcretePathStringView{target_absolute.getPath()},
                                                      std::span<const Builders::DirtyRectHint>{&hint, 1}),
                           "submit resize dirty rect");
        }

        auto telemetry = present_frame(space, window, "main", window_width, window_height);
        if (telemetry && telemetry->presented && !telemetry->skipped) {
            ++frames_presented;
            total_render_ms += telemetry->render_ms;
            total_present_ms += telemetry->present_ms;
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

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    trace.flush();
    return 0;
}

#endif // PATHSPACE_ENABLE_UI
