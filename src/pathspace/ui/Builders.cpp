#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include "DrawableUtils.hpp"

#include "core/Out.hpp"
#include "path/UnvalidatedPath.hpp"
#include "task/IFutureAny.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI::Builders {

namespace {

constexpr std::string_view kScenesSegment = "/scenes/";
constexpr std::string_view kRenderersSegment = "/renderers/";
constexpr std::string_view kSurfacesSegment = "/surfaces/";
constexpr std::string_view kWindowsSegment = "/windows/";
constexpr std::string_view kWidgetAuthoringMarker = "/authoring/";
std::atomic<std::uint64_t> g_auto_render_sequence{0};
std::atomic<std::uint64_t> g_scene_dirty_sequence{0};
std::atomic<std::uint64_t> g_widget_op_sequence{0};

struct SceneRevisionRecord {
    uint64_t    revision = 0;
    int64_t     published_at_ms = 0;
    std::string author;
};

auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

namespace SceneData = SP::UI::Scene;

template <typename T>
auto replace_single(PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void>;

auto combine_relative(AppRootPathView root, std::string spec) -> SP::Expected<ConcretePath>;

auto make_scene_meta(ScenePath const& scenePath, std::string const& leaf) -> std::string;

template <typename T>
auto read_optional(PathSpace const& space, std::string const& path) -> SP::Expected<std::optional<T>>;

auto button_states_equal(Widgets::ButtonState const& lhs,
                         Widgets::ButtonState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.pressed == rhs.pressed
        && lhs.hovered == rhs.hovered;
}

auto toggle_states_equal(Widgets::ToggleState const& lhs,
                         Widgets::ToggleState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.hovered == rhs.hovered
        && lhs.checked == rhs.checked;
}

auto slider_states_equal(Widgets::SliderState const& lhs,
                         Widgets::SliderState const& rhs) -> bool {
    return lhs.enabled == rhs.enabled
        && lhs.hovered == rhs.hovered
        && lhs.dragging == rhs.dragging
        && lhs.value == rhs.value;
}

auto list_states_equal(Widgets::ListState const& lhs,
                       Widgets::ListState const& rhs) -> bool {
    auto equal_float = [](float a, float b) {
        return std::fabs(a - b) <= 1e-6f;
    };
    return lhs.enabled == rhs.enabled
        && lhs.hovered_index == rhs.hovered_index
        && lhs.selected_index == rhs.selected_index
        && equal_float(lhs.scroll_offset, rhs.scroll_offset);
}

auto make_default_dirty_rect(float width, float height) -> DirtyRectHint {
    DirtyRectHint hint{};
    hint.min_x = 0.0f;
    hint.min_y = 0.0f;
    hint.max_x = std::max(width, 1.0f);
    hint.max_y = std::max(height, 1.0f);
    return hint;
}

auto ensure_valid_hint(DirtyRectHint hint) -> DirtyRectHint {
    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return DirtyRectHint{0.0f, 0.0f, 0.0f, 0.0f};
    }
    return hint;
}

auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

auto mix_color(std::array<float, 4> base,
               std::array<float, 4> target,
               float amount) -> std::array<float, 4> {
    amount = clamp_unit(amount);
    std::array<float, 4> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = clamp_unit(base[i] * (1.0f - amount) + target[i] * amount);
    }
    out[3] = clamp_unit(base[3] * (1.0f - amount) + target[3] * amount);
    return out;
}

auto lighten_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

auto darken_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {0.0f, 0.0f, 0.0f, color[3]}, amount);
}

auto desaturate_color(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    auto gray = std::array<float, 4>{0.5f, 0.5f, 0.5f, color[3]};
    return mix_color(color, gray, amount);
}

auto scale_alpha(std::array<float, 4> color, float factor) -> std::array<float, 4> {
    color[3] = clamp_unit(color[3] * factor);
    return color;
}

auto make_identity_transform() -> SceneData::Transform {
    SceneData::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto make_widget_authoring_id(std::string_view base_path,
                               std::string_view suffix) -> std::string {
    if (base_path.empty()) {
        std::string id = "widget/";
        id.append(suffix);
        return id;
    }
    std::string id;
    id.reserve(base_path.size() + std::size_t{11} + suffix.size());
    id.append(base_path);
    if (!base_path.empty() && base_path.back() != '/') {
        id.push_back('/');
    }
    id.append("authoring/");
    id.append(suffix);
    return id;
}

struct ButtonSnapshotConfig {
    float width = 200.0f;
    float height = 48.0f;
    float corner_radius = 6.0f;
    std::array<float, 4> color{0.176f, 0.353f, 0.914f, 1.0f};
};

auto make_button_bucket(ButtonSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xB17B0001ull};
    bucket.world_transforms = {make_identity_transform()};

    SceneData::BoundingSphere sphere{};
    float center_x = config.width * 0.5f;
    float center_y = config.height * 0.5f;
    sphere.center = {center_x, center_y, 0.0f};
    sphere.radius = std::sqrt(center_x * center_x + center_y * center_y);
    bucket.bounds_spheres = {sphere};

    SceneData::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {config.width, config.height, 0.0f};
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
        bucket.drawable_ids.front(),
        make_widget_authoring_id(authoring_root, "button/background"),
        0,
        0}};
    bucket.drawable_fingerprints = {0xB17B0001ull};

    float radius_limit = std::min(config.width, config.height) * 0.5f;
    float clamped_radius = std::clamp(config.corner_radius, 0.0f, radius_limit);

    if (clamped_radius > 0.0f) {
        SceneData::RoundedRectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = config.width;
        rect.max_y = config.height;
        rect.radius_top_left = clamped_radius;
        rect.radius_top_right = clamped_radius;
        rect.radius_bottom_left = clamped_radius;
        rect.radius_bottom_right = clamped_radius;
        rect.color = config.color;

        bucket.command_payload.resize(sizeof(SceneData::RoundedRectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RoundedRectCommand));
        bucket.command_kinds = {
            static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect),
        };
    } else {
        SceneData::RectCommand rect{};
        rect.min_x = 0.0f;
        rect.min_y = 0.0f;
        rect.max_x = config.width;
        rect.max_y = config.height;
        rect.color = config.color;

        bucket.command_payload.resize(sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RectCommand));
        bucket.command_kinds = {
            static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect),
        };
    }

    return bucket;
}

struct ToggleSnapshotConfig {
    float width = 56.0f;
    float height = 32.0f;
    bool checked = false;
    std::array<float, 4> track_off_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> track_on_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

auto make_toggle_bucket(ToggleSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x701701u, 0x701702u};
    bucket.world_transforms = {make_identity_transform(), make_identity_transform()};

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {config.width * 0.5f, config.height * 0.5f, 0.0f};
    trackSphere.radius = std::sqrt(trackSphere.center[0] * trackSphere.center[0]
                                   + trackSphere.center[1] * trackSphere.center[1]);

    SceneData::BoundingSphere thumbSphere{};
    float thumbRadius = config.height * 0.5f - 2.0f;
    float thumbCenterX = config.checked ? (config.width - thumbRadius - 2.0f) : (thumbRadius + 2.0f);
    thumbSphere.center = {thumbCenterX, config.height * 0.5f, 0.0f};
    thumbSphere.radius = thumbRadius;

    bucket.bounds_spheres = {trackSphere, thumbSphere};

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, 0.0f, 0.0f};
    trackBox.max = {config.width, config.height, 0.0f};

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumbCenterX - thumbRadius, config.height * 0.5f - thumbRadius, 0.0f};
    thumbBox.max = {thumbCenterX + thumbRadius, config.height * 0.5f + thumbRadius, 0.0f};

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
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[0],
            make_widget_authoring_id(authoring_root, "toggle/track"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[1],
            make_widget_authoring_id(authoring_root, "toggle/thumb"),
            0,
            0},
    };
    bucket.drawable_fingerprints = {0x701701u, 0x701702u};

    auto trackColor = config.checked ? config.track_on_color : config.track_off_color;

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = 0.0f;
    trackRect.max_x = config.width;
    trackRect.max_y = config.height;
    trackRect.radius_top_left = config.height * 0.5f;
    trackRect.radius_top_right = config.height * 0.5f;
    trackRect.radius_bottom_right = config.height * 0.5f;
    trackRect.radius_bottom_left = config.height * 0.5f;
    trackRect.color = trackColor;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumbRadius;
    thumbRect.radius_top_right = thumbRadius;
    thumbRect.radius_bottom_right = thumbRadius;
    thumbRect.radius_bottom_left = thumbRadius;
    thumbRect.color = config.thumb_color;

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

auto ensure_widget_root(PathSpace& /*space*/,
                        AppRootPathView appRoot) -> SP::Expected<ConcretePath> {
    return combine_relative(appRoot, "widgets");
}

struct SliderSnapshotConfig {
    float width = 240.0f;
    float height = 32.0f;
    float track_height = 6.0f;
    float thumb_radius = 10.0f;
    float min = 0.0f;
    float max = 1.0f;
    float value = 0.5f;
    std::array<float, 4> track_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> fill_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

auto make_slider_bucket(SliderSnapshotConfig const& config,
                        std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x51D301u, 0x51D302u, 0x51D303u};
    bucket.world_transforms = {make_identity_transform(), make_identity_transform(), make_identity_transform()};

    float const clamped_min = std::min(config.min, config.max);
    float const clamped_max = std::max(config.min, config.max);
    float const range = std::max(clamped_max - clamped_min, 1e-6f);
    float const clamped_value = std::clamp(config.value, clamped_min, clamped_max);
    float progress = (clamped_value - clamped_min) / range;
    progress = std::clamp(progress, 0.0f, 1.0f);

    float const width = std::max(config.width, 1.0f);
    float const height = std::max(config.height, 1.0f);
    float const track_height = std::clamp(config.track_height, 1.0f, height);
    float const thumb_radius = std::clamp(config.thumb_radius, track_height * 0.5f, height * 0.5f);

    float const center_y = height * 0.5f;
    float const track_half = track_height * 0.5f;
    float const track_radius = track_half;
    float const fill_width = std::max(progress * width, 0.0f);
    float thumb_x = progress * width;
    thumb_x = std::clamp(thumb_x, thumb_radius, width - thumb_radius);

    SceneData::BoundingSphere trackSphere{};
    trackSphere.center = {width * 0.5f, center_y, 0.0f};
    trackSphere.radius = std::sqrt(trackSphere.center[0] * trackSphere.center[0]
                                   + track_half * track_half);

    SceneData::BoundingSphere fillSphere{};
    fillSphere.center = {std::max(fill_width * 0.5f, 0.0f), center_y, 0.0f};
    fillSphere.radius = std::sqrt(fillSphere.center[0] * fillSphere.center[0]
                                  + track_half * track_half);

    SceneData::BoundingSphere thumbSphere{};
    thumbSphere.center = {thumb_x, center_y, 0.0f};
    thumbSphere.radius = thumb_radius;

    bucket.bounds_spheres = {trackSphere, fillSphere, thumbSphere};

    SceneData::BoundingBox trackBox{};
    trackBox.min = {0.0f, center_y - track_half, 0.0f};
    trackBox.max = {width, center_y + track_half, 0.0f};

    SceneData::BoundingBox fillBox{};
    fillBox.min = {0.0f, center_y - track_half, 0.0f};
    fillBox.max = {fill_width, center_y + track_half, 0.0f};

    SceneData::BoundingBox thumbBox{};
    thumbBox.min = {thumb_x - thumb_radius, center_y - thumb_radius, 0.0f};
    thumbBox.max = {thumb_x + thumb_radius, center_y + thumb_radius, 0.0f};

    bucket.bounds_boxes = {trackBox, fillBox, thumbBox};
    bucket.bounds_box_valid = {1, 1, 1};
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
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[0],
            make_widget_authoring_id(authoring_root, "slider/track"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[1],
            make_widget_authoring_id(authoring_root, "slider/fill"),
            0,
            0},
        SceneData::DrawableAuthoringMapEntry{
            bucket.drawable_ids[2],
            make_widget_authoring_id(authoring_root, "slider/thumb"),
            0,
            0},
    };
    bucket.drawable_fingerprints = {0x51D301u, 0x51D302u, 0x51D303u};

    SceneData::RoundedRectCommand trackRect{};
    trackRect.min_x = 0.0f;
    trackRect.min_y = center_y - track_half;
    trackRect.max_x = width;
    trackRect.max_y = center_y + track_half;
    trackRect.radius_top_left = track_radius;
    trackRect.radius_top_right = track_radius;
    trackRect.radius_bottom_right = track_radius;
    trackRect.radius_bottom_left = track_radius;
    trackRect.color = config.track_color;

    SceneData::RectCommand fillRect{};
    fillRect.min_x = 0.0f;
    fillRect.min_y = center_y - track_half;
    fillRect.max_x = fill_width;
    fillRect.max_y = center_y + track_half;
    fillRect.color = config.fill_color;

    SceneData::RoundedRectCommand thumbRect{};
    thumbRect.min_x = thumbBox.min[0];
    thumbRect.min_y = thumbBox.min[1];
    thumbRect.max_x = thumbBox.max[0];
    thumbRect.max_y = thumbBox.max[1];
    thumbRect.radius_top_left = thumb_radius;
    thumbRect.radius_top_right = thumb_radius;
    thumbRect.radius_bottom_right = thumb_radius;
    thumbRect.radius_bottom_left = thumb_radius;
    thumbRect.color = config.thumb_color;

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

struct ListSnapshotConfig {
    float width = 240.0f;
    float item_height = 36.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    std::size_t item_count = 0;
    std::int32_t selected_index = -1;
    std::int32_t hovered_index = -1;
    std::array<float, 4> background_color{};
    std::array<float, 4> border_color{};
    std::array<float, 4> item_color{};
    std::array<float, 4> item_hover_color{};
    std::array<float, 4> item_selected_color{};
    std::array<float, 4> separator_color{};
};

auto make_list_bucket(ListSnapshotConfig const& config,
                      std::string_view authoring_root = {}) -> SceneData::DrawableBucketSnapshot {
    auto const item_count = static_cast<std::size_t>(std::max<std::int32_t>(static_cast<std::int32_t>(config.item_count), 0));
    float const base_height = std::max(config.item_height * static_cast<float>(std::max<std::size_t>(item_count, 1u)),
                                       config.item_height);
    float const height = base_height + config.border_thickness * 2.0f;
    float const width = std::max(config.width, 1.0f);

    SceneData::DrawableBucketSnapshot bucket{};
    // background + per-item rectangles
    std::size_t drawable_count = 1 + std::max<std::size_t>(item_count, 1u);
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
    bucket.opaque_indices.reserve(drawable_count);
    bucket.clip_head_indices.reserve(drawable_count);
    bucket.authoring_map.reserve(drawable_count);
    bucket.drawable_fingerprints.reserve(drawable_count);

    auto push_common = [&](std::uint64_t drawable_id,
                           SceneData::BoundingBox const& box,
                           SceneData::BoundingSphere const& sphere,
                           int layer,
                           float z) {
        bucket.drawable_ids.push_back(drawable_id);
        bucket.world_transforms.push_back(make_identity_transform());
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

    // Background rounded rect.
    SceneData::BoundingBox background_box{};
    background_box.min = {0.0f, 0.0f, 0.0f};
    background_box.max = {width, height, 0.0f};

    SceneData::BoundingSphere background_sphere{};
    background_sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    background_sphere.radius = std::sqrt(background_sphere.center[0] * background_sphere.center[0]
                                         + background_sphere.center[1] * background_sphere.center[1]);

    push_common(0x11570001ull, background_box, background_sphere, 0, 0.0f);
    bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::RoundedRect));

    SceneData::RoundedRectCommand background{};
    background.min_x = 0.0f;
    background.min_y = 0.0f;
    background.max_x = width;
    background.max_y = height;
    background.radius_top_left = config.corner_radius;
    background.radius_top_right = config.corner_radius;
    background.radius_bottom_right = config.corner_radius;
    background.radius_bottom_left = config.corner_radius;
    background.color = config.background_color;

    auto const background_offset = bucket.command_payload.size();
    bucket.command_payload.resize(background_offset + sizeof(SceneData::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + background_offset,
                &background,
                sizeof(SceneData::RoundedRectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        bucket.drawable_ids.back(),
        make_widget_authoring_id(authoring_root, "list/background"),
        0,
        0});
    bucket.drawable_fingerprints.push_back(0x11570001ull);

    // Item rows.
    std::size_t const rows = std::max<std::size_t>(item_count, 1u);
    float const content_top = config.border_thickness;
    for (std::size_t index = 0; index < rows; ++index) {
        float const top = content_top + config.item_height * static_cast<float>(index);
        float const bottom = top + config.item_height;
        SceneData::BoundingBox row_box{};
        row_box.min = {config.border_thickness, top, 0.0f};
        row_box.max = {width - config.border_thickness, bottom, 0.0f};

        SceneData::BoundingSphere row_sphere{};
        row_sphere.center = {(row_box.min[0] + row_box.max[0]) * 0.5f,
                             (row_box.min[1] + row_box.max[1]) * 0.5f,
                             0.0f};
        row_sphere.radius = std::sqrt(std::pow(row_box.max[0] - row_sphere.center[0], 2.0f)
                                      + std::pow(row_box.max[1] - row_sphere.center[1], 2.0f));

        std::uint64_t drawable_id = 0x11570010ull + static_cast<std::uint64_t>(index);
        push_common(drawable_id, row_box, row_sphere, 1, 0.05f + static_cast<float>(index) * 0.001f);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));

        std::array<float, 4> color = config.item_color;
        if (static_cast<std::int32_t>(index) == config.selected_index) {
            color = config.item_selected_color;
        } else if (static_cast<std::int32_t>(index) == config.hovered_index) {
            color = config.item_hover_color;
        }

        SceneData::RectCommand row_rect{};
        row_rect.min_x = row_box.min[0];
        row_rect.min_y = row_box.min[1];
        row_rect.max_x = row_box.max[0];
        row_rect.max_y = row_box.max[1];
        row_rect.color = color;

        auto const payload_offset = bucket.command_payload.size();
        bucket.command_payload.resize(payload_offset + sizeof(SceneData::RectCommand));
        std::memcpy(bucket.command_payload.data() + payload_offset,
                    &row_rect,
                    sizeof(SceneData::RectCommand));

        auto label = make_widget_authoring_id(authoring_root,
                                              std::string("list/item/") + std::to_string(index));
        bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
            drawable_id, std::move(label), 0, 0});
        bucket.drawable_fingerprints.push_back(drawable_id);
    }

    bucket.alpha_indices.clear();
   bucket.layer_indices.clear();
    return bucket;
}

auto publish_scene_snapshot(PathSpace& space,
                            AppRootPathView appRoot,
                            ScenePath const& scenePath,
                            SceneData::DrawableBucketSnapshot const& bucket,
                            std::string_view author = "widgets",
                            std::string_view tool_version = "widgets-toolkit") -> SP::Expected<void> {
    SceneData::SceneSnapshotBuilder builder{space, appRoot, scenePath};
    SceneData::SnapshotPublishOptions options{};
    options.metadata.author = std::string(author);
    options.metadata.tool_version = std::string(tool_version);
    options.metadata.created_at = std::chrono::system_clock::now();
    options.metadata.drawable_count = bucket.drawable_ids.size();
    options.metadata.command_count = bucket.command_kinds.size();

    auto revision = builder.publish(options, bucket);
    if (!revision) {
        return std::unexpected(revision.error());
    }

    auto ready = Scene::WaitUntilReady(space, scenePath, std::chrono::milliseconds{50});
    if (!ready) {
        return std::unexpected(ready.error());
    }
    return {};
}

auto ensure_widget_state_scene(PathSpace& space,
                               AppRootPathView appRoot,
                               std::string_view name,
                               std::string_view state,
                               std::string_view description_prefix) -> SP::Expected<ScenePath> {
    auto spec = std::string("scenes/widgets/") + std::string(name) + "/states/" + std::string(state);
    auto resolved = combine_relative(appRoot, spec);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    ScenePath scenePath{resolved->getPath()};
    auto metaNamePath = make_scene_meta(scenePath, "name");
    auto existing = read_optional<std::string>(space, metaNamePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        if (auto status = replace_single<std::string>(space, metaNamePath, std::string(state)); !status) {
            return std::unexpected(status.error());
        }
        auto metaDescPath = make_scene_meta(scenePath, "description");
        auto description = std::string(description_prefix) + " (" + std::string(state) + ")";
        if (auto status = replace_single<std::string>(space, metaDescPath, description); !status) {
            return std::unexpected(status.error());
        }
    }
    return scenePath;
}

auto button_background_color(Widgets::ButtonStyle const& style,
                             Widgets::ButtonState const& state) -> std::array<float, 4> {
    auto base = style.background_color;
    if (!state.enabled) {
        return scale_alpha(desaturate_color(base, 0.65f), 0.55f);
    }
    if (state.pressed) {
        return darken_color(base, 0.18f);
    }
    if (state.hovered) {
        return lighten_color(base, 0.12f);
    }
    return base;
}

auto build_button_bucket(Widgets::ButtonStyle const& style,
                         Widgets::ButtonState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    float width = std::max(style.width, 1.0f);
    float height = std::max(style.height, 1.0f);
    float radius_limit = std::min(width, height) * 0.5f;
    float corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    ButtonSnapshotConfig config{
        .width = width,
        .height = height,
        .corner_radius = corner_radius,
        .color = button_background_color(style, state),
    };
    return make_button_bucket(config, authoring_root);
}

auto build_button_bucket(Widgets::ButtonStyle const& style,
                         Widgets::ButtonState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_button_bucket(style, state, {});
}

auto build_toggle_bucket(Widgets::ToggleStyle const& style,
                         Widgets::ToggleState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    ToggleSnapshotConfig config{
        .width = std::max(style.width, 1.0f),
        .height = std::max(style.height, 1.0f),
        .checked = state.checked,
        .track_off_color = style.track_off_color,
        .track_on_color = style.track_on_color,
        .thumb_color = style.thumb_color,
    };

    if (!state.enabled) {
        config.track_off_color = scale_alpha(desaturate_color(config.track_off_color, 0.6f), 0.5f);
        config.track_on_color = scale_alpha(desaturate_color(config.track_on_color, 0.6f), 0.5f);
        config.thumb_color = scale_alpha(desaturate_color(config.thumb_color, 0.6f), 0.5f);
    } else if (state.hovered) {
        config.track_off_color = lighten_color(config.track_off_color, 0.12f);
        config.track_on_color = lighten_color(config.track_on_color, 0.10f);
        config.thumb_color = lighten_color(config.thumb_color, 0.08f);
    }
    if (state.checked && state.hovered) {
        config.track_on_color = lighten_color(config.track_on_color, 0.08f);
    }

    return make_toggle_bucket(config, authoring_root);
}

auto build_toggle_bucket(Widgets::ToggleStyle const& style,
                         Widgets::ToggleState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_toggle_bucket(style, state, {});
}

auto clamp_slider_value(Widgets::SliderRange const& range, float value) -> float {
    float minimum = std::min(range.minimum, range.maximum);
    float maximum = std::max(range.minimum, range.maximum);
    if (minimum == maximum) {
        maximum = minimum + 1.0f;
    }
    float clamped = std::clamp(value, minimum, maximum);
    if (range.step > 0.0f) {
        float steps = std::round((clamped - minimum) / range.step);
        clamped = minimum + steps * range.step;
        clamped = std::clamp(clamped, minimum, maximum);
    }
    return clamped;
}

auto build_slider_bucket(Widgets::SliderStyle const& style,
                         Widgets::SliderRange const& range,
                         Widgets::SliderState const& state,
                         std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    Widgets::SliderState applied = state;
    applied.value = clamp_slider_value(range, state.value);

    SliderSnapshotConfig config{
        .width = std::max(style.width, 1.0f),
        .height = std::max(style.height, 16.0f),
        .track_height = std::clamp(style.track_height, 1.0f, style.height),
        .thumb_radius = std::clamp(style.thumb_radius,
                                   style.track_height * 0.5f,
                                   style.height * 0.5f),
        .min = range.minimum,
        .max = range.maximum,
        .value = applied.value,
        .track_color = style.track_color,
        .fill_color = style.fill_color,
        .thumb_color = style.thumb_color,
    };

    if (!applied.enabled) {
        config.track_color = scale_alpha(desaturate_color(config.track_color, 0.6f), 0.5f);
        config.fill_color = scale_alpha(desaturate_color(config.fill_color, 0.6f), 0.5f);
        config.thumb_color = scale_alpha(desaturate_color(config.thumb_color, 0.6f), 0.5f);
    } else if (applied.dragging) {
        config.fill_color = lighten_color(config.fill_color, 0.10f);
        config.thumb_color = darken_color(config.thumb_color, 0.12f);
    } else if (applied.hovered) {
        config.fill_color = lighten_color(config.fill_color, 0.08f);
        config.thumb_color = lighten_color(config.thumb_color, 0.06f);
    }

    return make_slider_bucket(config, authoring_root);
}

auto build_slider_bucket(Widgets::SliderStyle const& style,
                         Widgets::SliderRange const& range,
                         Widgets::SliderState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_slider_bucket(style, range, state, {});
}

auto first_enabled_index(std::vector<Widgets::ListItem> const& items) -> std::int32_t {
    auto it = std::find_if(items.begin(), items.end(), [](auto const& item) {
        return item.enabled;
    });
    if (it == items.end()) {
        return -1;
    }
    return static_cast<std::int32_t>(std::distance(items.begin(), it));
}

auto build_list_bucket(Widgets::ListStyle const& style,
                       std::vector<Widgets::ListItem> const& items,
                       Widgets::ListState const& state,
                       std::string_view authoring_root) -> SceneData::DrawableBucketSnapshot {
    Widgets::ListStyle appliedStyle = style;
    Widgets::ListState appliedState = state;
    if (!appliedState.enabled) {
        appliedStyle.background_color = scale_alpha(desaturate_color(appliedStyle.background_color, 0.6f), 0.6f);
        appliedStyle.border_color = scale_alpha(desaturate_color(appliedStyle.border_color, 0.6f), 0.6f);
        appliedStyle.item_color = scale_alpha(desaturate_color(appliedStyle.item_color, 0.6f), 0.6f);
        appliedStyle.item_hover_color = scale_alpha(desaturate_color(appliedStyle.item_hover_color, 0.6f), 0.6f);
        appliedStyle.item_selected_color = scale_alpha(desaturate_color(appliedStyle.item_selected_color, 0.6f), 0.6f);
        appliedStyle.separator_color = scale_alpha(desaturate_color(appliedStyle.separator_color, 0.6f), 0.6f);
        appliedStyle.item_text_color = scale_alpha(desaturate_color(appliedStyle.item_text_color, 0.6f), 0.6f);
        appliedState.hovered_index = -1;
        appliedState.selected_index = -1;
    }

    ListSnapshotConfig config{
        .width = std::max(appliedStyle.width, 96.0f),
        .item_height = std::max(appliedStyle.item_height, 24.0f),
        .corner_radius = std::clamp(appliedStyle.corner_radius,
                                    0.0f,
                                    std::min(appliedStyle.width,
                                             appliedStyle.item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u))) * 0.5f),
        .border_thickness = std::clamp(appliedStyle.border_thickness,
                                       0.0f,
                                       appliedStyle.item_height * 0.5f),
        .item_count = items.size(),
        .selected_index = appliedState.selected_index,
        .hovered_index = appliedState.hovered_index,
        .background_color = appliedStyle.background_color,
        .border_color = appliedStyle.border_color,
        .item_color = appliedStyle.item_color,
        .item_hover_color = appliedStyle.item_hover_color,
        .item_selected_color = appliedStyle.item_selected_color,
        .separator_color = appliedStyle.separator_color,
    };

    return make_list_bucket(config);
}

auto build_list_bucket(Widgets::ListStyle const& style,
                       std::vector<Widgets::ListItem> const& items,
                       Widgets::ListState const& state) -> SceneData::DrawableBucketSnapshot {
    return build_list_bucket(style, items, state, {});
}

auto publish_button_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::ButtonStyle const& style) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::ButtonState button_state;
        ScenePath* target = nullptr;
    };

    Variant variants[] = {
        {"idle", Widgets::ButtonState{}, &scenes.idle},
        {"hover", Widgets::ButtonState{.hovered = true}, &scenes.hover},
        {"pressed", Widgets::ButtonState{.pressed = true, .hovered = true}, &scenes.pressed},
        {"disabled", Widgets::ButtonState{.enabled = false}, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget button state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_button_bucket(style, variant.button_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

auto publish_toggle_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::ToggleStyle const& style) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::ToggleState toggle_state;
        ScenePath* target = nullptr;
    };

    Variant variants[] = {
        {"idle", Widgets::ToggleState{}, &scenes.idle},
        {"hover", Widgets::ToggleState{.hovered = true}, &scenes.hover},
        {"pressed", Widgets::ToggleState{.checked = true, .hovered = true}, &scenes.pressed},
        {"disabled", Widgets::ToggleState{.enabled = false}, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget toggle state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_toggle_bucket(style, variant.toggle_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

auto publish_slider_state_scenes(PathSpace& space,
                                 AppRootPathView appRoot,
                                 std::string_view name,
                                 Widgets::SliderStyle const& style,
                                 Widgets::SliderRange const& range,
                                 Widgets::SliderState const& default_state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};
    struct Variant {
        std::string_view state;
        Widgets::SliderState slider_state;
        ScenePath* target = nullptr;
    };

    Widgets::SliderState idle = default_state;
    Widgets::SliderState hover = idle;
    hover.hovered = true;
    Widgets::SliderState pressed = idle;
    pressed.dragging = true;
    pressed.hovered = true;
    Widgets::SliderState disabled = idle;
    disabled.enabled = false;

    Variant variants[] = {
        {"idle", idle, &scenes.idle},
        {"hover", hover, &scenes.hover},
        {"pressed", pressed, &scenes.pressed},
        {"disabled", disabled, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget slider state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_slider_bucket(style, range, variant.slider_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

auto publish_list_state_scenes(PathSpace& space,
                               AppRootPathView appRoot,
                               std::string_view name,
                               Widgets::ListStyle const& style,
                               std::vector<Widgets::ListItem> const& items,
                               Widgets::ListState const& default_state) -> SP::Expected<Widgets::WidgetStateScenes> {
    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + std::string(name));
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }
    auto const& authoring_root = widgetRoot->getPath();

    Widgets::WidgetStateScenes scenes{};

    auto normalize_index = [&](std::int32_t index) -> std::int32_t {
        if (index < 0) {
            return -1;
        }
        if (index >= static_cast<std::int32_t>(items.size())) {
            return items.empty() ? -1 : static_cast<std::int32_t>(items.size()) - 1;
        }
        if (!items[static_cast<std::size_t>(index)].enabled) {
            return first_enabled_index(items);
        }
        return index;
    };

    Widgets::ListState idle = default_state;
    idle.selected_index = normalize_index(idle.selected_index);
    Widgets::ListState hover = idle;
    if (hover.selected_index < 0) {
        hover.hovered_index = normalize_index(0);
    } else {
        hover.hovered_index = hover.selected_index;
    }
    Widgets::ListState pressed = idle;
    if (pressed.selected_index < 0) {
        pressed.selected_index = normalize_index(0);
    }
    Widgets::ListState disabled = idle;
    disabled.enabled = false;
    disabled.selected_index = -1;
    disabled.hovered_index = -1;

    struct Variant {
        std::string_view state;
        Widgets::ListState list_state;
        ScenePath* target = nullptr;
    };

    Variant variants[] = {
        {"idle", idle, &scenes.idle},
        {"hover", hover, &scenes.hover},
        {"pressed", pressed, &scenes.pressed},
        {"disabled", disabled, &scenes.disabled},
    };

    for (auto& variant : variants) {
        auto scenePath = ensure_widget_state_scene(space,
                                                   appRoot,
                                                   name,
                                                   variant.state,
                                                   "Widget list state");
        if (!scenePath) {
            return std::unexpected(scenePath.error());
        }
        auto bucket = build_list_bucket(style, items, variant.list_state, authoring_root);
        if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
            return std::unexpected(status.error());
        }
        *variant.target = *scenePath;
    }
    return scenes;
}

auto ensure_widget_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name,
                         std::string_view description) -> SP::Expected<ScenePath> {
    auto resolved = combine_relative(appRoot, std::string("scenes/widgets/") + std::string(name));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    ScenePath scenePath{resolved->getPath()};
    auto metaNamePath = make_scene_meta(scenePath, "name");
    auto existing = read_optional<std::string>(space, metaNamePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        if (auto status = replace_single<std::string>(space, metaNamePath, std::string{name}); !status) {
            return std::unexpected(status.error());
        }
        auto metaDescPath = make_scene_meta(scenePath, "description");
        if (auto status = replace_single<std::string>(space, metaDescPath, std::string(description)); !status) {
            return std::unexpected(status.error());
        }
    }
    return scenePath;
}

auto ensure_slider_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget slider");
}

auto ensure_list_scene(PathSpace& space,
                       AppRootPathView appRoot,
                       std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget list");
}

auto write_widget_kind(PathSpace& space,
                       std::string const& rootPath,
                       std::string_view kind) -> SP::Expected<void> {
    auto kindPath = rootPath + "/meta/kind";
    if (auto status = replace_single<std::string>(space, kindPath, std::string{kind}); !status) {
        return status;
    }
    return {};
}

auto write_button_metadata(PathSpace& space,
                           std::string const& rootPath,
                           std::string const& label,
                           Widgets::ButtonState const& state,
                           Widgets::ButtonStyle const& style) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ButtonState>(space, statePath, state); !status) {
        return status;
    }

    auto labelPath = rootPath + "/meta/label";
    if (auto status = replace_single<std::string>(space, labelPath, label); !status) {
        return status;
    }

    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::ButtonStyle>(space, stylePath, style); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "button"); !status) {
        return status;
    }

    return {};
}

auto write_slider_metadata(PathSpace& space,
                           std::string const& rootPath,
                           Widgets::SliderState const& state,
                           Widgets::SliderStyle const& style,
                           Widgets::SliderRange const& range) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::SliderState>(space, statePath, state); !status) {
        return status;
    }

    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::SliderStyle>(space, stylePath, style); !status) {
        return status;
    }

    auto rangePath = rootPath + "/meta/range";
    if (auto status = replace_single<Widgets::SliderRange>(space, rangePath, range); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "slider"); !status) {
        return status;
    }

    return {};
}

auto write_list_metadata(PathSpace& space,
                         std::string const& rootPath,
                         Widgets::ListState const& state,
                         Widgets::ListStyle const& style,
                         std::vector<Widgets::ListItem> const& items) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ListState>(space, statePath, state); !status) {
        return status;
    }
    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::ListStyle>(space, stylePath, style); !status) {
        return status;
    }
    auto itemsPath = rootPath + "/meta/items";
    if (auto status = replace_single<std::vector<Widgets::ListItem>>(space, itemsPath, items); !status) {
        return status;
    }

    if (auto status = write_widget_kind(space, rootPath, "list"); !status) {
        return status;
    }
    return {};
}

auto surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>> cache;
    return cache;
}

auto surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

#if PATHSPACE_UI_METAL
auto metal_surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>> cache;
    return cache;
}

auto metal_surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}
#endif

auto before_present_hook_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

auto before_present_hook_storage()
    -> Window::TestHooks::BeforePresentHook& {
    static Window::TestHooks::BeforePresentHook hook;
    return hook;
}

void invoke_before_present_hook(PathSurfaceSoftware& surface,
                                PathWindowView::PresentPolicy& policy,
                                std::vector<std::size_t>& dirty_tiles) {
    Window::TestHooks::BeforePresentHook hook_copy;
    {
        std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
        hook_copy = before_present_hook_storage();
    }
    if (hook_copy) {
        hook_copy(surface, policy, dirty_tiles);
    }
}

auto acquire_surface_unlocked(std::string const& key,
                              Builders::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
    auto& cache = surfaces_cache();
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto surface = std::make_unique<PathSurfaceSoftware>(desc);
        auto* raw = surface.get();
        cache.emplace(key, std::move(surface));
        return *raw;
    }

    auto& surface = *it->second;
    auto const& current = surface.desc();
    if (current.size_px.width != desc.size_px.width
        || current.size_px.height != desc.size_px.height
        || current.pixel_format != desc.pixel_format
        || current.color_space != desc.color_space
        || current.premultiplied_alpha != desc.premultiplied_alpha) {
        surface.resize(desc);
    }
    return surface;
}

auto acquire_surface(std::string const& key,
                     Builders::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
    auto& mutex = surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_surface_unlocked(key, desc);
}

#if PATHSPACE_UI_METAL
auto acquire_metal_surface_unlocked(std::string const& key,
                                    Builders::SurfaceDesc const& desc) -> PathSurfaceMetal& {
    auto& cache = metal_surfaces_cache();
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto surface = std::make_unique<PathSurfaceMetal>(desc);
        auto* raw = surface.get();
        cache.emplace(key, std::move(surface));
        return *raw;
    }

    auto& surface = *it->second;
    auto const& current = surface.desc();
    if (current.size_px.width != desc.size_px.width
        || current.size_px.height != desc.size_px.height
        || current.pixel_format != desc.pixel_format
        || current.color_space != desc.color_space
        || current.premultiplied_alpha != desc.premultiplied_alpha) {
        surface.resize(desc);
    }
    return surface;
}

auto acquire_metal_surface(std::string const& key,
                           Builders::SurfaceDesc const& desc) -> PathSurfaceMetal& {
    auto& mutex = metal_surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_metal_surface_unlocked(key, desc);
}
#endif

auto enqueue_auto_render_event(PathSpace& space,
                               std::string const& targetPath,
                               std::string_view reason,
                               std::uint64_t frame_index) -> SP::Expected<void> {
    auto queuePath = targetPath + "/events/renderRequested/queue";
    AutoRenderRequestEvent event{
        .sequence = g_auto_render_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        .reason = std::string(reason),
        .frame_index = frame_index,
    };
    auto inserted = space.insert(queuePath, event);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto maybe_schedule_auto_render_impl(PathSpace& space,
                                     std::string const& targetPath,
                                     PathWindowView::PresentStats const& stats,
                                     PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool> {
    if (!policy.auto_render_on_present) {
        return false;
    }

    std::vector<std::string> reasons;
    if (stats.skipped) {
        reasons.emplace_back("present-skipped");
    }

    if (!stats.buffered_frame_consumed) {
        if (stats.frame_age_frames > policy.max_age_frames) {
            reasons.emplace_back("age-frames");
        }
        if (stats.frame_age_ms > policy.staleness_budget_ms_value) {
            reasons.emplace_back("age-ms");
        }
    } else {
        if (stats.frame_age_frames > policy.max_age_frames) {
            reasons.emplace_back("age-frames");
        }
        if (stats.frame_age_ms > policy.staleness_budget_ms_value) {
            reasons.emplace_back("age-ms");
        }
    }

    if (reasons.empty()) {
        return false;
    }

    std::string reason = reasons.front();
    for (std::size_t i = 1; i < reasons.size(); ++i) {
        reason.append(",");
        reason.append(reasons[i]);
    }

    auto status = enqueue_auto_render_event(space, targetPath, reason, stats.frame.frame_index);
    if (!status) {
        return std::unexpected(status.error());
    }
    return true;
}

auto dirty_state_path(ScenePath const& scenePath) -> std::string {
    return std::string(scenePath.getPath()) + "/diagnostics/dirty/state";
}

auto dirty_queue_path(ScenePath const& scenePath) -> std::string {
    return std::string(scenePath.getPath()) + "/diagnostics/dirty/queue";
}

constexpr auto dirty_mask(Scene::DirtyKind kind) -> std::uint32_t {
    return static_cast<std::uint32_t>(kind);
}

constexpr auto make_dirty_kind(std::uint32_t mask) -> Scene::DirtyKind {
    return static_cast<Scene::DirtyKind>(mask & static_cast<std::uint32_t>(Scene::DirtyKind::All));
}

struct SurfaceRenderContext {
    SP::ConcretePathString target_path;
    SP::ConcretePathString renderer_path;
    SurfaceDesc            target_desc;
    RenderSettings         settings;
    RendererKind           renderer_kind = RendererKind::Software2D;
};

template <typename T>
auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>>;

auto present_mode_to_string(PathWindowView::PresentMode mode) -> std::string {
    switch (mode) {
    case PathWindowView::PresentMode::AlwaysFresh:
        return "AlwaysFresh";
    case PathWindowView::PresentMode::PreferLatestCompleteWithBudget:
        return "PreferLatestCompleteWithBudget";
    case PathWindowView::PresentMode::AlwaysLatestComplete:
        return "AlwaysLatestComplete";
    }
    return "Unknown";
}

auto parse_present_mode(std::string_view text) -> SP::Expected<PathWindowView::PresentMode> {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized.empty()) {
        return std::unexpected(make_error("present policy string must not be empty",
                                          SP::Error::Code::InvalidType));
    }
    if (normalized == "alwaysfresh") {
        return PathWindowView::PresentMode::AlwaysFresh;
    }
    if (normalized == "preferlatestcompletewithbudget"
        || normalized == "preferlatestcomplete") {
        return PathWindowView::PresentMode::PreferLatestCompleteWithBudget;
    }
    if (normalized == "alwayslatestcomplete") {
        return PathWindowView::PresentMode::AlwaysLatestComplete;
    }
    return std::unexpected(make_error("unknown present policy '" + std::string(text) + "'",
                                      SP::Error::Code::InvalidType));
}

auto read_present_policy(PathSpace const& space,
                         std::string const& viewBase) -> SP::Expected<PathWindowView::PresentPolicy> {
    PathWindowView::PresentPolicy policy{};
    auto policyPath = viewBase + "/present/policy";
    auto policyValue = read_optional<std::string>(space, policyPath);
    if (!policyValue) {
        return std::unexpected(policyValue.error());
    }
    if (policyValue->has_value()) {
        auto mode = parse_present_mode(**policyValue);
        if (!mode) {
            return std::unexpected(mode.error());
        }
        policy.mode = *mode;
    }

    auto read_double = [&](std::string const& path) -> SP::Expected<std::optional<double>> {
        return read_optional<double>(space, path);
    };
    auto read_uint64 = [&](std::string const& path) -> SP::Expected<std::optional<std::uint64_t>> {
        return read_optional<std::uint64_t>(space, path);
    };
    auto read_bool = [&](std::string const& path) -> SP::Expected<std::optional<bool>> {
        return read_optional<bool>(space, path);
    };

    auto paramsBase = viewBase + "/present/params";
    if (auto value = read_double(paramsBase + "/staleness_budget_ms"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.staleness_budget_ms_value = **value;
        auto duration = std::chrono::duration<double, std::milli>(**value);
        policy.staleness_budget = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    } else {
        policy.staleness_budget_ms_value = static_cast<double>(policy.staleness_budget.count());
    }

    if (auto value = read_double(paramsBase + "/frame_timeout_ms"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.frame_timeout_ms_value = **value;
        auto duration = std::chrono::duration<double, std::milli>(**value);
        policy.frame_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    } else {
        policy.frame_timeout_ms_value = static_cast<double>(policy.frame_timeout.count());
    }

    if (auto value = read_uint64(paramsBase + "/max_age_frames"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.max_age_frames = static_cast<std::uint32_t>(**value);
    }

    if (auto value = read_bool(paramsBase + "/vsync_align"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.vsync_align = **value;
    }

    if (auto value = read_bool(paramsBase + "/auto_render_on_present"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.auto_render_on_present = **value;
    }

    if (auto value = read_bool(paramsBase + "/capture_framebuffer"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.capture_framebuffer = **value;
    }

    return policy;
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext>;

auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind>;
auto read_renderer_kind(PathSpace& space,
                        std::string const& path) -> SP::Expected<RendererKind>;
auto renderer_kind_to_string(RendererKind kind) -> std::string;

auto ensure_non_empty(std::string_view value,
                      std::string_view what) -> SP::Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(std::string(what) + " must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto ensure_identifier(std::string_view value,
                       std::string_view what) -> SP::Expected<void> {
    if (auto status = ensure_non_empty(value, what); !status) {
        return status;
    }
    if (value == "." || value == "..") {
        return std::unexpected(make_error(std::string(what) + " must not be '.' or '..'",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    if (value.find('/') != std::string_view::npos) {
        return std::unexpected(make_error(std::string(what) + " must not contain '/' characters",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    return {};
}

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
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
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single(PathSpace& space,
                   std::string const& path,
                   T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

template <typename T>
auto read_value(PathSpace const& space,
                std::string const& path,
                SP::Out const& out = {}) -> SP::Expected<T> {
    auto const& base = static_cast<PathSpaceBase const&>(space);
    return base.template read<T, std::string>(path, out);
}

template <typename T>
auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>> {
    auto value = read_value<T>(space, path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}
auto combine_relative(AppRootPathView root,
                       std::string relative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, std::move(relative));
}

auto relative_to_root(AppRootPathView root,
                      ConcretePathView absolute) -> SP::Expected<std::string> {
    auto ensured = SP::App::ensure_within_app(root, absolute);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    auto const& rootStr = root.getPath();
    auto const& absStr = absolute.getPath();
    if (absStr.size() == rootStr.size()) {
        return std::string{};
    }
    if (absStr.size() <= rootStr.size() + 1) {
        return std::string{};
    }
    return std::string(absStr.substr(rootStr.size() + 1));
}

auto derive_app_root_for(ConcretePathView absolute) -> SP::Expected<AppRootPath> {
    return SP::App::derive_app_root(absolute);
}

auto ensure_contains_segment(ConcretePathView path,
                             std::string_view segment) -> SP::Expected<void> {
    if (path.getPath().find(segment) == std::string::npos) {
        return std::unexpected(make_error("path '" + std::string(path.getPath()) + "' missing segment '"
                                          + std::string(segment) + "'",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto same_app(ConcretePathView lhs,
             ConcretePathView rhs) -> SP::Expected<void> {
    auto lhsRoot = derive_app_root_for(lhs);
    if (!lhsRoot) {
        return std::unexpected(lhsRoot.error());
    }
    auto rhsRoot = derive_app_root_for(rhs);
    if (!rhsRoot) {
        return std::unexpected(rhsRoot.error());
    }
    if (lhsRoot->getPath() != rhsRoot->getPath()) {
        return std::unexpected(make_error("paths belong to different application roots",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext> {
    auto surfaceRoot = derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = read_value<std::string>(space, targetField);
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                                        *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto descPath = targetAbsolute->getPath() + "/desc";
    auto targetDesc = read_value<SurfaceDesc>(space, descPath);
    if (!targetDesc) {
        return std::unexpected(targetDesc.error());
    }

    auto const& targetStr = targetAbsolute->getPath();
    auto targetsPos = targetStr.find("/targets/");
    if (targetsPos == std::string::npos) {
        return std::unexpected(make_error("target path '" + targetStr + "' missing /targets/ segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto rendererPathStr = targetStr.substr(0, targetsPos);
    if (rendererPathStr.empty()) {
        return std::unexpected(make_error("renderer path derived from target is empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererKind = read_renderer_kind(space, rendererPathStr + "/meta/kind");
    if (!rendererKind) {
        return std::unexpected(rendererKind.error());
    }

    auto effectiveKind = *rendererKind;
#if !PATHSPACE_UI_METAL
    if (effectiveKind == RendererKind::Metal2D) {
        effectiveKind = RendererKind::Software2D;
    }
#else
    if (effectiveKind == RendererKind::Metal2D
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        effectiveKind = RendererKind::Software2D;
    }
#endif

    RenderSettings effective{};
    if (settingsOverride) {
        effective = *settingsOverride;
    } else {
        auto stored = Renderer::ReadSettings(space, ConcretePathView{targetAbsolute->getPath()});
        if (stored) {
            effective = *stored;
        } else {
            auto const& error = stored.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
            effective.surface.size_px.width = targetDesc->size_px.width;
            effective.surface.size_px.height = targetDesc->size_px.height;
            effective.surface.dpi_scale = 1.0f;
            effective.surface.visibility = true;
            effective.surface.metal = targetDesc->metal;
            effective.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
            effective.time.time_ms = 0.0;
            effective.time.delta_ms = 16.0;
            effective.time.frame_index = 0;
        }
    }

    effective.surface.size_px.width = targetDesc->size_px.width;
    effective.surface.size_px.height = targetDesc->size_px.height;
    effective.surface.metal = targetDesc->metal;
    if (effective.surface.dpi_scale == 0.0f) {
        effective.surface.dpi_scale = 1.0f;
    }

    if (!settingsOverride) {
        if (effective.time.delta_ms == 0.0) {
            effective.time.delta_ms = 16.0;
        }
        effective.time.time_ms += effective.time.delta_ms;
        effective.time.frame_index += 1;
    }

    effective.renderer.backend_kind = effectiveKind;
#if PATHSPACE_UI_METAL
    effective.renderer.metal_uploads_enabled = (effectiveKind == RendererKind::Metal2D)
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") != nullptr;
#else
    effective.renderer.metal_uploads_enabled = false;
#endif

    if (auto status = Renderer::UpdateSettings(space,
                                               ConcretePathView{targetAbsolute->getPath()},
                                               effective); !status) {
        return std::unexpected(status.error());
    }

    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{targetAbsolute->getPath()},
        .renderer_path = SP::ConcretePathString{rendererPathStr},
        .target_desc = *targetDesc,
        .settings = effective,
        .renderer_kind = effectiveKind,
    };
    return context;
}

auto render_into_target(PathSpace& space,
                        SurfaceRenderContext const& context,
                        PathSurfaceSoftware& software_surface
#if PATHSPACE_UI_METAL
                        ,
                        PathSurfaceMetal* metal_surface
#endif
                        ) -> SP::Expected<PathRenderer2D::RenderStats> {
#if PATHSPACE_UI_METAL
    if (context.renderer_kind == RendererKind::Metal2D) {
        if (metal_surface == nullptr) {
            return std::unexpected(make_error("metal renderer requested without metal surface cache",
                                              SP::Error::Code::InvalidType));
        }
    } else if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#else
    if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#endif

    PathRenderer2D renderer{space};
    PathRenderer2D::RenderParams params{
        .target_path = SP::ConcretePathStringView{context.target_path.getPath()},
        .settings = context.settings,
        .surface = software_surface,
        .backend_kind = context.renderer_kind,
#if PATHSPACE_UI_METAL
        .metal_surface = metal_surface,
#endif
    };
    return renderer.render(params);
}

auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

auto to_epoch_ns(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
}

auto from_epoch_ms(int64_t ms) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

auto to_record(SceneRevisionDesc const& desc) -> SceneRevisionRecord {
    SceneRevisionRecord record{};
    record.revision = desc.revision;
    record.published_at_ms = to_epoch_ms(desc.published_at);
    record.author = desc.author;
    return record;
}

auto from_record(SceneRevisionRecord const& record) -> SceneRevisionDesc {
    SceneRevisionDesc desc{};
    desc.revision = record.revision;
    desc.published_at = from_epoch_ms(record.published_at_ms);
    desc.author = record.author;
    return desc;
}

auto format_revision(uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto is_safe_asset_path(std::string_view path) -> bool {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return false;
    }
    if (path.find("..") != std::string_view::npos) {
        return false;
    }
    return true;
}

auto guess_mime_type(std::string_view logical_path) -> std::string {
    auto dot = logical_path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= logical_path.size()) {
        return "application/octet-stream";
    }
    std::string ext;
    ext.reserve(logical_path.size() - (dot + 1));
    for (std::size_t i = dot + 1; i < logical_path.size(); ++i) {
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(logical_path[i]))));
    }

    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "webp") return "image/webp";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "avif") return "image/avif";
    if (ext == "bmp")  return "image/bmp";

    if (ext == "woff2") return "font/woff2";
    if (ext == "woff")  return "font/woff";
    if (ext == "ttf")   return "font/ttf";
    if (ext == "otf")   return "font/otf";

    if (ext == "css")   return "text/css";
    if (ext == "js" || ext == "mjs") return "text/javascript";
    if (ext == "json")  return "application/json";

    return "application/octet-stream";
}

auto hydrate_html_assets(PathSpace& space,
                         std::string const& revisionBase,
                         std::vector<Html::Asset>& assets) -> SP::Expected<void> {
    for (auto& asset : assets) {
        bool const needs_lookup = asset.bytes.empty()
                                  || asset.mime_type == Html::kImageAssetReferenceMime
                                  || asset.mime_type == Html::kFontAssetReferenceMime;
        if (!needs_lookup) {
            continue;
        }

        if (!is_safe_asset_path(asset.logical_path)) {
            return std::unexpected(make_error("html asset logical path unsafe: " + asset.logical_path,
                                              SP::Error::Code::InvalidPath));
        }

        std::string full_path = revisionBase;
        if (asset.logical_path.rfind("assets/", 0) == 0) {
            full_path.append("/").append(asset.logical_path);
        } else {
            full_path.append("/assets/").append(asset.logical_path);
        }

        auto bytes = space.read<std::vector<std::uint8_t>>(full_path);
        if (!bytes) {
            auto const error = bytes.error();
            std::string message = "read html asset '" + asset.logical_path + "'";
            if (error.message) {
                message.append(": ").append(*error.message);
            }
            return std::unexpected(make_error(std::move(message), error.code));
        }

        asset.bytes = std::move(*bytes);
        if (asset.mime_type == Html::kImageAssetReferenceMime
            || asset.mime_type == Html::kFontAssetReferenceMime
            || asset.mime_type.empty()) {
            asset.mime_type = guess_mime_type(asset.logical_path);
        }
    }
    return {};
}

auto make_revision_base(ScenePath const& scenePath,
                        std::string const& revisionStr) -> std::string {
    return std::string(scenePath.getPath()) + "/builds/" + revisionStr;
}

auto make_scene_meta(ScenePath const& scenePath,
                     std::string const& leaf) -> std::string {
    return std::string(scenePath.getPath()) + "/meta/" + leaf;
}

auto bytes_from_span(std::span<std::byte const> bytes) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

auto resolve_renderer_spec(AppRootPathView appRoot,
                           std::string const& spec) -> SP::Expected<ConcretePath> {
    if (spec.empty()) {
        return std::unexpected(make_error("renderer spec must not be empty",
                                          SP::Error::Code::InvalidPath));
    }

    if (spec.front() == '/') {
        return SP::App::resolve_app_relative(appRoot, spec);
    }

    std::string candidate = spec;
    if (spec.find('/') == std::string::npos) {
        candidate = "renderers/" + spec;
    }
    return SP::App::resolve_app_relative(appRoot, candidate);
}

auto leaf_component(ConcretePathView path) -> SP::Expected<std::string> {
    SP::UnvalidatedPathView raw{path.getPath()};
    auto components = raw.split_absolute_components();
    if (!components) {
        return std::unexpected(components.error());
    }
    if (components->empty()) {
        return std::unexpected(make_error("path has no components",
                                          SP::Error::Code::InvalidPath));
    }
    return std::string(components->back());
}

auto read_relative_string(PathSpace const& space,
                          std::string const& path) -> SP::Expected<std::string> {
    auto value = read_value<std::string>(space, path);
    if (value) {
        return *value;
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound) {
        return std::string{};
    }
    return std::unexpected(error);
}

auto store_desc(PathSpace& space,
                std::string const& path,
                SurfaceDesc const& desc) -> SP::Expected<void> {
    return replace_single<SurfaceDesc>(space, path, desc);
}

auto store_renderer_kind(PathSpace& space,
                         std::string const& path,
                         RendererKind kind) -> SP::Expected<void> {
    auto status = replace_single<RendererKind>(space, path, kind);
    if (status) {
        return status;
    }

    auto const& error = status.error();
    auto is_type_mismatch = (error.code == SP::Error::Code::TypeMismatch
                             || error.code == SP::Error::Code::InvalidType);
    if (!is_type_mismatch) {
        std::cerr << "store_renderer_kind: replace_single failed code="
                  << static_cast<int>(error.code)
                  << " message=" << error.message.value_or("<none>") << std::endl;
        return status;
    }

    if (auto cleared = drain_queue<std::string>(space, path); !cleared) {
        std::cerr << "store_renderer_kind: drain_queue<string> failed code="
                  << static_cast<int>(cleared.error().code)
                  << " message=" << cleared.error().message.value_or("<none>")
                  << std::endl;
        return cleared;
    }

    return replace_single<RendererKind>(space, path, kind);
}

auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind> {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (normalized == "software" || normalized == "software2d") {
        return RendererKind::Software2D;
    }
    if (normalized == "metal" || normalized == "metal2d") {
        return RendererKind::Metal2D;
    }
    if (normalized == "vulkan" || normalized == "vulkan2d") {
        return RendererKind::Vulkan2D;
    }
    return std::nullopt;
}

auto read_renderer_kind(PathSpace& space,
                        std::string const& path) -> SP::Expected<RendererKind> {
    auto stored = read_value<RendererKind>(space, path);
    if (stored) {
        return *stored;
    }

    auto const& error = stored.error();
    if (error.code == SP::Error::Code::TypeMismatch) {
        auto legacy = read_value<std::string>(space, path);
        if (!legacy) {
            return std::unexpected(legacy.error());
        }
        auto parsed = parse_renderer_kind(*legacy);
        if (!parsed) {
            return std::unexpected(make_error("unable to parse renderer kind '" + *legacy + "'",
                                              SP::Error::Code::InvalidType));
        }
        if (auto status = store_renderer_kind(space, path, *parsed); !status) {
            return std::unexpected(status.error());
        }
        return *parsed;
    }

    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        auto fallback = RendererKind::Software2D;
        if (auto status = store_renderer_kind(space, path, fallback); !status) {
            return std::unexpected(status.error());
        }
        return fallback;
    }

    return std::unexpected(error);
}

auto renderer_kind_to_string(RendererKind kind) -> std::string {
    switch (kind) {
    case RendererKind::Software2D:
        return "Software2D";
    case RendererKind::Metal2D:
        return "Metal2D";
    case RendererKind::Vulkan2D:
        return "Vulkan2D";
    }
    return "Unknown";
}

auto ensure_within_root(AppRootPathView root,
                        ConcretePathView path) -> SP::Expected<void> {
    auto status = SP::App::ensure_within_app(root, path);
    if (!status) {
        return std::unexpected(status.error());
    }
    return {};
}

} // namespace

void Window::TestHooks::SetBeforePresentHook(BeforePresentHook hook) {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = std::move(hook);
}

void Window::TestHooks::ResetBeforePresentHook() {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = nullptr;
}

auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& targetPath,
                                PathWindowView::PresentStats const& stats,
                                PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool> {
    return maybe_schedule_auto_render_impl(space, targetPath, stats, policy);
}

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, maybeRelative);
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath> {
    return SP::App::derive_target_base(root, rendererPath, targetPath);
}

namespace Scene {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SceneParams const& params) -> SP::Expected<ScenePath> {
    if (auto status = ensure_identifier(params.name, "scene name"); !status) {
        return std::unexpected(status.error());
    }

    auto resolved = combine_relative(appRoot, std::string("scenes/") + params.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto metaNamePath = make_scene_meta(ScenePath{resolved->getPath()}, "name");
    auto existing = read_optional<std::string>(space, metaNamePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return ScenePath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, metaNamePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    auto metaDescPath = make_scene_meta(ScenePath{resolved->getPath()}, "description");
    if (auto status = replace_single<std::string>(space, metaDescPath, params.description); !status) {
        return std::unexpected(status.error());
    }

    return ScenePath{resolved->getPath()};
}

auto EnsureAuthoringRoot(PathSpace& /*space*/,
                          ScenePath const& scenePath) -> SP::Expected<void> {
    if (!scenePath.isValid()) {
        return std::unexpected(make_error("scene path is not valid",
                                          SP::Error::Code::InvalidPath));
    }
    if (auto status = ensure_contains_segment(ConcretePathView{scenePath.getPath()}, kScenesSegment); !status) {
        return status;
    }
    return {};
}

auto PublishRevision(PathSpace& space,
                      ScenePath const& scenePath,
                      SceneRevisionDesc const& revision,
                      std::span<std::byte const> drawableBucket,
                      std::span<std::byte const> metadata) -> SP::Expected<void> {
    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return status;
    }

    auto record = to_record(revision);
    auto revisionStr = format_revision(revision.revision);
    auto revisionBase = make_revision_base(scenePath, revisionStr);

    if (auto status = replace_single<SceneRevisionRecord>(space, revisionBase + "/desc", record); !status) {
        return status;
    }
    if (auto status = replace_single<std::vector<std::uint8_t>>(space,
                                                                revisionBase + "/drawable_bucket",
                                                                bytes_from_span(drawableBucket)); !status) {
        return status;
    }
    if (auto status = replace_single<std::vector<std::uint8_t>>(space,
                                                                revisionBase + "/metadata",
                                                                bytes_from_span(metadata)); !status) {
        return status;
    }

    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    if (auto status = replace_single<uint64_t>(space, currentRevisionPath, revision.revision); !status) {
        return status;
    }

    return {};
}

auto ReadCurrentRevision(PathSpace const& space,
                          ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc> {
    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    auto revisionValue = read_value<uint64_t>(space, currentRevisionPath);
    if (!revisionValue) {
        return std::unexpected(revisionValue.error());
    }

    auto revisionStr = format_revision(*revisionValue);
    auto descPath = make_revision_base(scenePath, revisionStr) + "/desc";
    auto record = read_value<SceneRevisionRecord>(space, descPath);
    if (!record) {
        return std::unexpected(record.error());
    }
    return from_record(*record);
}

auto WaitUntilReady(PathSpace& space,
                     ScenePath const& scenePath,
                     std::chrono::milliseconds timeout) -> SP::Expected<void> {
    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    auto result = read_value<uint64_t>(space, currentRevisionPath, SP::Out{} & SP::Block{timeout});
    if (!result) {
        return std::unexpected(result.error());
    }
    (void)result;
    return {};
}

auto HitTest(PathSpace& space,
             ScenePath const& scenePath,
             HitTestRequest const& request) -> SP::Expected<HitTestResult> {
    auto sceneRoot = derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }

    auto revision = ReadCurrentRevision(space, scenePath);
    if (!revision) {
        return std::unexpected(revision.error());
    }

    auto revisionStr = format_revision(revision->revision);
    auto revisionBase = make_revision_base(scenePath, revisionStr);
    auto bucket = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revisionBase);
    if (!bucket) {
        return std::unexpected(bucket.error());
    }

    std::optional<std::string> auto_render_target;
    if (request.schedule_render) {
        if (!request.auto_render_target) {
            return std::unexpected(make_error("auto render target required when scheduling render",
                                              SP::Error::Code::InvalidPath));
        }
        auto targetRoot = derive_app_root_for(ConcretePathView{request.auto_render_target->getPath()});
        if (!targetRoot) {
            return std::unexpected(targetRoot.error());
        }
        if (targetRoot->getPath() != sceneRoot->getPath()) {
            return std::unexpected(make_error("auto render target must belong to the same application as the scene",
                                              SP::Error::Code::InvalidPath));
        }
        auto_render_target = request.auto_render_target->getPath();
    }

    auto order = detail::build_draw_order(*bucket);
    HitTestResult result{};
    auto max_results = request.max_results == 0 ? std::size_t{1} : request.max_results;
    bool const enqueue_render = request.schedule_render && auto_render_target.has_value();
    bool render_enqueued = false;

    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        std::size_t drawable_index = *it;
        if (drawable_index >= bucket->drawable_ids.size()) {
            continue;
        }
        if (drawable_index < bucket->visibility.size()
            && bucket->visibility[drawable_index] == 0) {
            continue;
        }
        if (!detail::point_inside_clip(request.x, request.y, *bucket, drawable_index)) {
            continue;
        }
        if (!detail::point_inside_bounds(request.x, request.y, *bucket, drawable_index)) {
            continue;
        }

        HitCandidate candidate{};
        candidate.target.drawable_id = bucket->drawable_ids[drawable_index];

        if (drawable_index < bucket->authoring_map.size()) {
            auto const& author = bucket->authoring_map[drawable_index];
            candidate.target.authoring_node_id = author.authoring_node_id;
            candidate.target.drawable_index_within_node = author.drawable_index_within_node;
            candidate.target.generation = author.generation;
            candidate.focus_chain = detail::build_focus_chain(author.authoring_node_id);
            candidate.focus_path.reserve(candidate.focus_chain.size());
            for (std::size_t i = 0; i < candidate.focus_chain.size(); ++i) {
                FocusEntry entry;
                entry.path = candidate.focus_chain[i];
                entry.focusable = (i == 0);
                candidate.focus_path.push_back(std::move(entry));
            }
        }

        candidate.position.scene_x = request.x;
        candidate.position.scene_y = request.y;
        if (drawable_index < bucket->bounds_boxes.size()
            && (drawable_index >= bucket->bounds_box_valid.size()
                || bucket->bounds_box_valid[drawable_index] != 0)) {
            auto const& box = bucket->bounds_boxes[drawable_index];
            candidate.position.local_x = request.x - box.min[0];
            candidate.position.local_y = request.y - box.min[1];
            candidate.position.has_local = true;
        }

        if (enqueue_render && !render_enqueued) {
            auto status = enqueue_auto_render_event(space,
                                                    *auto_render_target,
                                                    "hit-test",
                                                    0);
            if (!status) {
                return std::unexpected(status.error());
            }
            render_enqueued = true;
        }

        result.hits.push_back(std::move(candidate));
        if (result.hits.size() >= max_results) {
            break;
        }
    }

    if (!result.hits.empty()) {
        result.hit = true;
        auto const& primary = result.hits.front();
        result.target = primary.target;
        result.position = primary.position;
        result.focus_chain = primary.focus_chain;
        result.focus_path = primary.focus_path;
    }

    return result;
}

auto MarkDirty(PathSpace& space,
               ScenePath const& scenePath,
               DirtyKind kinds,
               std::chrono::system_clock::time_point timestamp) -> SP::Expected<std::uint64_t> {
    if (kinds == DirtyKind::None) {
        return std::unexpected(make_error("dirty kinds must not be empty", SP::Error::Code::InvalidType));
    }

    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return std::unexpected(status.error());
    }

    auto statePath = dirty_state_path(scenePath);
    auto queuePath = dirty_queue_path(scenePath);

    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }

    DirtyState state{};
    if (existing->has_value()) {
        state = **existing;
    }

    auto seq = g_scene_dirty_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    auto combined_mask = dirty_mask(state.pending) | dirty_mask(kinds);
    state.pending = make_dirty_kind(combined_mask);
    state.sequence = seq;
    state.timestamp_ms = to_epoch_ms(timestamp);

    if (auto status = replace_single<DirtyState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }

    DirtyEvent event{
        .sequence = seq,
        .kinds = kinds,
        .timestamp_ms = state.timestamp_ms,
    };
    auto inserted = space.insert(queuePath, event);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return seq;
}

auto ClearDirty(PathSpace& space,
                ScenePath const& scenePath,
                DirtyKind kinds) -> SP::Expected<void> {
    if (kinds == DirtyKind::None) {
        return {};
    }

    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return std::unexpected(status.error());
    }

    auto statePath = dirty_state_path(scenePath);
    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return {};
    }

    auto state = **existing;
    auto current_mask = dirty_mask(state.pending);
    auto cleared_mask = current_mask & ~dirty_mask(kinds);
    if (cleared_mask == current_mask) {
        return {};
    }

    state.pending = make_dirty_kind(cleared_mask);
    state.timestamp_ms = to_epoch_ms(std::chrono::system_clock::now());

    if (auto status = replace_single<DirtyState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto ReadDirtyState(PathSpace const& space,
                    ScenePath const& scenePath) -> SP::Expected<DirtyState> {
    auto statePath = dirty_state_path(scenePath);
    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return DirtyState{};
    }
    return **existing;
}

auto TakeDirtyEvent(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<DirtyEvent> {
    auto queuePath = dirty_queue_path(scenePath);
    auto event = space.take<DirtyEvent>(queuePath, SP::Out{} & SP::Block{timeout});
    if (!event) {
        return std::unexpected(event.error());
    }
    return *event;
}

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
            AppRootPathView appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath> {
    if (auto status = ensure_identifier(params.name, "renderer name"); !status) {
        return std::unexpected(status.error());
    }

    auto resolved = combine_relative(appRoot, std::string("renderers/") + params.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto metaBase = std::string(resolved->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        auto ensureDescription = read_optional<std::string>(space, metaBase + "/description");
        if (!ensureDescription) {
            return std::unexpected(ensureDescription.error());
        }
        if (!ensureDescription->has_value()) {
            auto descStatus = replace_single<std::string>(space, metaBase + "/description", params.description);
            if (!descStatus) {
                return std::unexpected(descStatus.error());
            }
        }

        auto kindStatus = store_renderer_kind(space, metaBase + "/kind", params.kind);
        if (!kindStatus) {
            return std::unexpected(kindStatus.error());
        }

        return RendererPath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/description", params.description); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = store_renderer_kind(space, metaBase + "/kind", params.kind); !status) {
        return std::unexpected(status.error());
    }

    return RendererPath{resolved->getPath()};
}

auto CreateHtmlTarget(PathSpace& space,
                      AppRootPathView appRoot,
                      RendererPath const& rendererPath,
                      HtmlTargetParams const& params) -> SP::Expected<HtmlTargetPath> {
    if (auto status = ensure_identifier(params.name, "html target name"); !status) {
        return std::unexpected(status.error());
    }
    if (params.scene.empty()) {
        return std::unexpected(make_error("html target scene must not be empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererRoot = derive_app_root_for(ConcretePathView{rendererPath.getPath()});
    if (!rendererRoot) {
        return std::unexpected(rendererRoot.error());
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{rendererRoot->getPath()},
                                                       params.scene);
    if (!sceneAbsolute) {
        return std::unexpected(sceneAbsolute.error());
    }

    if (auto status = same_app(ConcretePathView{sceneAbsolute->getPath()},
                               ConcretePathView{rendererPath.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    auto rendererView = SP::App::AppRootPathView{rendererRoot->getPath()};
    auto rendererRelative = relative_to_root(rendererView, ConcretePathView{rendererPath.getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string targetRelative = *rendererRelative;
    if (!targetRelative.empty()) {
        targetRelative.push_back('/');
    }
    targetRelative.append("targets/html/");
    targetRelative.append(params.name);

    auto targetAbsolute = combine_relative(rendererView, std::move(targetRelative));
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto base = targetAbsolute->getPath();
    if (auto status = replace_single<HtmlTargetDesc>(space, base + "/desc", params.desc); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, base + "/scene", params.scene); !status) {
        return std::unexpected(status.error());
    }

    return HtmlTargetPath{base};
}

auto ResolveTargetBase(PathSpace const& /*space*/,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    if (auto status = ensure_non_empty(targetSpec, "target spec"); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = SP::App::ensure_within_app(appRoot, ConcretePathView{rendererPath.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    std::string spec{targetSpec};
    if (!spec.empty() && spec.front() == '/') {
        auto resolved = combine_relative(appRoot, std::move(spec));
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        return *resolved;
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath.getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string combined = *rendererRelative;
    if (!combined.empty()) {
        combined.push_back('/');
    }
    combined.append(spec);

    auto resolved = combine_relative(appRoot, std::move(combined));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return *resolved;
}

auto UpdateSettings(PathSpace& space,
                     ConcretePathView targetPath,
                     RenderSettings const& settings) -> SP::Expected<void> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return replace_single<RenderSettings>(space, settingsPath, settings);
}

auto ReadSettings(PathSpace const& space,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return read_value<RenderSettings>(space, settingsPath);
}

auto rectangles_touch_or_overlap(DirtyRectHint const& a, DirtyRectHint const& b) -> bool {
    auto overlaps_axis = [](float min_a, float max_a, float min_b, float max_b) {
        return !(max_a < min_b || min_a > max_b);
    };
    return overlaps_axis(a.min_x, a.max_x, b.min_x, b.max_x)
        && overlaps_axis(a.min_y, a.max_y, b.min_y, b.max_y);
}

auto merge_hints(std::vector<DirtyRectHint>& hints,
                 float tile_size,
                 float width,
                 float height) -> void {
    if (hints.empty()) {
        return;
    }
    auto fallback_to_full_surface = [&]() {
        hints.clear();
        if (width <= 0.0f || height <= 0.0f) {
            return;
        }
        hints.push_back(DirtyRectHint{
            .min_x = 0.0f,
            .min_y = 0.0f,
            .max_x = width,
            .max_y = height,
        });
    };
    if (width <= 0.0f || height <= 0.0f) {
        hints.clear();
        return;
    }
    bool merged_any = true;
    while (merged_any) {
        merged_any = false;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            for (std::size_t j = i + 1; j < hints.size(); ++j) {
                if (rectangles_touch_or_overlap(hints[i], hints[j])) {
                    hints[i].min_x = std::min(hints[i].min_x, hints[j].min_x);
                    hints[i].min_y = std::min(hints[i].min_y, hints[j].min_y);
                    hints[i].max_x = std::max(hints[i].max_x, hints[j].max_x);
                    hints[i].max_y = std::max(hints[i].max_y, hints[j].max_y);
                    hints.erase(hints.begin() + static_cast<std::ptrdiff_t>(j));
                    merged_any = true;
                    break;
                }
            }
            if (merged_any) {
                break;
            }
        }
    }
    constexpr std::size_t kMaxStoredHints = 128;
    if (hints.size() > kMaxStoredHints) {
        fallback_to_full_surface();
        return;
    }
    double total_area = 0.0;
    for (auto const& rect : hints) {
        auto const width_px = std::max(0.0f, rect.max_x - rect.min_x);
        auto const height_px = std::max(0.0f, rect.max_y - rect.min_y);
        total_area += static_cast<double>(width_px) * static_cast<double>(height_px);
    }
    auto const surface_area = static_cast<double>(width) * static_cast<double>(height);
    if (surface_area > 0.0 && total_area >= surface_area * 0.9) {
        fallback_to_full_surface();
        return;
    }

    auto approximately = [tile_size](float a, float b) {
        auto const epsilon = std::max(tile_size * 0.001f, 1e-5f);
        return std::fabs(a - b) <= epsilon;
    };

    for (auto& rect : hints) {
        if (approximately(rect.min_x, 0.0f)) {
            rect.min_x = 0.0f;
        }
        if (approximately(rect.min_y, 0.0f)) {
            rect.min_y = 0.0f;
        }
        if (approximately(rect.max_x, width)) {
            rect.max_x = width;
        }
        if (approximately(rect.max_y, height)) {
            rect.max_y = height;
        }
    }
    std::sort(hints.begin(),
              hints.end(),
              [](DirtyRectHint const& lhs, DirtyRectHint const& rhs) {
                  if (lhs.min_y == rhs.min_y) {
                      return lhs.min_x < rhs.min_x;
                  }
                  return lhs.min_y < rhs.min_y;
              });
}

auto snap_hint_to_tiles(DirtyRectHint hint, float tile_size) -> DirtyRectHint {
    if (tile_size <= 1.0f) {
        return hint;
    }
    auto align_down = [tile_size](float value) {
        return std::floor(value / tile_size) * tile_size;
    };
    auto align_up = [tile_size](float value) {
        return std::ceil(value / tile_size) * tile_size;
    };
    DirtyRectHint snapped{};
    snapped.min_x = align_down(hint.min_x);
    snapped.min_y = align_down(hint.min_y);
    snapped.max_x = align_up(hint.max_x);
    snapped.max_y = align_up(hint.max_y);
    if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
        return {};
    }
    return snapped;
}

auto SubmitDirtyRects(PathSpace& space,
                      ConcretePathView targetPath,
                      std::span<DirtyRectHint const> rects) -> SP::Expected<void> {
    if (rects.empty()) {
        return SP::Expected<void>{};
    }
    auto hintsPath = std::string(targetPath.getPath()) + "/hints/dirtyRects";

    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto desc = read_value<SurfaceDesc>(space, descPath);
    if (!desc) {
        return std::unexpected(desc.error());
    }
    auto const tile_size = static_cast<float>(std::max(1, (*desc).progressive_tile_size_px));
    auto const width = static_cast<float>(std::max(0, (*desc).size_px.width));
    auto const height = static_cast<float>(std::max(0, (*desc).size_px.height));

    std::vector<DirtyRectHint> stored;
    stored.reserve(rects.size());
    for (auto const& hint : rects) {
        auto snapped = snap_hint_to_tiles(hint, tile_size);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        snapped.min_x = std::clamp(snapped.min_x, 0.0f, width);
        snapped.min_y = std::clamp(snapped.min_y, 0.0f, height);
        snapped.max_x = std::clamp(snapped.max_x, 0.0f, width);
        snapped.max_y = std::clamp(snapped.max_y, 0.0f, height);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        stored.push_back(snapped);
    }
    merge_hints(stored, tile_size, width, height);
    return replace_single<std::vector<DirtyRectHint>>(space, hintsPath, stored);
}

auto TriggerRender(PathSpace& space,
                   ConcretePathView targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto surfaceDesc = read_value<SurfaceDesc>(space, descPath);
    if (!surfaceDesc) {
        return std::unexpected(surfaceDesc.error());
    }

    auto const targetStr = std::string(targetPath.getPath());
    auto targetsPos = targetStr.find("/targets/");
    if (targetsPos == std::string::npos) {
        return std::unexpected(make_error("target path '" + targetStr + "' missing /targets/ segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto rendererPathStr = targetStr.substr(0, targetsPos);
    if (rendererPathStr.empty()) {
        return std::unexpected(make_error("renderer path derived from target is empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererKind = read_renderer_kind(space, rendererPathStr + "/meta/kind");
    if (!rendererKind) {
        return std::unexpected(rendererKind.error());
    }

    auto effectiveKind = *rendererKind;
#if !PATHSPACE_UI_METAL
    if (effectiveKind == RendererKind::Metal2D) {
        effectiveKind = RendererKind::Software2D;
    }
#else
    if (effectiveKind == RendererKind::Metal2D
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        effectiveKind = RendererKind::Software2D;
    }
#endif

    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{std::string{targetPath.getPath()}},
        .renderer_path = SP::ConcretePathString{rendererPathStr},
        .target_desc = *surfaceDesc,
        .settings = settings,
        .renderer_kind = effectiveKind,
    };

    auto surface_key = std::string(context.target_path.getPath());
    auto& surface = acquire_surface(surface_key, context.target_desc);

#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context.renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(surface_key, context.target_desc);
    }
    auto stats = render_into_target(space, context, surface, metal_surface);
#else
    auto stats = render_into_target(space, context, surface);
#endif
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

auto RenderHtml(PathSpace& space,
                ConcretePathView targetPath) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath());
    uint64_t rendered_revision = 0;

    auto report_error = [&](SP::Error const& error, std::string detail = std::string{}) -> SP::Expected<void> {
        Diagnostics::PathSpaceError diag{};
        diag.code = static_cast<std::int32_t>(error.code);
        diag.severity = Diagnostics::PathSpaceError::Severity::Recoverable;
        diag.message = error.message.value_or("RenderHtml failed");
        diag.detail = std::move(detail);
        diag.path = base;
        diag.revision = rendered_revision;
        (void)Diagnostics::WriteTargetError(space, targetPath, diag);
        return std::unexpected(error);
    };

    auto targetRoot = derive_app_root_for(targetPath);
    if (!targetRoot) {
        return report_error(targetRoot.error(), "derive_app_root_for");
    }

    auto descPath = base + "/desc";
    auto desc = read_value<HtmlTargetDesc>(space, descPath);
    if (!desc) {
        return report_error(desc.error(), "read html desc");
    }

    auto sceneRel = read_value<std::string>(space, base + "/scene");
    if (!sceneRel) {
        return report_error(sceneRel.error(), "read html scene binding");
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{targetRoot->getPath()},
                                                       *sceneRel);
    if (!sceneAbsolute) {
        return report_error(sceneAbsolute.error(), "resolve scene path");
    }

    auto sceneRevision = Scene::ReadCurrentRevision(space, ScenePath{sceneAbsolute->getPath()});
    if (!sceneRevision) {
        return report_error(sceneRevision.error(), "read current scene revision");
    }
    rendered_revision = sceneRevision->revision;

    auto revisionBase = std::string(sceneAbsolute->getPath()) + "/builds/" + format_revision(sceneRevision->revision);
    auto bucket = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revisionBase);
    if (!bucket) {
        return report_error(bucket.error(), "decode scene snapshot");
    }

    Html::EmitOptions options{};
    options.max_dom_nodes = desc->max_dom_nodes;
    options.prefer_dom = desc->prefer_dom;
    options.allow_canvas_fallback = desc->allow_canvas_fallback;
    options.resolve_asset =
        [&](std::string_view logical_path,
            std::uint64_t /*fingerprint*/,
            Html::AssetKind /*kind*/) -> SP::Expected<Html::Asset> {
            Html::Asset asset{};
            asset.logical_path = std::string(logical_path);

            if (!is_safe_asset_path(asset.logical_path)) {
                return std::unexpected(make_error("html asset logical path unsafe: " + asset.logical_path,
                                                  SP::Error::Code::InvalidPath));
            }

            std::string full_path = revisionBase;
            if (asset.logical_path.rfind("assets/", 0) == 0) {
                full_path.append("/").append(asset.logical_path);
            } else {
                full_path.append("/assets/").append(asset.logical_path);
            }

            auto bytes = space.read<std::vector<std::uint8_t>>(full_path);
            if (!bytes) {
                auto const error = bytes.error();
                std::string message = "read html asset '" + asset.logical_path + "'";
                if (error.message) {
                    message.append(": ").append(*error.message);
                }
                return std::unexpected(make_error(std::move(message), error.code));
            }

            asset.bytes = std::move(*bytes);
            asset.mime_type = guess_mime_type(asset.logical_path);
            if (asset.mime_type.empty()) {
                asset.mime_type = "application/octet-stream";
            }
            return asset;
        };

    {
        auto fontManifestPath = revisionBase + "/assets/font-manifest";
        auto fontsValue = read_optional<std::vector<Html::Asset>>(space, fontManifestPath);
        if (!fontsValue) {
            return report_error(fontsValue.error(), "read html font manifest");
        }
        if (fontsValue->has_value()) {
            std::unordered_set<std::string> unique_fonts;
            for (auto const& font : **fontsValue) {
                if (font.logical_path.empty()) {
                    continue;
                }
                if (unique_fonts.insert(font.logical_path).second) {
                    options.font_logical_paths.push_back(font.logical_path);
                }
            }
        }
    }

    Html::Adapter adapter;
    auto emitted = adapter.emit(*bucket, options);
    if (!emitted) {
        return report_error(emitted.error(), "emit html adapter output");
    }

    if (auto status = hydrate_html_assets(space, revisionBase, emitted->assets); !status) {
        return report_error(status.error(), "hydrate html assets");
    }

    auto htmlBase = base + "/output/v1/html";

    // Track existing asset manifest to clear stale blobs/metadata.
    std::vector<std::string> previous_asset_manifest;
    auto manifestPath = htmlBase + "/assets/manifest";
    if (auto existingManifest = read_optional<std::vector<std::string>>(space, manifestPath); !existingManifest) {
        return report_error(existingManifest.error(), "read html asset manifest");
    } else if (existingManifest->has_value()) {
        previous_asset_manifest = std::move(**existingManifest);
    }

    std::vector<std::string> current_manifest;
    current_manifest.reserve(emitted->assets.size());
    for (auto const& asset : emitted->assets) {
        current_manifest.push_back(asset.logical_path);
    }

    std::unordered_set<std::string> current_asset_set{current_manifest.begin(), current_manifest.end()};
    std::unordered_set<std::string> previous_asset_set{previous_asset_manifest.begin(), previous_asset_manifest.end()};

    auto assetsDataBase = htmlBase + "/assets/data";
    auto assetsMetaBase = htmlBase + "/assets/meta";

    // Remove stale asset payloads.
    for (auto const& logical : previous_asset_set) {
        if (current_asset_set.find(logical) != current_asset_set.end()) {
            continue;
        }
        auto const dataPath = assetsDataBase + "/" + logical;
        if (auto status = drain_queue<std::vector<std::uint8_t>>(space, dataPath); !status) {
            return report_error(status.error(), "clear stale html asset bytes");
        }
        auto const mimePath = assetsMetaBase + "/" + logical;
        if (auto status = drain_queue<std::string>(space, mimePath); !status) {
            return report_error(status.error(), "clear stale html asset mime");
        }
    }

    for (auto const& asset : emitted->assets) {
        auto const dataPath = assetsDataBase + "/" + asset.logical_path;
        if (auto status = replace_single<std::vector<std::uint8_t>>(space, dataPath, asset.bytes); !status) {
            return report_error(status.error(), "write html asset bytes");
        }
        auto const mimePath = assetsMetaBase + "/" + asset.logical_path;
        if (auto status = replace_single<std::string>(space, mimePath, asset.mime_type); !status) {
            return report_error(status.error(), "write html asset mime");
        }
    }

    if (current_manifest.empty()) {
        if (auto status = drain_queue<std::vector<std::string>>(space, manifestPath); !status) {
            return report_error(status.error(), "clear html asset manifest");
        }
    } else {
        if (auto status = replace_single<std::vector<std::string>>(space, manifestPath, current_manifest); !status) {
            return report_error(status.error(), "write html asset manifest");
        }
    }

    if (auto status = replace_single<uint64_t>(space, htmlBase + "/revision", sceneRevision->revision); !status) {
        return report_error(status.error(), "write html revision");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/dom", emitted->dom); !status) {
        return report_error(status.error(), "write dom");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/css", emitted->css); !status) {
        return report_error(status.error(), "write css");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/commands", emitted->canvas_commands); !status) {
        return report_error(status.error(), "write canvas commands");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/usedCanvasFallback", emitted->used_canvas_fallback); !status) {
        return report_error(status.error(), "write canvas fallback flag");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/commandCount",
                                               static_cast<uint64_t>(emitted->canvas_replay_commands.size())); !status) {
        return report_error(status.error(), "write command count");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/domNodeCount",
                                               static_cast<uint64_t>(bucket->drawable_ids.size())); !status) {
        return report_error(status.error(), "write dom node count");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/assetCount",
                                               static_cast<uint64_t>(emitted->assets.size())); !status) {
        return report_error(status.error(), "write asset count");
    }
    if (auto status = replace_single<std::vector<Html::Asset>>(space,
                                                               htmlBase + "/assets",
                                                               emitted->assets); !status) {
        return report_error(status.error(), "write assets");
    }
    if (auto status = replace_single<uint64_t>(space,
                                               htmlBase + "/options/maxDomNodes",
                                               static_cast<uint64_t>(desc->max_dom_nodes)); !status) {
        return report_error(status.error(), "write maxDomNodes");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/options/preferDom", desc->prefer_dom); !status) {
        return report_error(status.error(), "write preferDom");
    }
    if (auto status = replace_single<bool>(space, htmlBase + "/options/allowCanvasFallback", desc->allow_canvas_fallback); !status) {
        return report_error(status.error(), "write allowCanvasFallback");
    }
    auto mode = emitted->used_canvas_fallback ? std::string{"canvas"} : std::string{"dom"};
    if (auto status = replace_single<std::string>(space, htmlBase + "/mode", mode); !status) {
        return report_error(status.error(), "write mode");
    }
    if (auto status = replace_single<std::string>(space, htmlBase + "/metadata/activeMode", mode); !status) {
        return report_error(status.error(), "write active mode metadata");
    }

    if (auto status = Diagnostics::ClearTargetError(space, targetPath); !status) {
        return status;
    }
    return {};
}

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    if (auto status = ensure_identifier(params.name, "surface name"); !status) {
        return std::unexpected(status.error());
    }

    auto surfacePath = combine_relative(appRoot, std::string("surfaces/") + params.name);
    if (!surfacePath) {
        return std::unexpected(surfacePath.error());
    }

    auto rendererPath = resolve_renderer_spec(appRoot, params.renderer);
    if (!rendererPath) {
        return std::unexpected(rendererPath.error());
    }

    if (auto status = ensure_contains_segment(ConcretePathView{surfacePath->getPath()}, kSurfacesSegment); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_contains_segment(ConcretePathView{rendererPath->getPath()}, kRenderersSegment); !status) {
        return std::unexpected(status.error());
    }

    auto metaBase = std::string(surfacePath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return SurfacePath{surfacePath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }

    auto descPath = std::string(surfacePath->getPath()) + "/desc";
    if (auto status = store_desc(space, descPath, params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath->getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    auto rendererField = std::string(surfacePath->getPath()) + "/renderer";
    if (auto status = replace_single<std::string>(space, rendererField, *rendererRelative); !status) {
        return std::unexpected(status.error());
    }

    auto targetSpec = std::string("targets/surfaces/") + params.name;
    auto targetBase = Renderer::ResolveTargetBase(space, appRoot, *rendererPath, targetSpec);
    if (!targetBase) {
        return std::unexpected(targetBase.error());
    }

    auto targetRelative = relative_to_root(appRoot, ConcretePathView{targetBase->getPath()});
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    if (auto status = store_desc(space, std::string(targetBase->getPath()) + "/desc", params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto targetField = std::string(surfacePath->getPath()) + "/target";
    if (auto status = replace_single<std::string>(space, targetField, *targetRelative); !status) {
        return std::unexpected(status.error());
    }

    return SurfacePath{surfacePath->getPath()};
}

auto SetScene(PathSpace& space,
               SurfacePath const& surfacePath,
               ScenePath const& scenePath) -> SP::Expected<void> {
    auto surfaceRoot = derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    auto sceneRoot = derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }
    if (surfaceRoot->getPath() != sceneRoot->getPath()) {
        return std::unexpected(make_error("surface and scene belong to different applications",
                                          SP::Error::Code::InvalidPath));
    }

    auto sceneRelative = relative_to_root(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                          ConcretePathView{scenePath.getPath()});
    if (!sceneRelative) {
        return std::unexpected(sceneRelative.error());
    }

    auto sceneField = std::string(surfacePath.getPath()) + "/scene";
    if (auto status = replace_single<std::string>(space, sceneField, *sceneRelative); !status) {
        return status;
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = read_value<std::string>(space, targetField);
    if (!targetRelative) {
        if (targetRelative.error().code == SP::Error::Code::NoObjectFound) {
            return std::unexpected(make_error("surface missing target binding",
                                              SP::Error::Code::InvalidPath));
        }
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                                        *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto targetScenePath = targetAbsolute->getPath() + "/scene";
    return replace_single<std::string>(space, targetScenePath, *sceneRelative);
}

auto RenderOnce(PathSpace& space,
                 SurfacePath const& surfacePath,
                 std::optional<RenderSettings> settingsOverride) -> SP::Expected<SP::FutureAny> {
    auto context = prepare_surface_render_context(space, surfacePath, settingsOverride);
    if (!context) {
        return std::unexpected(context.error());
    }

    auto& surface = acquire_surface(std::string(context->target_path.getPath()), context->target_desc);
#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(std::string(context->target_path.getPath()),
                                               context->target_desc);
    }
    auto stats = render_into_target(space,
                                    *context,
                                    surface,
                                    metal_surface);
#else
    auto stats = render_into_target(space, *context, surface);
#endif
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath> {
    if (auto status = ensure_identifier(params.name, "window name"); !status) {
        return std::unexpected(status.error());
    }

    auto windowPath = combine_relative(appRoot, std::string("windows/") + params.name);
    if (!windowPath) {
        return std::unexpected(windowPath.error());
    }

    auto metaBase = std::string(windowPath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return WindowPath{windowPath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/title", params.title); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/width", params.width); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/height", params.height); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<float>(space, metaBase + "/scale", params.scale); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/background", params.background); !status) {
        return std::unexpected(status.error());
    }

    return WindowPath{windowPath->getPath()};
}

auto AttachSurface(PathSpace& space,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    if (auto status = same_app(ConcretePathView{windowPath.getPath()},
                               ConcretePathView{surfacePath.getPath()}); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    auto surfaceRelative = relative_to_root(SP::App::AppRootPathView{windowRoot->getPath()},
                                            ConcretePathView{surfacePath.getPath()});
    if (!surfaceRelative) {
        return std::unexpected(surfaceRelative.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    if (auto status = replace_single<std::string>(space, viewBase + "/surface", *surfaceRelative); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, viewBase + "/htmlTarget", std::string{}); !status) {
        return status;
    }
    (void)drain_queue<std::string>(space, viewBase + "/windowTarget");
    return {};
}

auto AttachHtmlTarget(PathSpace& space,
                       WindowPath const& windowPath,
                       std::string_view viewName,
                       HtmlTargetPath const& targetPath) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    if (auto status = same_app(ConcretePathView{windowPath.getPath()},
                               ConcretePathView{targetPath.getPath()}); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    auto targetRelative = relative_to_root(SP::App::AppRootPathView{windowRoot->getPath()},
                                           ConcretePathView{targetPath.getPath()});
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    // Ensure the target exists by validating the descriptor.
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    if (auto desc = read_optional<HtmlTargetDesc>(space, descPath); !desc) {
        return std::unexpected(desc.error());
    } else if (!desc->has_value()) {
        return std::unexpected(make_error("html target descriptor missing",
                                          SP::Error::Code::InvalidPath));
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    if (auto status = replace_single<std::string>(space, viewBase + "/htmlTarget", *targetRelative); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, viewBase + "/surface", std::string{}); !status) {
        return status;
    }
    (void)drain_queue<std::string>(space, viewBase + "/windowTarget");
    return {};
}

auto Present(PathSpace& space,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<WindowPresentResult> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return std::unexpected(status.error());
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    auto surfaceRel = read_optional<std::string>(space, viewBase + "/surface");
    if (!surfaceRel) {
        return std::unexpected(surfaceRel.error());
    }
    auto htmlRel = read_optional<std::string>(space, viewBase + "/htmlTarget");
    if (!htmlRel) {
        return std::unexpected(htmlRel.error());
    }

    auto surfaceBinding = surfaceRel->has_value() ? **surfaceRel : std::string{};
    auto htmlBinding = htmlRel->has_value() ? **htmlRel : std::string{};
    bool hasSurface = !surfaceBinding.empty();
    bool hasHtml = !htmlBinding.empty();

    if (hasSurface && hasHtml) {
        return std::unexpected(make_error("view is bound to both surface and html target",
                                          SP::Error::Code::InvalidPath));
    }
    if (!hasSurface && !hasHtml) {
        return std::unexpected(make_error("view is not bound to a presentable target",
                                          SP::Error::Code::InvalidPath));
    }

    if (hasHtml) {
        auto htmlPath = SP::App::resolve_app_relative(SP::App::AppRootPathView{windowRoot->getPath()},
                                                      htmlBinding);
        if (!htmlPath) {
            return std::unexpected(htmlPath.error());
        }

        auto html_render_start = std::chrono::steady_clock::now();
        auto renderStatus = Renderer::RenderHtml(space,
                                                 ConcretePathView{htmlPath->getPath()});
        if (!renderStatus) {
            return std::unexpected(renderStatus.error());
        }
        auto html_render_end = std::chrono::steady_clock::now();
        auto render_ms = std::chrono::duration<double, std::milli>(html_render_end - html_render_start).count();

        auto htmlBase = std::string(htmlPath->getPath()) + "/output/v1/html";

        auto revisionValue = read_optional<uint64_t>(space, htmlBase + "/revision");
        if (!revisionValue) {
            return std::unexpected(revisionValue.error());
        }
        uint64_t revision = revisionValue->value_or(0);

        auto read_string_or = [&](std::string const& path) -> SP::Expected<std::string> {
            auto value = read_optional<std::string>(space, path);
            if (!value) {
                return std::unexpected(value.error());
            }
            return value->value_or(std::string{});
        };

        auto domValue = read_string_or(htmlBase + "/dom");
        if (!domValue) {
            return std::unexpected(domValue.error());
        }
        auto cssValue = read_string_or(htmlBase + "/css");
        if (!cssValue) {
            return std::unexpected(cssValue.error());
        }
        auto commandsValue = read_string_or(htmlBase + "/commands");
        if (!commandsValue) {
            return std::unexpected(commandsValue.error());
        }
        auto modeValue = read_string_or(htmlBase + "/mode");
        if (!modeValue) {
            return std::unexpected(modeValue.error());
        }

        auto usedCanvasValue = read_optional<bool>(space, htmlBase + "/usedCanvasFallback");
        if (!usedCanvasValue) {
            return std::unexpected(usedCanvasValue.error());
        }
        bool usedCanvas = usedCanvasValue->value_or(false);

        std::vector<Html::Asset> assets;
        if (auto assetsValue = read_optional<std::vector<Html::Asset>>(space, htmlBase + "/assets"); !assetsValue) {
            return std::unexpected(assetsValue.error());
        } else if (assetsValue->has_value()) {
            assets = std::move(**assetsValue);
        }
        auto commonBase = std::string(htmlPath->getPath()) + "/output/v1/common";
        uint64_t next_frame_index = 1;
        if (auto previousFrame = read_optional<uint64_t>(space, commonBase + "/frameIndex"); !previousFrame) {
            return std::unexpected(previousFrame.error());
        } else if (previousFrame->has_value()) {
            next_frame_index = **previousFrame + 1;
        }

        PathWindowPresentStats presentStats{};
        presentStats.presented = true;
        presentStats.mode = PathWindowPresentMode::AlwaysLatestComplete;
        presentStats.auto_render_on_present = false;
        presentStats.vsync_aligned = false;
        presentStats.backend_kind = "Html";
        presentStats.frame.frame_index = next_frame_index;
        presentStats.frame.revision = revision;
        presentStats.frame.render_ms = render_ms;
        presentStats.present_ms = 0.0;
        presentStats.gpu_encode_ms = 0.0;
        presentStats.gpu_present_ms = 0.0;
        presentStats.wait_budget_ms = 0.0;
        presentStats.frame_age_ms = 0.0;
        presentStats.frame_age_frames = 0;

        PathWindowPresentPolicy htmlPolicy{};
        htmlPolicy.mode = PathWindowPresentMode::AlwaysLatestComplete;
        htmlPolicy.auto_render_on_present = false;
        htmlPolicy.vsync_align = false;
        htmlPolicy.staleness_budget = std::chrono::milliseconds{0};
        htmlPolicy.staleness_budget_ms_value = 0.0;
        htmlPolicy.frame_timeout = std::chrono::milliseconds{0};
        htmlPolicy.frame_timeout_ms_value = 0.0;
        htmlPolicy.max_age_frames = 0;

        WindowPresentResult result{};
        result.stats = presentStats;
        result.html = WindowPresentResult::HtmlPayload{
            .revision = revision,
            .dom = std::move(*domValue),
            .css = std::move(*cssValue),
            .commands = std::move(*commandsValue),
            .mode = std::move(*modeValue),
            .used_canvas_fallback = usedCanvas,
            .assets = std::move(assets),
        };

        if (auto status = Diagnostics::WritePresentMetrics(space,
                                                           ConcretePathView{htmlPath->getPath()},
                                                           presentStats,
                                                           htmlPolicy); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = Diagnostics::WriteResidencyMetrics(space,
                                                             ConcretePathView{htmlPath->getPath()},
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0); !status) {
            return std::unexpected(status.error());
        }

        if (auto status = Diagnostics::WriteWindowPresentMetrics(space,
                                                                  ConcretePathView{windowPath.getPath()},
                                                                  viewName,
                                                                  presentStats,
                                                                  htmlPolicy); !status) {
            return std::unexpected(status.error());
        }

        return result;
    }

    // Surface-backed present path.
    auto surfacePath = SP::App::resolve_app_relative(SP::App::AppRootPathView{windowRoot->getPath()},
                                                     surfaceBinding);
    if (!surfacePath) {
        return std::unexpected(surfacePath.error());
    }

    auto context = prepare_surface_render_context(space,
                                                  SurfacePath{surfacePath->getPath()},
                                                  std::nullopt);
    if (!context) {
        return std::unexpected(context.error());
    }

    auto policy = read_present_policy(space, viewBase);
    if (!policy) {
        return std::unexpected(policy.error());
    }
    auto presentPolicy = *policy;

    auto target_key = std::string(context->target_path.getPath());
    auto& surface = acquire_surface(target_key, context->target_desc);

#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(target_key, context->target_desc);
    }
#endif

#if PATHSPACE_UI_METAL
    auto renderStats = render_into_target(space,
                                          *context,
                                          surface,
                                          metal_surface);
#else
    auto renderStats = render_into_target(space, *context, surface);
#endif
    if (!renderStats) {
        return std::unexpected(renderStats.error());
    }
    auto const& stats_value = *renderStats;

    PathSurfaceMetal::TextureInfo metal_texture{};
    bool has_metal_texture = false;
#if PATHSPACE_UI_METAL
    if (metal_surface) {
        has_metal_texture = true;
    }
#endif

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    invoke_before_present_hook(surface, presentPolicy, dirty_tiles);

    PathWindowView presenter;
    std::vector<std::uint8_t> framebuffer;
    std::span<std::uint8_t> framebuffer_span{};
#if !defined(__APPLE__)
    if (framebuffer.empty()) {
        framebuffer.resize(surface.frame_bytes(), 0);
    }
    framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
#else
    if (framebuffer_span.empty()
        && (presentPolicy.capture_framebuffer || !surface.has_buffered())) {
        framebuffer.resize(surface.frame_bytes(), 0);
        framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
    }
#endif
    auto now = std::chrono::steady_clock::now();
    auto vsync_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(presentPolicy.frame_timeout);
    if (vsync_budget < std::chrono::steady_clock::duration::zero()) {
        vsync_budget = std::chrono::steady_clock::duration::zero();
    }
#if defined(__APPLE__)
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .vsync_align = presentPolicy.vsync_align,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_surface = metal_surface,
        .metal_texture = metal_texture,
        .allow_iosurface_sharing = true,
    };
#else
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .vsync_align = presentPolicy.vsync_align,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_surface = metal_surface,
        .metal_texture = metal_texture,
    };
#endif
    auto presentStats = presenter.present(surface, presentPolicy, request);
    if (renderStats) {
        presentStats.frame.frame_index = stats_value.frame_index;
        presentStats.frame.revision = stats_value.revision;
        presentStats.frame.render_ms = stats_value.render_ms;
        presentStats.damage_ms = stats_value.damage_ms;
        presentStats.encode_ms = stats_value.encode_ms;
        presentStats.progressive_copy_ms = stats_value.progressive_copy_ms;
        presentStats.publish_ms = stats_value.publish_ms;
        presentStats.drawable_count = stats_value.drawable_count;
        presentStats.progressive_tiles_updated = stats_value.progressive_tiles_updated;
        presentStats.progressive_bytes_copied = stats_value.progressive_bytes_copied;
        presentStats.progressive_tile_size = stats_value.progressive_tile_size;
        presentStats.progressive_workers_used = stats_value.progressive_workers_used;
        presentStats.progressive_jobs = stats_value.progressive_jobs;
        presentStats.encode_workers_used = stats_value.encode_workers_used;
        presentStats.encode_jobs = stats_value.encode_jobs;
        presentStats.progressive_tiles_dirty = stats_value.progressive_tiles_dirty;
        presentStats.progressive_tiles_total = stats_value.progressive_tiles_total;
        presentStats.progressive_tiles_skipped = stats_value.progressive_tiles_skipped;
        presentStats.progressive_tile_diagnostics_enabled = stats_value.progressive_tile_diagnostics_enabled;
        presentStats.backend_kind = renderer_kind_to_string(stats_value.backend_kind);
    }
#if defined(__APPLE__)
    auto copy_iosurface_into = [&](PathSurfaceSoftware::SharedIOSurface const& handle,
                                   std::vector<std::uint8_t>& out) {
        auto retained = handle.retain_for_external_use();
        if (!retained) {
            return;
        }
        bool locked = IOSurfaceLock(retained, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess;
        auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(retained));
        auto const row_bytes = IOSurfaceGetBytesPerRow(retained);
        auto const height = handle.height();
        auto const row_stride = surface.row_stride_bytes();
        auto const copy_bytes = std::min<std::size_t>(row_bytes, row_stride);
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));
        if (locked && base && copy_bytes > 0 && height > 0) {
            out.resize(total_bytes);
            for (int row = 0; row < height; ++row) {
                auto* dst = out.data() + static_cast<std::size_t>(row) * row_stride;
                auto const* src = base + static_cast<std::size_t>(row) * row_bytes;
                std::memcpy(dst, src, copy_bytes);
            }
        }
        if (locked) {
            IOSurfaceUnlock(retained, kIOSurfaceLockAvoidSync, nullptr);
        }
        CFRelease(retained);
    };

    if (presentStats.iosurface && presentStats.iosurface->valid()) {
        auto const row_stride = surface.row_stride_bytes();
        auto const height = presentStats.iosurface->height();
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));

        if (presentPolicy.capture_framebuffer) {
            copy_iosurface_into(*presentStats.iosurface, framebuffer);
        } else {
            framebuffer.clear();
        }
    }
    if (presentPolicy.capture_framebuffer
        && presentStats.buffered_frame_consumed
        && framebuffer.empty()) {
        auto required = static_cast<std::size_t>(surface.frame_bytes());
        framebuffer.resize(required);
        auto copy = surface.copy_buffered_frame(framebuffer);
        if (!copy) {
            framebuffer.clear();
        }
    }
#endif

    auto metricsBase = std::string(context->target_path.getPath()) + "/output/v1/common";
    std::uint64_t previous_age_frames = 0;
    if (auto previous = read_optional<uint64_t>(space, metricsBase + "/presentedAgeFrames"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_frames = **previous;
    }

    double previous_age_ms = 0.0;
    if (auto previous = read_optional<double>(space, metricsBase + "/presentedAgeMs"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_ms = **previous;
    }

    auto frame_timeout_ms = static_cast<double>(presentPolicy.frame_timeout.count());
    bool reuse_previous_frame = !presentStats.buffered_frame_consumed;
#if defined(__APPLE__)
    if (presentStats.used_iosurface) {
        reuse_previous_frame = false;
    }
#endif
    if (!reuse_previous_frame && presentStats.skipped) {
        reuse_previous_frame = true;
    }

    if (reuse_previous_frame) {
        presentStats.frame_age_frames = previous_age_frames + 1;
        presentStats.frame_age_ms = previous_age_ms + frame_timeout_ms;
    } else {
        presentStats.frame_age_frames = 0;
        presentStats.frame_age_ms = 0.0;
    }
    presentStats.stale = presentStats.frame_age_frames > presentPolicy.max_age_frames;

    if (auto scheduled = maybe_schedule_auto_render(space,
                                                    std::string(context->target_path.getPath()),
                                                    presentStats,
                                                    presentPolicy); !scheduled) {
        return std::unexpected(scheduled.error());
    }

    if (auto status = Diagnostics::WritePresentMetrics(space,
                                                       SP::ConcretePathStringView{context->target_path.getPath()},
                                                       presentStats,
                                                       presentPolicy); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = Diagnostics::WriteResidencyMetrics(space,
                                                         SP::ConcretePathStringView{context->target_path.getPath()},
                                                         stats_value.resource_cpu_bytes,
                                                         stats_value.resource_gpu_bytes,
                                                         context->settings.cache.cpu_soft_bytes,
                                                         context->settings.cache.cpu_hard_bytes,
                                                         context->settings.cache.gpu_soft_bytes,
                                                         context->settings.cache.gpu_hard_bytes); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = Diagnostics::WriteWindowPresentMetrics(space,
                                                              ConcretePathView{windowPath.getPath()},
                                                              viewName,
                                                              presentStats,
                                                              presentPolicy); !status) {
        return std::unexpected(status.error());
    }

    auto framebufferPath = std::string(context->target_path.getPath()) + "/output/v1/software/framebuffer";

    WindowPresentResult result{};
    result.stats = presentStats;
    if (presentPolicy.capture_framebuffer) {
        SoftwareFramebuffer stored_framebuffer{};
        stored_framebuffer.width = context->target_desc.size_px.width;
        stored_framebuffer.height = context->target_desc.size_px.height;
        stored_framebuffer.row_stride_bytes = static_cast<std::uint32_t>(surface.row_stride_bytes());
        stored_framebuffer.pixel_format = context->target_desc.pixel_format;
        stored_framebuffer.color_space = context->target_desc.color_space;
        stored_framebuffer.premultiplied_alpha = context->target_desc.premultiplied_alpha;
        stored_framebuffer.pixels = std::move(framebuffer);

        if (auto status = replace_single<SoftwareFramebuffer>(space, framebufferPath, stored_framebuffer); !status) {
            return std::unexpected(status.error());
        }
        result.framebuffer = std::move(stored_framebuffer.pixels);
    } else {
        if (auto cleared = drain_queue<SoftwareFramebuffer>(space, framebufferPath); !cleared) {
            return std::unexpected(cleared.error());
        }
        result.framebuffer = std::move(framebuffer);
    }
    return result;
}

} // namespace Window

namespace App {

auto Bootstrap(PathSpace& space,
               AppRootPathView appRoot,
               ScenePath const& scene,
               BootstrapParams const& params) -> SP::Expected<BootstrapResult> {
    if (auto status = ensure_identifier(params.view_name, "view name"); !status) {
        return std::unexpected(status.error());
    }

    auto renderer_params = params.renderer;
    if (renderer_params.description.empty()) {
        renderer_params.description = "bootstrap renderer";
    }
    auto renderer = Renderer::Create(space, appRoot, renderer_params);
    if (!renderer) {
        return std::unexpected(renderer.error());
    }

    auto surface_params = params.surface;
    auto resolve_dimension = [&](int value, int fallback_from_window, int fallback_settings, int fallback_default) {
        if (value > 0) {
            return value;
        }
        if (fallback_from_window > 0) {
            return fallback_from_window;
        }
        if (fallback_settings > 0) {
            return fallback_settings;
        }
        return fallback_default;
    };
    auto settings_width = params.renderer_settings_override
                              ? params.renderer_settings_override->surface.size_px.width
                              : 0;
    auto settings_height = params.renderer_settings_override
                               ? params.renderer_settings_override->surface.size_px.height
                               : 0;
    surface_params.desc.size_px.width = resolve_dimension(surface_params.desc.size_px.width,
                                                          params.window.width,
                                                          settings_width,
                                                          1280);
    surface_params.desc.size_px.height = resolve_dimension(surface_params.desc.size_px.height,
                                                           params.window.height,
                                                           settings_height,
                                                           720);
    surface_params.renderer = renderer->getPath();

    auto surface = Surface::Create(space, appRoot, surface_params);
    if (!surface) {
        return std::unexpected(surface.error());
    }

    if (auto status = Surface::SetScene(space, *surface, scene); !status) {
        return std::unexpected(status.error());
    }

    auto target_relative = read_value<std::string>(space,
                                                   std::string(surface->getPath()) + "/target");
    if (!target_relative) {
        return std::unexpected(target_relative.error());
    }

    auto target_absolute = SP::App::resolve_app_relative(appRoot, *target_relative);
    if (!target_absolute) {
        return std::unexpected(target_absolute.error());
    }

    auto window_params = params.window;
    window_params.width = resolve_dimension(window_params.width,
                                            surface_params.desc.size_px.width,
                                            settings_width,
                                            1280);
    window_params.height = resolve_dimension(window_params.height,
                                             surface_params.desc.size_px.height,
                                             settings_height,
                                             720);
    if (window_params.scale <= 0.0f) {
        window_params.scale = 1.0f;
    }

    auto window = Window::Create(space, appRoot, window_params);
    if (!window) {
        return std::unexpected(window.error());
    }

    if (auto status = Window::AttachSurface(space,
                                            *window,
                                            params.view_name,
                                            *surface); !status) {
        return std::unexpected(status.error());
    }

    auto surface_desc = read_value<SurfaceDesc>(space, std::string(surface->getPath()) + "/desc");
    if (!surface_desc) {
        return std::unexpected(surface_desc.error());
    }
    auto target_desc = read_value<SurfaceDesc>(space,
                                               std::string(target_absolute->getPath()) + "/desc");
    if (!target_desc) {
        return std::unexpected(target_desc.error());
    }

    auto present_policy = params.present_policy;
    auto view_base = std::string(window->getPath()) + "/views/" + params.view_name;
    if (params.configure_present_policy) {
        auto policy_string = present_mode_to_string(present_policy.mode);
        if (auto status = replace_single<std::string>(space,
                                                      view_base + "/present/policy",
                                                      policy_string); !status) {
            return std::unexpected(status.error());
        }

        auto to_ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        present_policy.staleness_budget_ms_value = to_ms(present_policy.staleness_budget);
        present_policy.frame_timeout_ms_value = to_ms(present_policy.frame_timeout);

        auto params_base = view_base + "/present/params";
        if (auto status = replace_single<double>(space,
                                                 params_base + "/staleness_budget_ms",
                                                 present_policy.staleness_budget_ms_value); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<double>(space,
                                                 params_base + "/frame_timeout_ms",
                                                 present_policy.frame_timeout_ms_value); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   params_base + "/max_age_frames",
                                                   static_cast<uint64_t>(present_policy.max_age_frames)); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/vsync_align",
                                               present_policy.vsync_align); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/auto_render_on_present",
                                               present_policy.auto_render_on_present); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/capture_framebuffer",
                                               present_policy.capture_framebuffer); !status) {
            return std::unexpected(status.error());
        }
    }

    RenderSettings applied_settings{};
    if (params.configure_renderer_settings) {
        if (params.renderer_settings_override) {
            applied_settings = *params.renderer_settings_override;
        } else {
            applied_settings.clear_color = {0.11f, 0.12f, 0.15f, 1.0f};
        }
        if (applied_settings.surface.size_px.width <= 0) {
            applied_settings.surface.size_px.width = target_desc->size_px.width;
        }
        if (applied_settings.surface.size_px.height <= 0) {
            applied_settings.surface.size_px.height = target_desc->size_px.height;
        }
        if (applied_settings.surface.dpi_scale <= 0.0f) {
            applied_settings.surface.dpi_scale = window_params.scale > 0.0f ? window_params.scale : 1.0f;
        }
        applied_settings.surface.visibility = true;
        applied_settings.renderer.backend_kind = params.renderer.kind;
        applied_settings.renderer.metal_uploads_enabled = (params.renderer.kind == RendererKind::Metal2D);

        auto update = Renderer::UpdateSettings(space,
                                               ConcretePathView{target_absolute->getPath()},
                                               applied_settings);
        if (!update) {
            return std::unexpected(update.error());
        }
    }

    if (params.submit_initial_dirty_rect) {
        DirtyRectHint hint{};
        if (params.initial_dirty_rect_override) {
            hint = ensure_valid_hint(*params.initial_dirty_rect_override);
        } else {
            hint = make_default_dirty_rect(static_cast<float>(target_desc->size_px.width),
                                           static_cast<float>(target_desc->size_px.height));
        }
        if (hint.max_x > hint.min_x && hint.max_y > hint.min_y) {
            std::array<DirtyRectHint, 1> hints{hint};
            if (auto status = Renderer::SubmitDirtyRects(space,
                                                         ConcretePathView{target_absolute->getPath()},
                                                         hints); !status) {
                return std::unexpected(status.error());
            }
        }
    }

    BootstrapResult result{};
    result.renderer = *renderer;
    result.surface = *surface;
    result.window = *window;
    result.view_name = params.view_name;
    result.target = ConcretePath{target_absolute->getPath()};
    result.surface_desc = *target_desc;
    result.present_policy = present_policy;
    result.applied_settings = applied_settings;
    return result;
}

} // namespace App

namespace Widgets {

auto ResolveHitTarget(Scene::HitTestResult const& hit) -> std::optional<HitTarget> {
    if (!hit.hit) {
        return std::nullopt;
    }

    std::string_view authoring = hit.target.authoring_node_id;
    auto marker = authoring.find(kWidgetAuthoringMarker);
    if (marker == std::string::npos || marker == 0) {
        return std::nullopt;
    }

    std::string widget_root = std::string(authoring.substr(0, marker));
    if (widget_root.empty() || widget_root.front() != '/') {
        return std::nullopt;
    }

    std::string component;
    auto component_pos = marker + kWidgetAuthoringMarker.size();
    if (component_pos < authoring.size()) {
        component.assign(authoring.substr(component_pos));
    }

    HitTarget target{};
    target.widget = WidgetPath{std::move(widget_root)};
    target.component = std::move(component);
    return target;
}

auto CreateButton(PathSpace& space,
                  AppRootPathView appRoot,
                  ButtonParams const& params) -> SP::Expected<ButtonPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::ButtonStyle style = params.style;
    style.width = std::max(style.width, 1.0f);
    style.height = std::max(style.height, 1.0f);
    float radius_limit = std::min(style.width, style.height) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.typography.font_size = std::max(style.typography.font_size, 1.0f);
    style.typography.line_height = std::max(style.typography.line_height, style.typography.font_size);
    style.typography.letter_spacing = std::max(style.typography.letter_spacing, 0.0f);

    Widgets::ButtonState defaultState{};
    if (auto status = write_button_metadata(space,
                                            widgetRoot->getPath(),
                                            params.label,
                                            defaultState,
                                            style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_widget_scene(space,
                                         appRoot,
                                         params.name,
                                         std::string("Widget button: ") + params.label);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_button_state_scenes(space, appRoot, params.name, style);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_button_bucket(style, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    ButtonPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .label = ConcretePath{widgetRoot->getPath() + "/meta/label"},
    };
    return paths;
}

auto write_toggle_metadata(PathSpace& space,
                           std::string const& rootPath,
                           Widgets::ToggleState const& state,
                           Widgets::ToggleStyle const& style) -> SP::Expected<void> {
    auto statePath = rootPath + "/state";
    if (auto status = replace_single<Widgets::ToggleState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }
    auto stylePath = rootPath + "/meta/style";
    if (auto status = replace_single<Widgets::ToggleStyle>(space, stylePath, style); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = write_widget_kind(space, rootPath, "toggle"); !status) {
        return status;
    }
    return {};
}

auto ensure_toggle_scene(PathSpace& space,
                         AppRootPathView appRoot,
                         std::string_view name) -> SP::Expected<ScenePath> {
    return ensure_widget_scene(space, appRoot, name, "Widget toggle");
}

auto CreateToggle(PathSpace& space,
                  AppRootPathView appRoot,
                  Widgets::ToggleParams const& params) -> SP::Expected<Widgets::TogglePaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::ToggleState defaultState{};
    if (auto status = write_toggle_metadata(space, widgetRoot->getPath(), defaultState, params.style); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_toggle_scene(space, appRoot, params.name);
   if (!scenePath) {
       return std::unexpected(scenePath.error());
   }

    auto stateScenes = publish_toggle_state_scenes(space, appRoot, params.name, params.style);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_toggle_bucket(params.style, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::TogglePaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
    };
    return paths;
}

auto CreateSlider(PathSpace& space,
                  AppRootPathView appRoot,
                  Widgets::SliderParams const& params) -> SP::Expected<Widgets::SliderPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    Widgets::SliderRange range{};
    range.minimum = std::min(params.minimum, params.maximum);
    range.maximum = std::max(params.minimum, params.maximum);
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }
    range.step = std::max(params.step, 0.0f);

    auto clamp_value = [&](float v) {
        float clamped = std::clamp(v, range.minimum, range.maximum);
        if (range.step > 0.0f) {
            float steps = std::round((clamped - range.minimum) / range.step);
            clamped = range.minimum + steps * range.step;
            clamped = std::clamp(clamped, range.minimum, range.maximum);
        }
        return clamped;
    };

    Widgets::SliderStyle style = params.style;
    style.width = std::max(style.width, 32.0f);
    style.height = std::max(style.height, 16.0f);
    style.track_height = std::clamp(style.track_height, 1.0f, style.height);
    style.thumb_radius = std::clamp(style.thumb_radius, style.track_height * 0.5f, style.height * 0.5f);
    style.label_typography.font_size = std::max(style.label_typography.font_size, 1.0f);
    style.label_typography.line_height = std::max(style.label_typography.line_height, style.label_typography.font_size);
    style.label_typography.letter_spacing = std::max(style.label_typography.letter_spacing, 0.0f);

    Widgets::SliderState defaultState{};
    defaultState.value = clamp_value(params.value);

    if (auto status = write_slider_metadata(space, widgetRoot->getPath(), defaultState, style, range); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_slider_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_slider_state_scenes(space,
                                                   appRoot,
                                                   params.name,
                                                   style,
                                                   range,
                                                   defaultState);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_slider_bucket(style, range, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::SliderPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .range = ConcretePath{widgetRoot->getPath() + "/meta/range"},
    };
    return paths;
}

auto CreateList(PathSpace& space,
                AppRootPathView appRoot,
                Widgets::ListParams const& params) -> SP::Expected<Widgets::ListPaths> {
    if (auto status = ensure_identifier(params.name, "widget name"); !status) {
        return std::unexpected(status.error());
    }

    auto widgetRoot = combine_relative(appRoot, std::string("widgets/") + params.name);
    if (!widgetRoot) {
        return std::unexpected(widgetRoot.error());
    }

    std::vector<Widgets::ListItem> items = params.items;
    if (items.empty()) {
        items.push_back(Widgets::ListItem{
            .id = "item-0",
            .label = "Item 1",
            .enabled = true,
        });
    }

    std::unordered_set<std::string> ids;
    ids.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto& item = items[index];
        if (item.id.empty()) {
            item.id = "item-" + std::to_string(index);
        }
        if (auto status = ensure_identifier(item.id, "list item id"); !status) {
            return std::unexpected(status.error());
        }
        if (!ids.insert(item.id).second) {
            return std::unexpected(make_error("list item ids must be unique",
                                              SP::Error::Code::MalformedInput));
        }
    }

    Widgets::ListStyle style = params.style;
    style.width = std::max(style.width, 96.0f);
    style.item_height = std::max(style.item_height, 24.0f);
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, std::min(style.width, style.item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u))) * 0.5f);
    style.border_thickness = std::clamp(style.border_thickness, 0.0f, style.item_height * 0.5f);
    style.item_typography.font_size = std::max(style.item_typography.font_size, 1.0f);
    style.item_typography.line_height = std::max(style.item_typography.line_height, style.item_typography.font_size);
    style.item_typography.letter_spacing = std::max(style.item_typography.letter_spacing, 0.0f);

    auto first_enabled = std::find_if(items.begin(), items.end(), [](auto const& entry) {
        return entry.enabled;
    });

    Widgets::ListState defaultState{};
    defaultState.selected_index = (first_enabled != items.end())
        ? static_cast<std::int32_t>(std::distance(items.begin(), first_enabled))
        : -1;
    defaultState.hovered_index = -1;
    defaultState.scroll_offset = 0.0f;

    if (auto status = write_list_metadata(space,
                                          widgetRoot->getPath(),
                                          defaultState,
                                          style,
                                          items); !status) {
        return std::unexpected(status.error());
    }

    auto scenePath = ensure_list_scene(space, appRoot, params.name);
    if (!scenePath) {
        return std::unexpected(scenePath.error());
    }

    auto stateScenes = publish_list_state_scenes(space,
                                                 appRoot,
                                                 params.name,
                                                 style,
                                                 items,
                                                 defaultState);
    if (!stateScenes) {
        return std::unexpected(stateScenes.error());
    }

    auto bucket = build_list_bucket(style, items, defaultState, widgetRoot->getPath());
    if (auto status = publish_scene_snapshot(space, appRoot, *scenePath, bucket); !status) {
        return std::unexpected(status.error());
    }

    Widgets::ListPaths paths{
        .scene = *scenePath,
        .states = *stateScenes,
        .root = WidgetPath{widgetRoot->getPath()},
        .state = ConcretePath{widgetRoot->getPath() + "/state"},
        .items = ConcretePath{widgetRoot->getPath() + "/meta/items"},
    };
    return paths;
}

auto UpdateButtonState(PathSpace& space,
                       Widgets::ButtonPaths const& paths,
                       Widgets::ButtonState const& new_state) -> SP::Expected<bool> {
    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ButtonState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !button_states_equal(**current, new_state);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ButtonState>(space, statePath, new_state); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ButtonStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto bucket = build_button_bucket(*styleValue, new_state, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateToggleState(PathSpace& space,
                       Widgets::TogglePaths const& paths,
                       Widgets::ToggleState const& new_state) -> SP::Expected<bool> {
    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ToggleState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !toggle_states_equal(**current, new_state);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ToggleState>(space, statePath, new_state); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ToggleStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto bucket = build_toggle_bucket(*styleValue, new_state, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateSliderState(PathSpace& space,
                       Widgets::SliderPaths const& paths,
                       Widgets::SliderState const& new_state) -> SP::Expected<bool> {
    auto rangePath = std::string(paths.range.getPath());
    auto rangeValue = read_optional<Widgets::SliderRange>(space, rangePath);
    if (!rangeValue) {
        return std::unexpected(rangeValue.error());
    }
    Widgets::SliderRange range = rangeValue->value_or(Widgets::SliderRange{});
    if (range.minimum > range.maximum) {
        std::swap(range.minimum, range.maximum);
    }
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }

    auto clamp_value = [&](float v) {
        float clamped = std::clamp(v, range.minimum, range.maximum);
        if (range.step > 0.0f) {
            float steps = std::round((clamped - range.minimum) / range.step);
            clamped = range.minimum + steps * range.step;
            clamped = std::clamp(clamped, range.minimum, range.maximum);
        }
        return clamped;
    };

    Widgets::SliderState sanitized = new_state;
    sanitized.value = clamp_value(new_state.value);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::SliderState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !slider_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::SliderState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::SliderStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }
    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto bucket = build_slider_bucket(*styleValue, range, sanitized, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto UpdateListState(PathSpace& space,
                     Widgets::ListPaths const& paths,
                     Widgets::ListState const& new_state) -> SP::Expected<bool> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    auto itemsValue = read_optional<std::vector<Widgets::ListItem>>(space, itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    std::vector<Widgets::ListItem> items;
    if (itemsValue->has_value()) {
        items = **itemsValue;
    }

    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    auto styleValue = space.read<Widgets::ListStyle, std::string>(stylePath);
    if (!styleValue) {
        return std::unexpected(styleValue.error());
    }

    auto sanitize_index = [&](std::int32_t index) -> std::int32_t {
        if (items.empty()) {
            return -1;
        }
        if (index < 0) {
            return -1;
        }
        if (index >= static_cast<std::int32_t>(items.size())) {
            index = static_cast<std::int32_t>(items.size()) - 1;
        }
        auto is_enabled = [&](std::int32_t candidate) -> bool {
            auto const idx = static_cast<std::size_t>(candidate);
            return idx < items.size() && items[idx].enabled;
        };
        if (is_enabled(index)) {
            return index;
        }
        for (std::int32_t forward = index + 1; forward < static_cast<std::int32_t>(items.size()); ++forward) {
            if (is_enabled(forward)) {
                return forward;
            }
        }
        for (std::int32_t backward = index - 1; backward >= 0; --backward) {
            if (is_enabled(backward)) {
                return backward;
            }
        }
        return -1;
    };

    Widgets::ListState sanitized = new_state;
    sanitized.enabled = new_state.enabled;
    sanitized.hovered_index = sanitize_index(new_state.hovered_index);
    sanitized.selected_index = sanitize_index(new_state.selected_index);
    sanitized.scroll_offset = std::max(new_state.scroll_offset, 0.0f);

    float const content_span = styleValue->item_height * static_cast<float>(std::max<std::size_t>(items.size(), 1u));
    float const max_scroll = std::max(0.0f, content_span - styleValue->item_height);
    sanitized.scroll_offset = std::clamp(sanitized.scroll_offset, 0.0f, max_scroll);

    auto statePath = std::string(paths.state.getPath());
    auto current = read_optional<Widgets::ListState>(space, statePath);
    if (!current) {
        return std::unexpected(current.error());
    }
    bool changed = !current->has_value() || !list_states_equal(**current, sanitized);
    if (!changed) {
        return false;
    }
    if (auto status = replace_single<Widgets::ListState>(space, statePath, sanitized); !status) {
        return std::unexpected(status.error());
    }

    auto appRootPath = derive_app_root_for(ConcretePathView{paths.root.getPath()});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto appRootView = SP::App::AppRootPathView{appRootPath->getPath()};
    auto bucket = build_list_bucket(*styleValue, items, sanitized, paths.root.getPath());
    if (auto status = publish_scene_snapshot(space, appRootView, paths.scene, bucket); !status) {
        return std::unexpected(status.error());
    }

    if (auto mark = Scene::MarkDirty(space, paths.scene, Scene::DirtyKind::Visual); !mark) {
        return std::unexpected(mark.error());
    }
    return true;
}

auto MakeDefaultWidgetTheme() -> WidgetTheme {
    WidgetTheme theme{};
    theme.button.width = 200.0f;
    theme.button.height = 48.0f;
    theme.button.corner_radius = 8.0f;
    theme.button.background_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.button.text_color = {1.0f, 1.0f, 1.0f, 1.0f};
    theme.button.typography.font_size = 28.0f;
    theme.button.typography.line_height = 28.0f;
    theme.button.typography.letter_spacing = 1.0f;
    theme.button.typography.baseline_shift = 0.0f;

    theme.toggle.width = 56.0f;
    theme.toggle.height = 32.0f;
    theme.toggle.track_off_color = {0.75f, 0.75f, 0.78f, 1.0f};
    theme.toggle.track_on_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.toggle.thumb_color = {1.0f, 1.0f, 1.0f, 1.0f};

    theme.slider.width = 240.0f;
    theme.slider.height = 32.0f;
    theme.slider.track_height = 6.0f;
    theme.slider.thumb_radius = 10.0f;
    theme.slider.track_color = {0.75f, 0.75f, 0.78f, 1.0f};
    theme.slider.fill_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.slider.thumb_color = {1.0f, 1.0f, 1.0f, 1.0f};
    theme.slider.label_color = {0.90f, 0.92f, 0.96f, 1.0f};
    theme.slider.label_typography.font_size = 24.0f;
    theme.slider.label_typography.line_height = 28.0f;
    theme.slider.label_typography.letter_spacing = 1.0f;
    theme.slider.label_typography.baseline_shift = 0.0f;

    theme.list.width = 240.0f;
    theme.list.item_height = 36.0f;
    theme.list.corner_radius = 8.0f;
    theme.list.border_thickness = 1.0f;
    theme.list.background_color = {0.121f, 0.129f, 0.145f, 1.0f};
    theme.list.border_color = {0.239f, 0.247f, 0.266f, 1.0f};
    theme.list.item_color = {0.176f, 0.184f, 0.204f, 1.0f};
    theme.list.item_hover_color = {0.247f, 0.278f, 0.349f, 1.0f};
    theme.list.item_selected_color = {0.176f, 0.353f, 0.914f, 1.0f};
    theme.list.separator_color = {0.224f, 0.231f, 0.247f, 1.0f};
    theme.list.item_text_color = {0.94f, 0.96f, 0.99f, 1.0f};
    theme.list.item_typography.font_size = 21.0f;
    theme.list.item_typography.line_height = 24.0f;
    theme.list.item_typography.letter_spacing = 1.0f;
    theme.list.item_typography.baseline_shift = 0.0f;

    theme.heading.font_size = 32.0f;
    theme.heading.line_height = 36.0f;
    theme.heading.letter_spacing = 1.0f;
    theme.heading.baseline_shift = 0.0f;
    theme.caption.font_size = 24.0f;
    theme.caption.line_height = 28.0f;
    theme.caption.letter_spacing = 1.0f;
    theme.caption.baseline_shift = 0.0f;
    theme.heading_color = {0.93f, 0.95f, 0.98f, 1.0f};
    theme.caption_color = {0.90f, 0.92f, 0.96f, 1.0f};
    theme.accent_text_color = {0.85f, 0.88f, 0.95f, 1.0f};
    theme.muted_text_color = {0.70f, 0.72f, 0.78f, 1.0f};

    return theme;
}

auto MakeSunsetWidgetTheme() -> WidgetTheme {
    WidgetTheme theme = MakeDefaultWidgetTheme();
    theme.button.background_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.button.text_color = {1.0f, 0.984f, 0.945f, 1.0f};
    theme.toggle.track_on_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.toggle.track_off_color = {0.60f, 0.44f, 0.38f, 1.0f};
    theme.toggle.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.fill_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.slider.thumb_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.slider.label_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.list.background_color = {0.215f, 0.128f, 0.102f, 1.0f};
    theme.list.border_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_color = {0.266f, 0.166f, 0.138f, 1.0f};
    theme.list.item_hover_color = {0.422f, 0.248f, 0.198f, 1.0f};
    theme.list.item_selected_color = {0.882f, 0.424f, 0.310f, 1.0f};
    theme.list.separator_color = {0.365f, 0.231f, 0.201f, 1.0f};
    theme.list.item_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.heading_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.caption_color = {0.965f, 0.886f, 0.812f, 1.0f};
    theme.accent_text_color = {0.996f, 0.949f, 0.902f, 1.0f};
    theme.muted_text_color = {0.855f, 0.698f, 0.612f, 1.0f};
    return theme;
}

auto ApplyTheme(WidgetTheme const& theme, ButtonParams& params) -> void {
    params.style = theme.button;
}

auto ApplyTheme(WidgetTheme const& theme, ToggleParams& params) -> void {
    params.style = theme.toggle;
}

auto ApplyTheme(WidgetTheme const& theme, SliderParams& params) -> void {
    params.style = theme.slider;
}

auto ApplyTheme(WidgetTheme const& theme, ListParams& params) -> void {
    params.style = theme.list;
}

namespace {

auto widget_name_from_root(std::string const& app_root,
                           std::string const& widget_root) -> SP::Expected<std::string> {
    auto prefix = app_root + "/widgets/";
    if (widget_root.rfind(prefix, 0) != 0) {
        return std::unexpected(make_error("widget path must belong to app widgets subtree",
                                          SP::Error::Code::InvalidPath));
    }
    auto name = widget_root.substr(prefix.size());
    if (name.empty()) {
        return std::unexpected(make_error("widget path missing identifier", SP::Error::Code::InvalidPath));
    }
    return name;
}

auto widget_scene_path(std::string const& app_root,
                       std::string const& widget_name) -> std::string {
    return app_root + "/scenes/widgets/" + widget_name;
}

auto determine_widget_kind(PathSpace& space,
                           std::string const& rootPath) -> SP::Expected<WidgetKind> {
    auto kindPath = rootPath + "/meta/kind";
    auto kindValue = read_optional<std::string>(space, kindPath);
    if (!kindValue) {
        return std::unexpected(kindValue.error());
    }
    if (kindValue->has_value()) {
        std::string_view kind = **kindValue;
        if (kind == "button") {
            return WidgetKind::Button;
        }
        if (kind == "toggle") {
            return WidgetKind::Toggle;
        }
        if (kind == "slider") {
            return WidgetKind::Slider;
        }
        if (kind == "list") {
            return WidgetKind::List;
        }
    }

    auto itemsPath = rootPath + "/meta/items";
    auto itemsValue = read_optional<std::vector<Widgets::ListItem>>(space, itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    if (itemsValue->has_value()) {
        return WidgetKind::List;
    }

    auto rangePath = rootPath + "/meta/range";
    auto rangeValue = read_optional<Widgets::SliderRange>(space, rangePath);
    if (!rangeValue) {
        return std::unexpected(rangeValue.error());
    }
    if (rangeValue->has_value()) {
        return WidgetKind::Slider;
    }

    auto labelPath = rootPath + "/meta/label";
    auto labelValue = read_optional<std::string>(space, labelPath);
    if (!labelValue) {
        return std::unexpected(labelValue.error());
    }
    if (labelValue->has_value()) {
        return WidgetKind::Button;
    }

    return WidgetKind::Toggle;
}

auto update_button_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ButtonState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ButtonState desired = *stateValue;
    desired.hovered = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::ButtonPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .label = ConcretePath{widget_root + "/meta/label"},
    };
    auto updated = Widgets::UpdateButtonState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_toggle_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ToggleState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ToggleState desired = *stateValue;
    desired.hovered = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::TogglePaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
    };
    auto updated = Widgets::UpdateToggleState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_slider_focus(PathSpace& space,
                         std::string const& widget_root,
                         std::string const& app_root,
                         bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::SliderState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::SliderState desired = *stateValue;
    desired.hovered = focused;

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::SliderPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .range = ConcretePath{widget_root + "/meta/range"},
    };
    auto updated = Widgets::UpdateSliderState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_list_focus(PathSpace& space,
                       std::string const& widget_root,
                       std::string const& app_root,
                       bool focused) -> SP::Expected<bool> {
    auto statePath = widget_root + "/state";
    auto stateValue = space.read<Widgets::ListState, std::string>(statePath);
    if (!stateValue) {
        return std::unexpected(stateValue.error());
    }
    Widgets::ListState desired = *stateValue;

    auto itemsPath = widget_root + "/meta/items";
    auto itemsValue = space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
    if (!itemsValue) {
        return std::unexpected(itemsValue.error());
    }
    auto const& items = *itemsValue;
    if (focused) {
        if (!items.empty()) {
            auto max_index = static_cast<std::int32_t>(items.size()) - 1;
            auto hovered = desired.hovered_index;
            if (hovered < 0 || hovered > max_index) {
                if (desired.selected_index >= 0 && desired.selected_index <= max_index) {
                    hovered = desired.selected_index;
                } else {
                    hovered = 0;
                }
            }
            desired.hovered_index = hovered;
        } else {
            desired.hovered_index = -1;
        }
    } else {
        desired.hovered_index = -1;
    }

    auto widget_name = widget_name_from_root(app_root, widget_root);
    if (!widget_name) {
        return std::unexpected(widget_name.error());
    }
    auto scenePath = widget_scene_path(app_root, *widget_name);
    Widgets::ListPaths paths{
        .scene = ScenePath{scenePath},
        .states = {},
        .root = WidgetPath{widget_root},
        .state = ConcretePath{statePath},
        .items = ConcretePath{itemsPath},
    };
    auto updated = Widgets::UpdateListState(space, paths, desired);
    if (!updated) {
        return std::unexpected(updated.error());
    }
    return *updated;
}

auto update_widget_focus(PathSpace& space,
                         std::string const& widget_root,
                         bool focused) -> SP::Expected<bool> {
    auto appRootPath = derive_app_root_for(ConcretePathView{widget_root});
    if (!appRootPath) {
        return std::unexpected(appRootPath.error());
    }
    auto kind = determine_widget_kind(space, widget_root);
    if (!kind) {
        return std::unexpected(kind.error());
    }

    auto const& app_root = appRootPath->getPath();
    switch (*kind) {
        case WidgetKind::Button:
            return update_button_focus(space, widget_root, app_root, focused);
        case WidgetKind::Toggle:
            return update_toggle_focus(space, widget_root, app_root, focused);
        case WidgetKind::Slider:
            return update_slider_focus(space, widget_root, app_root, focused);
        case WidgetKind::List:
            return update_list_focus(space, widget_root, app_root, focused);
    }
    return std::unexpected(make_error("unknown widget kind", SP::Error::Code::InvalidType));
}

} // namespace

namespace Focus {

auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath {
    return ConcretePath{std::string(appRoot.getPath()) + "/widgets/focus/current"};
}

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target) -> Config {
    Config config{
        .focus_state = FocusStatePath(appRoot),
        .auto_render_target = std::move(auto_render_target),
    };
    return config;
}

auto Current(PathSpace const& space,
             ConcretePathView focus_state) -> SP::Expected<std::optional<std::string>> {
    std::string path{focus_state.getPath()};
    auto existing = read_optional<std::string>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return std::optional<std::string>{};
    }
    auto const& value = **existing;
    if (value.empty()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{value};
}

auto set_focus_string(PathSpace& space,
                      ConcretePathView focus_state,
                      std::string const& value) -> SP::Expected<void> {
    std::string path{focus_state.getPath()};
    if (auto status = replace_single<std::string>(space, path, value); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto maybe_schedule_focus_render(PathSpace& space,
                                 Config const& config,
                                 bool changed) -> SP::Expected<void> {
    if (!changed) {
        return {};
    }
    if (!config.auto_render_target.has_value()) {
        return {};
    }
    return enqueue_auto_render_event(space,
                                     config.auto_render_target->getPath(),
                                     "focus-navigation",
                                     0);
}

auto Set(PathSpace& space,
         Config const& config,
         WidgetPath const& widget) -> SP::Expected<UpdateResult> {
    std::string target_path{widget.getPath()};
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    auto previous = *current;

    auto apply_focus = update_widget_focus(space, target_path, true);
    if (!apply_focus) {
        return std::unexpected(apply_focus.error());
    }
    bool changed = *apply_focus;

    if (!previous.has_value() || *previous != target_path) {
        if (previous.has_value()) {
            auto clear_prev = update_widget_focus(space, *previous, false);
            if (!clear_prev) {
                return std::unexpected(clear_prev.error());
            }
        }
        if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, target_path); !status) {
            return std::unexpected(status.error());
        }
        changed = true;
    }

    if (auto status = maybe_schedule_focus_render(space, config, changed); !status) {
        return std::unexpected(status.error());
    }

    return UpdateResult{
        .widget = widget,
        .changed = changed,
    };
}

auto Clear(PathSpace& space,
           Config const& config) -> SP::Expected<bool> {
    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    if (!current->has_value()) {
        return false;
    }

    bool changed = false;
    auto clear_prev = update_widget_focus(space, **current, false);
    if (!clear_prev) {
        return std::unexpected(clear_prev.error());
    }
    changed = *clear_prev;

    if (auto status = set_focus_string(space, ConcretePathView{config.focus_state.getPath()}, std::string{}); !status) {
        return std::unexpected(status.error());
    }
    changed = true;

    if (auto status = maybe_schedule_focus_render(space, config, true); !status) {
        return std::unexpected(status.error());
    }

    return changed;
}

auto Move(PathSpace& space,
          Config const& config,
          std::span<WidgetPath const> order,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>> {
    if (order.empty()) {
        return std::optional<UpdateResult>{};
    }

    auto current = Current(space, ConcretePathView{config.focus_state.getPath()});
    if (!current) {
        return std::unexpected(current.error());
    }
    auto current_value = current->value_or(std::string{});

    std::size_t next_index = 0;
    if (!current_value.empty()) {
        auto it = std::find_if(order.begin(), order.end(), [&](WidgetPath const& path) {
            return path.getPath() == current_value;
        });
        if (it != order.end()) {
            auto index = static_cast<std::size_t>(std::distance(order.begin(), it));
            if (direction == Direction::Forward) {
                next_index = (index + 1) % order.size();
            } else {
                next_index = (index + order.size() - 1) % order.size();
            }
        } else {
            next_index = (direction == Direction::Forward) ? 0 : order.size() - 1;
        }
    } else {
        next_index = (direction == Direction::Forward) ? 0 : order.size() - 1;
    }

    auto result = Set(space, config, order[next_index]);
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::optional<UpdateResult>{*result};
}

auto ApplyHit(PathSpace& space,
              Config const& config,
              Scene::HitTestResult const& hit) -> SP::Expected<std::optional<UpdateResult>> {
    auto target = ResolveHitTarget(hit);
    if (!target) {
        return std::optional<UpdateResult>{};
    }
    auto result = Set(space, config, target->widget);
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::optional<UpdateResult>{*result};
}

} // namespace Focus

} // namespace Widgets

namespace Widgets::Bindings {

namespace {

auto compute_ops_queue(WidgetPath const& root) -> ConcretePath {
    return ConcretePath{std::string(root.getPath()) + "/ops/inbox/queue"};
}

auto build_options(WidgetPath const& root,
                   ConcretePathView targetPath,
                   DirtyRectHint hint,
                   bool auto_render) -> BindingOptions {
    BindingOptions options{
        .target = ConcretePath{std::string(targetPath.getPath())},
        .ops_queue = compute_ops_queue(root),
        .dirty_rect = ensure_valid_hint(hint),
        .auto_render = auto_render,
    };
    return options;
}

auto read_frame_index(PathSpace& space, std::string const& target) -> SP::Expected<std::uint64_t> {
    auto frame = read_optional<std::uint64_t>(space, target + "/output/v1/common/frameIndex");
    if (!frame) {
        return std::unexpected(frame.error());
    }
    if (frame->has_value()) {
        return **frame;
    }
    return std::uint64_t{0};
}

auto submit_dirty_hint(PathSpace& space,
                       BindingOptions const& options) -> SP::Expected<void> {
    auto const& rect = options.dirty_rect;
    if (rect.max_x <= rect.min_x || rect.max_y <= rect.min_y) {
        return {};
    }
    std::array<DirtyRectHint, 1> hints{rect};
    return Renderer::SubmitDirtyRects(space,
                                      SP::ConcretePathStringView{options.target.getPath()},
                                      std::span<const DirtyRectHint>(hints.data(), hints.size()));
}

auto schedule_auto_render(PathSpace& space,
                          BindingOptions const& options,
                          std::string_view reason) -> SP::Expected<void> {
    if (!options.auto_render) {
        return {};
    }
    auto frame_index = read_frame_index(space, options.target.getPath());
    if (!frame_index) {
        return std::unexpected(frame_index.error());
    }
    return enqueue_auto_render_event(space,
                                     options.target.getPath(),
                                     reason,
                                     *frame_index);
}

auto enqueue_widget_op(PathSpace& space,
                       BindingOptions const& options,
                       std::string const& widget_path,
                       WidgetOpKind kind,
                       PointerInfo const& pointer,
                       float value) -> SP::Expected<void> {
    WidgetOp op{};
    op.kind = kind;
    op.widget_path = widget_path;
    op.pointer = pointer;
    op.value = value;
    op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

    auto inserted = space.insert(options.ops_queue.getPath(), op);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto read_button_style(PathSpace& space,
                       ButtonPaths const& paths) -> SP::Expected<Widgets::ButtonStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ButtonStyle, std::string>(stylePath);
}

auto read_toggle_style(PathSpace& space,
                       TogglePaths const& paths) -> SP::Expected<Widgets::ToggleStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ToggleStyle, std::string>(stylePath);
}

auto read_slider_style(PathSpace& space,
                       SliderPaths const& paths) -> SP::Expected<Widgets::SliderStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::SliderStyle, std::string>(stylePath);
}

auto read_list_style(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<Widgets::ListStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ListStyle, std::string>(stylePath);
}

auto read_list_items(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<std::vector<Widgets::ListItem>> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    return space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
}

} // namespace

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo {
    PointerInfo info{};
    info.scene_x = hit.position.scene_x;
    info.scene_y = hit.position.scene_y;
    info.inside = hit.hit;
    info.primary = true;
    return info;
}

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ButtonBinding> {
    auto style = read_button_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ButtonBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ToggleBinding> {
    auto style = read_toggle_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ToggleBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<SliderBinding> {
    auto style = read_slider_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    SliderBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateListBinding(PathSpace& space,
                       AppRootPathView,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<ListBinding> {
    auto style = read_list_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    auto items = read_list_items(space, paths);
    if (!items) {
        return std::unexpected(items.error());
    }

    auto item_count = std::max<std::size_t>(items->size(), 1u);
    float height = style->item_height * static_cast<float>(item_count) + style->border_thickness * 2.0f;
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, height));
    ListBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Activate:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for button binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateButtonState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/button"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.pressed ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Toggle:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for toggle binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateToggleState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/toggle"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.checked ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::SliderBegin:
    case WidgetOpKind::SliderUpdate:
    case WidgetOpKind::SliderCommit:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for slider binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateSliderState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto current_state = space.read<SliderState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/slider"); !status) {
            return std::unexpected(status.error());
        }
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        current_state->value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer,
                  std::int32_t item_index,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::ListHover:
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
    case WidgetOpKind::ListScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for list binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    Widgets::ListState desired = new_state;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        desired.hovered_index = item_index;
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        if (item_index >= 0) {
            desired.selected_index = item_index;
        }
        break;
    case WidgetOpKind::ListScroll:
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateListState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space,
                                               binding.options,
                                               "widget/list"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        op_value = static_cast<float>(updated_state->hovered_index);
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        op_value = static_cast<float>(updated_state->selected_index);
        break;
    case WidgetOpKind::ListScroll:
        op_value = updated_state->scroll_offset;
        break;
    default:
        break;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

} // namespace Widgets::Bindings

namespace Widgets::Reducers {

namespace {

auto to_widget_action(Bindings::WidgetOp const& op) -> WidgetAction {
    WidgetAction action{};
    action.kind = op.kind;
    action.widget_path = op.widget_path;
    action.pointer = op.pointer;
    action.analog_value = op.value;
    action.sequence = op.sequence;
    action.timestamp_ns = op.timestamp_ns;

    switch (op.kind) {
    case Bindings::WidgetOpKind::ListHover:
    case Bindings::WidgetOpKind::ListSelect:
    case Bindings::WidgetOpKind::ListActivate:
        action.discrete_index = static_cast<std::int32_t>(std::llround(op.value));
        break;
    default:
        action.discrete_index = -1;
        break;
    }

    return action;
}

} // namespace

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath {
    std::string queue_path = widget_root.getPath();
    queue_path.append("/ops/actions/inbox/queue");
    return ConcretePath{std::move(queue_path)};
}

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions) -> SP::Expected<std::vector<WidgetAction>> {
    std::vector<WidgetAction> actions;
    if (max_actions == 0) {
        return actions;
    }

    auto queue_path = std::string(ops_queue.getPath());
    if (queue_path.empty()) {
        return actions;
    }

    for (std::size_t processed = 0; processed < max_actions; ++processed) {
        auto taken = space.take<Bindings::WidgetOp, std::string>(queue_path);
        if (taken) {
            actions.push_back(to_widget_action(*taken));
            continue;
        }

        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }

    return actions;
}

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void> {
    if (actions.empty()) {
        return {};
    }

    auto queue_path = std::string(actions_queue.getPath());
    if (queue_path.empty()) {
        return {};
    }

    for (auto const& action : actions) {
        auto inserted = space.insert(queue_path, action);
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
    }

    return {};
}

} // namespace Widgets::Reducers

namespace Diagnostics {

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics> {
    TargetMetrics metrics{};

    auto base = std::string(targetPath.getPath()) + "/output/v1/common";

    if (auto value = read_value<uint64_t>(space, base + "/frameIndex"); value) {
        metrics.frame_index = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/revision"); value) {
        metrics.revision = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/renderMs"); value) {
        metrics.render_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/presentMs"); value) {
        metrics.present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/gpuEncodeMs"); value) {
        metrics.gpu_encode_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/gpuPresentMs"); value) {
        metrics.gpu_present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/usedMetalTexture"); value) {
        metrics.used_metal_texture = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, base + "/backendKind"); value) {
        metrics.backend_kind = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/lastPresentSkipped"); value) {
       metrics.last_present_skipped = *value;
   } else if (value.error().code != SP::Error::Code::NoObjectFound
              && value.error().code != SP::Error::Code::NoSuchPath) {
       return std::unexpected(value.error());
   }

    if (auto value = read_value<uint64_t>(space, base + "/materialCount"); value) {
        metrics.material_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto descriptors = read_optional<std::vector<MaterialDescriptor>>(space, base + "/materialDescriptors"); !descriptors) {
        return std::unexpected(descriptors.error());
    } else if (descriptors->has_value()) {
        metrics.materials = std::move(**descriptors);
        if (metrics.material_count == 0) {
            metrics.material_count = static_cast<uint64_t>(metrics.materials.size());
        }
    }

    if (auto value = read_value<uint64_t>(space, base + "/materialResourceCount"); value) {
        metrics.material_resource_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto resources = read_optional<std::vector<MaterialResourceResidency>>(space, base + "/materialResources"); !resources) {
        return std::unexpected(resources.error());
    } else if (resources->has_value()) {
        metrics.material_resources = std::move(**resources);
        if (metrics.material_resource_count == 0) {
            metrics.material_resource_count = static_cast<uint64_t>(metrics.material_resources.size());
        }
    }

    auto residencyBase = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuBytes"); value) {
        metrics.cpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuSoftBytes"); value) {
        metrics.cpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuHardBytes"); value) {
        metrics.cpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuBytes"); value) {
        metrics.gpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuSoftBytes"); value) {
        metrics.gpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuHardBytes"); value) {
        metrics.gpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/cpuSoftBudgetRatio"); value) {
        metrics.cpu_soft_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/cpuHardBudgetRatio"); value) {
        metrics.cpu_hard_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/gpuSoftBudgetRatio"); value) {
        metrics.gpu_soft_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, residencyBase + "/gpuHardBudgetRatio"); value) {
        metrics.gpu_hard_budget_ratio = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/cpuSoftExceeded"); value) {
        metrics.cpu_soft_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/cpuHardExceeded"); value) {
        metrics.cpu_hard_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/gpuSoftExceeded"); value) {
        metrics.gpu_soft_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, residencyBase + "/gpuHardExceeded"); value) {
        metrics.gpu_hard_exceeded = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/cpuStatus"); value) {
        metrics.cpu_residency_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/gpuStatus"); value) {
        metrics.gpu_residency_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, residencyBase + "/overallStatus"); value) {
        metrics.residency_overall_status = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    metrics.last_error.clear();
    metrics.last_error_code = 0;
    metrics.last_error_revision = 0;
    metrics.last_error_severity = PathSpaceError::Severity::Info;
    metrics.last_error_timestamp_ns = 0;
    metrics.last_error_detail.clear();

    auto diagPath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto errorValue = read_optional<PathSpaceError>(space, diagPath); !errorValue) {
        return std::unexpected(errorValue.error());
    } else if (errorValue->has_value() && !(*errorValue)->message.empty()) {
        metrics.last_error = (*errorValue)->message;
        metrics.last_error_code = (*errorValue)->code;
        metrics.last_error_revision = (*errorValue)->revision;
        metrics.last_error_severity = (*errorValue)->severity;
        metrics.last_error_timestamp_ns = (*errorValue)->timestamp_ns;
        metrics.last_error_detail = (*errorValue)->detail;
    } else {
        if (auto value = read_value<std::string>(space, base + "/lastError"); value) {
            metrics.last_error = *value;
        } else if (value.error().code != SP::Error::Code::NoObjectFound
                   && value.error().code != SP::Error::Code::NoSuchPath) {
            return std::unexpected(value.error());
        }
    }

    return metrics;
}

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    PathSpaceError cleared{};
    if (auto status = replace_single<PathSpaceError>(space, livePath, cleared); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, std::string{});
}

auto WriteTargetError(PathSpace& space,
                      ConcretePathView targetPath,
                      PathSpaceError const& error) -> SP::Expected<void> {
    if (error.message.empty()) {
        return ClearTargetError(space, targetPath);
    }

    PathSpaceError stored = error;
    if (stored.path.empty()) {
        stored.path = std::string(targetPath.getPath());
    }
    if (stored.timestamp_ns == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        stored.timestamp_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto status = replace_single<PathSpaceError>(space, livePath, stored); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, stored.message);
}

auto ReadTargetError(PathSpace const& space,
                     ConcretePathView targetPath) -> SP::Expected<std::optional<PathSpaceError>> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    return read_optional<PathSpaceError>(space, livePath);
}

auto ReadSoftwareFramebuffer(PathSpace const& space,
                              ConcretePathView targetPath) -> SP::Expected<SoftwareFramebuffer> {
    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    return read_value<SoftwareFramebuffer>(space, framebufferPath);
}

auto write_present_metrics_to_base(PathSpace& space,
                                   std::string const& base,
                                   PathWindowPresentStats const& stats,
                                   PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    if (auto status = replace_single<uint64_t>(space, base + "/frameIndex", stats.frame.frame_index); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/revision", stats.frame.revision); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/renderMs", stats.frame.render_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/damageMs", stats.damage_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/encodeMs", stats.encode_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/progressiveCopyMs", stats.progressive_copy_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/publishMs", stats.publish_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentMs", stats.present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuEncodeMs", stats.gpu_encode_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuPresentMs", stats.gpu_present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/lastPresentSkipped", stats.skipped); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedMetalTexture", stats.used_metal_texture); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/backendKind", stats.backend_kind); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/presented", stats.presented); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/bufferedFrameConsumed", stats.buffered_frame_consumed); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedProgressive", stats.used_progressive); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentedAgeMs", stats.frame_age_ms); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/presentedAgeFrames", stats.frame_age_frames); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/stale", stats.stale); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space,
                                                  base + "/presentMode",
                                                  present_mode_to_string(stats.mode)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/drawableCount", stats.drawable_count); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTilesUpdated",
                                               stats.progressive_tiles_updated); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveBytesCopied",
                                               stats.progressive_bytes_copied); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTileSize",
                                               stats.progressive_tile_size); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveWorkersUsed",
                                               stats.progressive_workers_used); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveJobs",
                                               stats.progressive_jobs); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/encodeWorkersUsed",
                                               stats.encode_workers_used); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/encodeJobs",
                                               stats.encode_jobs); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/progressiveTileDiagnosticsEnabled",
                                           stats.progressive_tile_diagnostics_enabled); !status) {
        return status;
    }
    auto progressive_tiles_copied = static_cast<uint64_t>(stats.progressive_tiles_copied);
    if (progressive_tiles_copied == 0) {
        auto existing_tiles = space.read<uint64_t>(base + "/progressiveTilesCopied");
        if (existing_tiles) {
            progressive_tiles_copied = *existing_tiles;
        }
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveTilesCopied",
                                               progressive_tiles_copied); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveRectsCoalesced",
                                               static_cast<uint64_t>(stats.progressive_rects_coalesced)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveSkipOddSeq",
                                               static_cast<uint64_t>(stats.progressive_skip_seq_odd)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/progressiveRecopyAfterSeqChange",
                                               static_cast<uint64_t>(stats.progressive_recopy_after_seq_change)); !status) {
        return status;
    }
    if (stats.progressive_tile_diagnostics_enabled) {
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesDirty",
                                                   stats.progressive_tiles_dirty); !status) {
            return status;
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesTotal",
                                                   stats.progressive_tiles_total); !status) {
            return status;
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   base + "/progressiveTilesSkipped",
                                                   stats.progressive_tiles_skipped); !status) {
            return status;
        }
    }
    if (auto status = replace_single<double>(space, base + "/waitBudgetMs", stats.wait_budget_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/stalenessBudgetMs",
                                             policy.staleness_budget_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/frameTimeoutMs",
                                             policy.frame_timeout_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/maxAgeFrames",
                                               static_cast<uint64_t>(policy.max_age_frames)); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/autoRenderOnPresent",
                                           policy.auto_render_on_present); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/vsyncAlign",
                                           policy.vsync_align); !status) {
        return status;
    }
    return {};
}

auto WritePresentMetrics(PathSpace& space,
                          ConcretePathView targetPath,
                          PathWindowPresentStats const& stats,
                          PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/output/v1/common";
    if (auto status = write_present_metrics_to_base(space, base, stats, policy); !status) {
        return status;
    }

    if (!stats.error.empty()) {
        PathSpaceError error{};
        error.code = 3000;
        error.severity = PathSpaceError::Severity::Recoverable;
        error.message = stats.error;
        error.path = std::string(targetPath.getPath());
        error.revision = stats.frame.revision;
        if (auto status = WriteTargetError(space, targetPath, error); !status) {
            return status;
        }
    } else {
        if (auto status = ClearTargetError(space, targetPath); !status) {
            return status;
        }
    }
    return {};
}

auto WriteWindowPresentMetrics(PathSpace& space,
                               ConcretePathView windowPath,
                               std::string_view viewName,
                               PathWindowPresentStats const& stats,
                               PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    std::string base = std::string(windowPath.getPath()) + "/diagnostics/metrics/live/views/";
    base.append(viewName);
    base.append("/present");

    if (auto status = write_present_metrics_to_base(space, base, stats, policy); !status) {
        return status;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    if (auto status = replace_single<std::string>(space, base + "/lastError", stats.error); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/viewName", std::string(viewName)); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/timestampNs", timestamp_ns); !status) {
        return status;
    }
#if defined(__APPLE__)
    if (auto status = replace_single<bool>(space, base + "/usedIOSurface", stats.used_iosurface); !status) {
        return status;
    }
#endif
    return {};
}

auto WriteResidencyMetrics(PathSpace& space,
                           ConcretePathView targetPath,
                           std::uint64_t cpu_bytes,
                           std::uint64_t gpu_bytes,
                           std::uint64_t cpu_soft_bytes,
                           std::uint64_t cpu_hard_bytes,
                           std::uint64_t gpu_soft_bytes,
                           std::uint64_t gpu_hard_bytes) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuBytes", cpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuSoftBytes", cpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuHardBytes", cpu_hard_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuBytes", gpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuSoftBytes", gpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuHardBytes", gpu_hard_bytes); !status) {
        return status;
    }

    auto safe_ratio = [](std::uint64_t value, std::uint64_t limit) -> double {
        if (limit == 0) {
            return 0.0;
        }
        return static_cast<double>(value) / static_cast<double>(limit);
    };
    auto classify = [](std::uint64_t value, std::uint64_t soft, std::uint64_t hard) -> std::string {
        if (hard > 0 && value >= hard) {
            return "hard";
        }
        if (soft > 0 && value >= soft) {
            return "soft";
        }
        return "ok";
    };
    auto severity_rank = [](std::string_view status) {
        if (status == "hard") {
            return 2;
        }
        if (status == "soft") {
            return 1;
        }
        return 0;
    };

    const double cpu_soft_ratio = safe_ratio(cpu_bytes, cpu_soft_bytes);
    const double cpu_hard_ratio = safe_ratio(cpu_bytes, cpu_hard_bytes);
    const double gpu_soft_ratio = safe_ratio(gpu_bytes, gpu_soft_bytes);
    const double gpu_hard_ratio = safe_ratio(gpu_bytes, gpu_hard_bytes);

    const bool cpu_soft_exceeded = cpu_soft_bytes > 0 && cpu_bytes >= cpu_soft_bytes;
    const bool cpu_hard_exceeded = cpu_hard_bytes > 0 && cpu_bytes >= cpu_hard_bytes;
    const bool gpu_soft_exceeded = gpu_soft_bytes > 0 && gpu_bytes >= gpu_soft_bytes;
    const bool gpu_hard_exceeded = gpu_hard_bytes > 0 && gpu_bytes >= gpu_hard_bytes;

    const std::string cpu_status = classify(cpu_bytes, cpu_soft_bytes, cpu_hard_bytes);
    const std::string gpu_status = classify(gpu_bytes, gpu_soft_bytes, gpu_hard_bytes);
    const std::string overall_status =
        severity_rank(cpu_status) >= severity_rank(gpu_status) ? cpu_status : gpu_status;

    if (auto status = replace_single<double>(space, base + "/cpuSoftBudgetRatio", cpu_soft_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/cpuHardBudgetRatio", cpu_hard_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuSoftBudgetRatio", gpu_soft_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuHardBudgetRatio", gpu_hard_ratio); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/cpuSoftExceeded", cpu_soft_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/cpuHardExceeded", cpu_hard_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/gpuSoftExceeded", gpu_soft_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/gpuHardExceeded", gpu_hard_exceeded); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/cpuStatus", cpu_status); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/gpuStatus", gpu_status); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/overallStatus", overall_status); !status) {
        return status;
    }

    return {};
}

} // namespace Diagnostics

} // namespace SP::UI::Builders
