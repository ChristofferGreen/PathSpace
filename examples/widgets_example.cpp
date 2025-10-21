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
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

constexpr int kGlyphRows = 7;
constexpr float kDefaultMargin = 32.0f;

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

auto measure_text_width(std::string_view text, float scale) -> float {
    auto upper = uppercase_copy(text);
    float width = 0.0f;
    for (char raw : upper) {
        if (raw == ' ') {
            width += scale * 4.0f;
            continue;
        }
        auto glyph = find_glyph(raw);
        if (!glyph) {
            width += scale * 4.0f;
            continue;
        }
        width += static_cast<float>(glyph->width) * scale;
        width += scale;
    }
    if (width > 0.0f) {
        width -= scale;
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

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto revision_base(ScenePath const& scene, std::uint64_t revision) -> std::string {
    return std::string(scene.getPath()) + "/builds/" + format_revision(revision);
}

auto decode_bucket_for_scene(PathSpace& space, ScenePath const& scene) -> SceneData::DrawableBucketSnapshot {
    auto revision = unwrap_or_exit(SceneBuilders::ReadCurrentRevision(space, scene),
                                  "read current revision for " + std::string(scene.getPath()));
    auto base = revision_base(scene, revision.revision);
    return unwrap_or_exit(SceneData::SceneSnapshotBuilder::decode_bucket(space, base),
                          "decode drawable bucket for " + base);
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
                       float scale,
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
    for (char raw : uppercase) {
        if (raw == ' ') {
            cursor_x += scale * 4.0f;
            continue;
        }
        auto glyph = find_glyph(raw);
        if (!glyph) {
            cursor_x += scale * 4.0f;
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
        cursor_x += static_cast<float>(glyph->width) * scale + scale;
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
};

auto build_gallery_bucket(PathSpace& space,
                          AppRootPathView appRoot,
                          Widgets::ButtonPaths const& button,
                          Widgets::ButtonStyle const& button_style,
                          std::string const& button_label,
                          Widgets::TogglePaths const& toggle,
                          Widgets::ToggleStyle const& toggle_style,
                          Widgets::SliderPaths const& slider,
                          Widgets::SliderStyle const& slider_style,
                          Widgets::SliderState const& slider_state,
                          Widgets::ListPaths const& list,
                          Widgets::ListStyle const& list_style,
                          std::vector<Widgets::ListItem> const& list_items) -> GalleryBuildResult {
    std::vector<SceneData::DrawableBucketSnapshot> pending;
    pending.reserve(16);

    float left = kDefaultMargin;
    float max_width = 0.0f;
    float max_height = 0.0f;
    std::uint64_t next_drawable_id = 0xA1000000ull;

    // Title text
    auto title_text = build_text_bucket("PathSpace Widgets",
                                        left,
                                        kDefaultMargin,
                                        5.0f,
                                        {0.93f, 0.95f, 0.98f, 1.0f},
                                        next_drawable_id++,
                                        "widget/gallery/title",
                                        0.4f);
    float cursor_y = kDefaultMargin;
    if (title_text) {
        pending.emplace_back(std::move(title_text->bucket));
        max_width = std::max(max_width, left + title_text->width);
        max_height = std::max(max_height, cursor_y + title_text->height);
        cursor_y += title_text->height + 24.0f;
    }

    // Button widget
    {
        auto bucket = decode_bucket_for_scene(space, button.scene);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        float widget_height = button_style.height;
        max_width = std::max(max_width, left + button_style.width);
        max_height = std::max(max_height, cursor_y + widget_height);

        float label_width = measure_text_width(button_label, 4.0f);
        float label_height = 4.0f * static_cast<float>(kGlyphRows);
        float label_x = left + std::max(0.0f, (button_style.width - label_width) * 0.5f);
        float label_y = cursor_y + std::max(0.0f, (button_style.height - label_height) * 0.5f);
        auto label = build_text_bucket(button_label,
                                       label_x,
                                       label_y,
                                       4.0f,
                                       button_style.text_color,
                                       next_drawable_id++,
                                       "widget/gallery/button/label",
                                       0.6f);
        if (label) {
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, label_x + label->width);
            max_height = std::max(max_height, label_y + label->height);
        }
        cursor_y += widget_height + 48.0f;
    }

    // Toggle widget
    {
        auto bucket = decode_bucket_for_scene(space, toggle.scene);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + toggle_style.width);
        max_height = std::max(max_height, cursor_y + toggle_style.height);

        auto label = build_text_bucket("Toggle",
                                       left + toggle_style.width + 24.0f,
                                       cursor_y + (toggle_style.height - 7.0f * 3.0f) * 0.5f,
                                       3.0f,
                                       {0.85f, 0.88f, 0.95f, 1.0f},
                                       next_drawable_id++,
                                       "widget/gallery/toggle/label",
                                       0.6f);
        if (label) {
            pending.emplace_back(std::move(label->bucket));
            max_width = std::max(max_width, left + toggle_style.width + 24.0f + label->width);
            max_height = std::max(max_height, cursor_y + label->height);
        }
        cursor_y += toggle_style.height + 40.0f;
    }

    // Slider widget with label
    {
        std::string slider_caption = "Volume " + std::to_string(static_cast<int>(std::round(slider_state.value)));
        auto caption = build_text_bucket(slider_caption,
                                         left,
                                         cursor_y,
                                         3.5f,
                                         {0.9f, 0.92f, 0.96f, 1.0f},
                                         next_drawable_id++,
                                         "widget/gallery/slider/caption",
                                         0.6f);
        if (caption) {
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + caption->height);
            cursor_y += caption->height + 12.0f;
        }

        auto bucket = decode_bucket_for_scene(space, slider.scene);
        translate_bucket(bucket, left, cursor_y);
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + slider_style.width);
        max_height = std::max(max_height, cursor_y + slider_style.height);
        cursor_y += slider_style.height + 48.0f;
    }

    // List widget with per-item labels
    {
        auto caption = build_text_bucket("Inventory",
                                         left,
                                         cursor_y,
                                         3.5f,
                                         {0.9f, 0.92f, 0.96f, 1.0f},
                                         next_drawable_id++,
                                         "widget/gallery/list/caption",
                                         0.6f);
        if (caption) {
            pending.emplace_back(std::move(caption->bucket));
            max_width = std::max(max_width, left + caption->width);
            max_height = std::max(max_height, cursor_y + caption->height);
            cursor_y += caption->height + 12.0f;
        }

        auto bucket = decode_bucket_for_scene(space, list.scene);
        translate_bucket(bucket, left, cursor_y);
        float list_height = list_style.item_height * static_cast<float>(std::max<std::size_t>(list_items.size(), 1u))
                            + list_style.border_thickness * 2.0f;
        pending.emplace_back(std::move(bucket));
        max_width = std::max(max_width, left + list_style.width);
        max_height = std::max(max_height, cursor_y + list_height);

        float text_scale = 3.0f;
        float content_top = cursor_y + list_style.border_thickness;
        for (std::size_t index = 0; index < list_items.size(); ++index) {
            auto const& item = list_items[index];
            float row_top = content_top + list_style.item_height * static_cast<float>(index);
            float row_height = list_style.item_height;
            float text_height = text_scale * static_cast<float>(kGlyphRows);
            float text_y = row_top + std::max(0.0f, (row_height - text_height) * 0.5f);
            float text_x = left + list_style.border_thickness + 16.0f;
            auto label = build_text_bucket(item.label,
                                           text_x,
                                           text_y,
                                           text_scale,
                                           {0.94f, 0.96f, 0.99f, 1.0f},
                                           next_drawable_id++,
                                           "widget/gallery/list/item/" + item.id,
                                           0.65f);
            if (label) {
                pending.emplace_back(std::move(label->bucket));
                max_width = std::max(max_width, text_x + label->width);
                max_height = std::max(max_height, text_y + label->height);
            }
        }
        cursor_y += list_height + 48.0f;
    }

    // Footer hint
    auto footer = build_text_bucket("Close window to exit",
                                    left,
                                    cursor_y,
                                    3.0f,
                                    {0.7f, 0.72f, 0.78f, 1.0f},
                                    next_drawable_id++,
                                    "widget/gallery/footer",
                                    0.6f);
    if (footer) {
        pending.emplace_back(std::move(footer->bucket));
        max_width = std::max(max_width, left + footer->width);
        max_height = std::max(max_height, cursor_y + footer->height);
        cursor_y += footer->height;
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
    return result;
}

struct GallerySceneResult {
    ScenePath scene;
    int width = 0;
    int height = 0;
};

auto publish_gallery_scene(PathSpace& space,
                           AppRootPathView appRoot,
                           Widgets::ButtonPaths const& button,
                           Widgets::ButtonStyle const& button_style,
                           std::string const& button_label,
                           Widgets::TogglePaths const& toggle,
                           Widgets::ToggleStyle const& toggle_style,
                           Widgets::SliderPaths const& slider,
                           Widgets::SliderStyle const& slider_style,
                           Widgets::SliderState const& slider_state,
                           Widgets::ListPaths const& list,
                           Widgets::ListStyle const& list_style,
                           std::vector<Widgets::ListItem> const& list_items) -> GallerySceneResult {
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
                                      button_label,
                                      toggle,
                                      toggle_style,
                                      slider,
                                      slider_style,
                                      slider_state,
                                      list,
                                      list_style,
                                      list_items);

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
    };
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
    namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
    namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;

    PathSpace space;
    AppRootPath appRoot{"/system/applications/widgets_example"};
    AppRootPathView appRootView{appRoot.getPath()};

    Widgets::ButtonParams button_params{};
    button_params.name = "primary_button";
    button_params.label = "Primary";
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

    // Reducer demonstration (unchanged behaviour).
    auto button_queue = WidgetReducers::WidgetOpsQueue(button.root);
    WidgetBindings::WidgetOp activate{};
    activate.kind = WidgetBindings::WidgetOpKind::Activate;
    activate.widget_path = button.root.getPath();
    activate.value = 1.0f;
    activate.sequence = 1;
    activate.timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto inserted = space.insert(button_queue.getPath(), activate);
    if (!inserted.errors.empty()) {
        std::cerr << "Failed to enqueue sample widget op: "
                  << inserted.errors.front().message.value_or("unknown error") << "\n";
        return 12;
    }

    auto reduced = unwrap_or_exit(WidgetReducers::ReducePending(space,
                                                                ConcretePathStringView{button_queue.getPath()}),
                                  "reduce widget ops");
    if (!reduced.empty()) {
        auto actions_queue = WidgetReducers::DefaultActionsQueue(button.root);
        auto span = std::span<const WidgetReducers::WidgetAction>(reduced.data(), reduced.size());
        unwrap_or_exit(WidgetReducers::PublishActions(space,
                                                      ConcretePathStringView{actions_queue.getPath()},
                                                      span),
                       "publish widget actions");
        auto action = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue.getPath());
        if (action) {
            std::cout << "Reducer emitted action:\n"
                      << "  widget: " << action->widget_path << "\n"
                      << "  kind: " << static_cast<int>(action->kind) << "\n"
                      << "  value: " << action->analog_value << "\n";
        }
    }

    bool headless = false;
    if (const char* headless_env = std::getenv("WIDGETS_EXAMPLE_HEADLESS")) {
        if (headless_env[0] != '\0' && headless_env[0] != '0') {
            headless = true;
        }
    }

    auto gallery = publish_gallery_scene(space,
                                         appRootView,
                                         button,
                                         button_params.style,
                                         button_params.label,
                                         toggle,
                                         toggle_params.style,
                                         slider,
                                         slider_params.style,
                                         slider_state,
                                         list,
                                         list_params.style,
                                         list_params.items);

    std::cout << "widgets_example gallery scene:\n"
              << "  scene: " << gallery.scene.getPath() << "\n"
              << "  size : " << gallery.width << "x" << gallery.height << " pixels\n";

    if (headless) {
        std::cout << "widgets_example exiting headless mode (WIDGETS_EXAMPLE_HEADLESS set).\n";
        return 0;
    }

    SP::UI::SetLocalWindowCallbacks({});
    SP::UI::InitLocalWindowWithSize(gallery.width,
                                    gallery.height,
                                    "PathSpace Widgets Gallery");

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

    WindowParams window_params{
        .name = "gallery_window",
        .title = "PathSpace Widgets Gallery",
        .width = gallery.width,
        .height = gallery.height,
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
    renderer_settings.surface.size_px.width = gallery.width;
    renderer_settings.surface.size_px.height = gallery.height;
    unwrap_or_exit(Renderer::UpdateSettings(space,
                                            ConcretePathStringView{target_absolute.getPath()},
                                            renderer_settings),
                   "update renderer settings");

    Builders::DirtyRectHint initial_hint{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(gallery.width),
        .max_y = static_cast<float>(gallery.height),
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
    int window_width = gallery.width;
    int window_height = gallery.height;

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

return 0;
}

#endif // PATHSPACE_ENABLE_UI
