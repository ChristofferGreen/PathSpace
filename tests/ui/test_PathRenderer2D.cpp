#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <sstream>
#include <string_view>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_STATIC

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
using SP::UI::MaterialResourceResidency;
using SP::UI::Scene::BoundingBox;
using SP::UI::Scene::BoundingSphere;
using SP::UI::Scene::DrawableAuthoringMapEntry;
using SP::UI::Scene::DrawableBucketSnapshot;
using SP::UI::Scene::SceneSnapshotBuilder;
using SP::UI::Scene::SnapshotPublishOptions;
using SP::UI::Scene::Transform;
using SP::UI::Scene::RoundedRectCommand;
using SP::UI::Scene::RectCommand;
using SP::UI::Scene::DrawCommandKind;
using SP::UI::Scene::ImageCommand;
using SP::UI::Scene::TextGlyphsCommand;
using SP::UI::Scene::PathCommand;
using SP::UI::Scene::MeshCommand;
using SP::UI::Scene::StrokeCommand;
using namespace SP::UI::PipelineFlags;
namespace UIScene = SP::UI::Scene;

namespace SP::UI::Builders {
auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& targetPath,
                                PathWindowView::PresentStats const& stats,
                                PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool>;
}

namespace {

struct ScopedEnv {
    std::string name;
    std::optional<std::string> previous;

    ScopedEnv(char const* key, char const* value) : name(key) {
        if (auto* existing = std::getenv(key)) {
            previous = std::string(existing);
        }
        if (value) {
            ::setenv(name.c_str(), value, 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    ~ScopedEnv() {
        if (previous.has_value()) {
            ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

struct RendererFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/test_app"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_snapshot(ScenePath const& scenePath,
                          DrawableBucketSnapshot bucket) -> std::uint64_t {
        SceneSnapshotBuilder builder{space, root_view(), scenePath};
        SnapshotPublishOptions opts{};
        opts.metadata.author = "tests";
        opts.metadata.tool_version = "tests";
        opts.metadata.created_at = std::chrono::system_clock::time_point{};
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();
        auto revision = builder.publish(opts, bucket);
        REQUIRE(revision);
        return *revision;
    }
};

void enable_framebuffer_capture(PathSpace& space,
                                WindowPath const& windowPath,
                                std::string_view viewName) {
    auto viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    auto result = space.insert(viewBase + "/present/params/capture_framebuffer", true);
    REQUIRE(result.errors.empty());
}

auto create_scene(RendererFixture& fx,
                  std::string const& name,
                  DrawableBucketSnapshot bucket) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "Test scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    fx.publish_snapshot(*scene, std::move(bucket));
    return *scene;
}

auto create_renderer(RendererFixture& fx, std::string const& name) -> RendererPath {
    RendererParams params{
        .name = name,
        .kind = RendererKind::Software2D,
        .description = "Test renderer",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);
    return *renderer;
}

auto create_surface(RendererFixture& fx,
                    std::string const& name,
                    Builders::SurfaceDesc desc,
                    std::string const& rendererName) -> SurfacePath {
    SurfaceParams params{};
    params.name = name;
    params.desc = desc;
    params.renderer = rendererName;
    auto surface = Surface::Create(fx.space, fx.root_view(), params);
    REQUIRE(surface);
    return *surface;
}

auto resolve_target(RendererFixture& fx,
                    SurfacePath const& surfacePath) -> SP::ConcretePathString {
    auto targetRel = fx.space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);
    return SP::ConcretePathString{targetAbs->getPath()};
}

auto identity_transform() -> Transform {
    Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

auto encode_rect_command(RectCommand const& rect,
                         DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(RectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Rect));
}

auto encode_rounded_rect_command(RoundedRectCommand const& rounded,
                                 DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rounded, sizeof(RoundedRectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::RoundedRect));
}

auto encode_mesh_command(UIScene::MeshCommand const& mesh,
                         DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::MeshCommand));
    std::memcpy(bucket.command_payload.data() + offset, &mesh, sizeof(UIScene::MeshCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Mesh));
}

auto encode_text_glyphs_command(UIScene::TextGlyphsCommand const& glyphs,
                                DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::TextGlyphsCommand));
    std::memcpy(bucket.command_payload.data() + offset, &glyphs, sizeof(UIScene::TextGlyphsCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::TextGlyphs));
}

auto encode_path_command(UIScene::PathCommand const& path,
                         DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::PathCommand));
    std::memcpy(bucket.command_payload.data() + offset, &path, sizeof(UIScene::PathCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Path));
}

auto encode_stroke_command(UIScene::StrokeCommand const& stroke,
                           DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::StrokeCommand));
    std::memcpy(bucket.command_payload.data() + offset, &stroke, sizeof(UIScene::StrokeCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Stroke));
}

auto encode_image_command(ImageCommand const& image,
                          DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(ImageCommand));
    std::memcpy(bucket.command_payload.data() + offset, &image, sizeof(ImageCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Image));
}

struct RectDrawableDef {
    std::uint64_t id = 0;
    std::uint64_t fingerprint = 0;
    RectCommand   rect{};
};

auto make_rect_bucket(std::vector<RectDrawableDef> const& defs) -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    auto count = defs.size();
    bucket.drawable_ids.reserve(count);
    bucket.world_transforms.reserve(count);
    bucket.bounds_spheres.reserve(count);
    bucket.bounds_boxes.reserve(count);
    bucket.bounds_box_valid.reserve(count);
    bucket.layers.reserve(count);
    bucket.z_values.reserve(count);
    bucket.material_ids.reserve(count);
    bucket.pipeline_flags.reserve(count);
    bucket.visibility.reserve(count);
    bucket.command_offsets.reserve(count);
    bucket.command_counts.reserve(count);
    bucket.clip_head_indices.reserve(count);
    bucket.authoring_map.reserve(count);
    bucket.drawable_fingerprints.reserve(count);

    for (std::size_t index = 0; index < defs.size(); ++index) {
        auto const& def = defs[index];
        bucket.drawable_ids.push_back(def.id);
        bucket.world_transforms.push_back(identity_transform());

        BoundingBox box{};
        box.min = {def.rect.min_x, def.rect.min_y, 0.0f};
        box.max = {def.rect.max_x, def.rect.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto width = std::max(def.rect.max_x - def.rect.min_x, 0.0f);
        auto height = std::max(def.rect.max_y - def.rect.min_y, 0.0f);
        float radius = std::sqrt(width * width + height * height) * 0.5f;
        BoundingSphere sphere{};
        sphere.center = {(def.rect.min_x + def.rect.max_x) * 0.5f,
                         (def.rect.min_y + def.rect.max_y) * 0.5f,
                         0.0f};
        sphere.radius = radius;
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(index));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_counts.push_back(1);
        bucket.clip_head_indices.push_back(-1);
        bucket.authoring_map.push_back(DrawableAuthoringMapEntry{
            def.id,
            "drawable_" + std::to_string(index),
            0,
            0,
        });
        bucket.drawable_fingerprints.push_back(def.fingerprint);

        encode_rect_command(def.rect, bucket);
    }

    bucket.opaque_indices.resize(defs.size());
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0u);
    bucket.alpha_indices.clear();

    return bucket;
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

constexpr std::array<std::uint8_t, 78> kTestPngRgba = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,
    0,0,0,21,73,68,65,84,120,156,99,248,207,192,240,31,8,27,24,128,52,8,56,0,0,68,19,8,185,
    109,230,62,33,0,0,0,0,73,69,78,68,174,66,96,130
};

struct LinearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

auto srgb_to_linear(float value) -> float {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto linear_to_srgb(float value) -> float {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

auto make_linear_color(std::array<float, 4> const& rgba) -> LinearColor {
    auto alpha = std::clamp(rgba[3], 0.0f, 1.0f);
    return LinearColor{
        .r = srgb_to_linear(rgba[0]) * alpha,
        .g = srgb_to_linear(rgba[1]) * alpha,
        .b = srgb_to_linear(rgba[2]) * alpha,
        .a = alpha,
    };
}

auto blend(LinearColor dest, LinearColor src) -> LinearColor {
    auto const inv = 1.0f - src.a;
    dest.r = src.r + dest.r * inv;
    dest.g = src.g + dest.g * inv;
    dest.b = src.b + dest.b * inv;
    dest.a = src.a + dest.a * inv;
    return dest;
}

auto encode_linear_to_bytes(LinearColor color,
                            Builders::SurfaceDesc const& desc,
                            bool encode_srgb) -> std::array<std::uint8_t, 4> {
    auto alpha = std::clamp(color.a, 0.0f, 1.0f);
    std::array<float, 3> premul{
        std::clamp(color.r, 0.0f, 1.0f),
        std::clamp(color.g, 0.0f, 1.0f),
        std::clamp(color.b, 0.0f, 1.0f),
    };

    std::array<float, 3> straight{0.0f, 0.0f, 0.0f};
    if (alpha > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            straight[i] = std::clamp(premul[i] / alpha, 0.0f, 1.0f);
        }
    }

    auto apply_channel = [&](float linear_value) -> float {
        if (encode_srgb) {
            return linear_to_srgb(linear_value);
        }
        return linear_value;
    };

    std::array<float, 3> encoded{};
    for (int i = 0; i < 3; ++i) {
        auto value = apply_channel(straight[i]);
        if (desc.premultiplied_alpha) {
            value *= alpha;
        }
        encoded[i] = std::clamp(value, 0.0f, 1.0f);
    }

    return {
        static_cast<std::uint8_t>(std::lround(encoded[0] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(encoded[1] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(encoded[2] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(alpha * 255.0f)),
    };
}

auto copy_buffer(PathSurfaceSoftware& surface) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> buffer(surface.frame_bytes(), 0);
    auto copied = surface.copy_buffered_frame(buffer);
    if (!copied.has_value()) {
        auto dirty = surface.consume_progressive_dirty_tiles();
        if (!dirty.empty() && surface.has_progressive()) {
            auto& progressive = surface.progressive_buffer();
            std::vector<std::uint8_t> tile_storage;
            tile_storage.reserve(static_cast<std::size_t>(progressive.tile_size())
                                 * static_cast<std::size_t>(progressive.tile_size()) * 4u);
            auto row_stride = surface.row_stride_bytes();
            for (auto tile_index : dirty) {
                auto dims = progressive.tile_dimensions(tile_index);
                if (dims.width <= 0 || dims.height <= 0) {
                    continue;
                }
                auto const row_pitch = static_cast<std::size_t>(dims.width) * 4u;
                tile_storage.resize(static_cast<std::size_t>(dims.height) * row_pitch);
                auto copied_tile = progressive.copy_tile(tile_index, tile_storage);
                if (!copied_tile) {
                    continue;
                }
                for (int row = 0; row < dims.height; ++row) {
                    auto const src_offset = static_cast<std::size_t>(row) * row_pitch;
                    auto const dst_offset = (static_cast<std::size_t>(dims.y + row) * row_stride)
                                            + static_cast<std::size_t>(dims.x) * 4u;
                    std::memcpy(buffer.data() + dst_offset,
                                tile_storage.data() + src_offset,
                                row_pitch);
                }
            }
            return buffer;
        }
    }
    REQUIRE(copied.has_value());
    return buffer;
}

struct GoldenBuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bytes;
};

auto golden_dir() -> std::filesystem::path {
    return std::filesystem::path{PATHSPACE_SOURCE_DIR} / "tests" / "ui" / "golden";
}

auto golden_path(std::string_view name) -> std::filesystem::path {
    return golden_dir() / std::filesystem::path{name};
}

auto env_update_goldens() -> bool {
    if (auto* value = std::getenv("PATHSPACE_UPDATE_GOLDENS")) {
        std::string_view view{value};
        return !(view.empty() || view == "0" || view == "false" || view == "FALSE");
    }
    return false;
}

auto strip_non_hex(std::string_view input) -> std::string {
    std::string output;
    output.reserve(input.size());
    for (char ch : input) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return output;
}

auto hex_to_bytes(std::string_view hex) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto from_hex = [](unsigned char ch) -> std::uint8_t {
            if (ch >= '0' && ch <= '9') {
                return static_cast<std::uint8_t>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f') {
                return static_cast<std::uint8_t>(10 + (ch - 'a'));
            }
            if (ch >= 'A' && ch <= 'F') {
                return static_cast<std::uint8_t>(10 + (ch - 'A'));
            }
            return 0;
        };
        auto value = static_cast<std::uint8_t>((from_hex(static_cast<unsigned char>(hex[i])) << 4)
                                               | from_hex(static_cast<unsigned char>(hex[i + 1])));
        bytes.push_back(value);
    }
    return bytes;
}

auto read_golden(std::string_view name) -> std::optional<GoldenBuffer> {
    auto path = golden_path(name);
    std::ifstream file{path};
    if (!file) {
        return std::nullopt;
    }

    GoldenBuffer golden{};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream iss{line};
        if (!(iss >> golden.width >> golden.height)) {
            continue;
        }
        break;
    }

    std::string hex_data;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        hex_data += strip_non_hex(line);
    }

    golden.bytes = hex_to_bytes(hex_data);
    return golden;
}

void write_golden(std::string_view name,
                  Builders::SurfaceDesc const& desc,
                  std::span<std::uint8_t const> bytes) {
    auto dir = golden_dir();
    std::filesystem::create_directories(dir);
    auto path = golden_path(name);
    std::ofstream file{path, std::ios::trunc};
    REQUIRE(file.good());
    file << "# PathRenderer2D golden framebuffer\n";
    file << desc.size_px.width << ' ' << desc.size_px.height << '\n';
    file << std::hex << std::setfill('0');
    auto row_stride = static_cast<std::size_t>(desc.size_px.width) * 4u;
    for (int y = 0; y < desc.size_px.height; ++y) {
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (std::size_t i = 0; i < row_stride; ++i) {
            auto value = bytes[base + i];
            file << std::setw(2) << static_cast<int>(value);
        }
        file << '\n';
    }
    file << std::dec;
}

auto join_strings(std::vector<std::string> const& parts) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << parts[i];
    }
    return oss.str();
}

void expect_matches_golden(std::string_view name,
                           Builders::SurfaceDesc const& desc,
                           std::span<std::uint8_t const> buffer,
                           std::uint8_t tolerance = 1) {
    if (env_update_goldens()) {
        write_golden(name, desc, buffer);
        return;
    }

    auto golden = read_golden(name);
    REQUIRE_MESSAGE(golden.has_value(),
                    "Missing golden file '" << golden_path(name).string()
                                            << "'. Run with PATHSPACE_UPDATE_GOLDENS=1 to generate.");
    REQUIRE(golden->width == desc.size_px.width);
    REQUIRE(golden->height == desc.size_px.height);
    REQUIRE(golden->bytes.size() == buffer.size());

    std::vector<std::string> mismatches;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        auto actual = buffer[i];
        auto expected = golden->bytes[i];
        auto delta = static_cast<int>(actual) - static_cast<int>(expected);
        if (std::abs(delta) > tolerance) {
            if (mismatches.size() < 8) {
                std::ostringstream oss;
                oss << "idx=" << i
                    << " actual=0x" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(actual)
                    << " expected=0x" << std::setw(2) << static_cast<int>(expected)
                    << " delta=" << std::dec << delta;
                mismatches.push_back(oss.str());
            }
        }
    }

    if (!mismatches.empty()) {
        FAIL("Framebuffer diverged from golden '" << name << "': "
             << join_strings(mismatches));
    }
}

} // namespace

TEST_SUITE("PathRenderer2D") {

TEST_CASE("render executes rect commands across passes and encodes pixels") {
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x010000u, 0x000100u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{2.0f, 2.0f, 0.0f}, 3.0f},
                             BoundingSphere{{2.0f, 2.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}},
        BoundingBox{{1.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.5f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node1", 0, 0},
    };

    RectCommand base_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    encode_rect_command(base_rect, bucket);

    RoundedRectCommand overlay_rect{
        .min_x = 1.0f,
        .min_y = 1.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .radius_top_left = 0.25f,
        .radius_top_right = 0.25f,
        .radius_bottom_right = 0.25f,
        .radius_bottom_left = 0.25f,
        .color = {0.0f, 1.0f, 0.0f, 0.5f},
    };
    encode_rounded_rect_command(overlay_rect, bucket);

    auto scenePath = create_scene(fx, "scene_rects", bucket);
    auto rendererPath = create_renderer(fx, "renderer_rects");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_rects", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.1f, 0.2f, 0.3f, 1.0f};
    settings.time.frame_index = 7;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 2);
    CHECK(result->backend_kind == RendererKind::Software2D);

    auto buffer = copy_buffer(surface);
    expect_matches_golden("pathrenderer2d_rect_rrect_rgba8srgb.golden",
                          surfaceDesc,
                          std::span<std::uint8_t const>{buffer.data(), buffer.size()});
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 7);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/revision").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/culledDrawables").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueSortViolations").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() == 0);
    CHECK(fx.space.read<double>(metricsBase + "/approxOpaquePixels").value() == doctest::Approx(16.0));
    CHECK(fx.space.read<double>(metricsBase + "/approxAlphaPixels").value() == doctest::Approx(4.0));
    CHECK(fx.space.read<double>(metricsBase + "/approxDrawablePixels").value() == doctest::Approx(20.0));
    CHECK(fx.space.read<double>(metricsBase + "/approxOverdrawFactor").value() == doctest::Approx(1.25));
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesUpdated").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveBytesCopied").value() > 0);
    auto damageRects = fx.space.read<uint64_t>(metricsBase + "/damageRectangles");
    REQUIRE(damageRects);
    auto damageTiles = fx.space.read<std::vector<Builders::DirtyRectHint>>(metricsBase + "/damageTiles");
    REQUIRE(damageTiles);
    CHECK(damageTiles->size() == *damageRects);
    auto damageCoverage = fx.space.read<double>(metricsBase + "/damageCoverageRatio");
    REQUIRE(damageCoverage);
    CHECK(*damageCoverage == doctest::Approx(1.0));
    auto matchExact = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesExact");
    REQUIRE(matchExact);
    CHECK(*matchExact == 0);
    auto matchRemap = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesRemap");
    REQUIRE(matchRemap);
    CHECK(*matchRemap == 0);
    auto changed = fx.space.read<uint64_t>(metricsBase + "/fingerprintChanges");
    REQUIRE(changed);
    CHECK(*changed == 0);
    auto added = fx.space.read<uint64_t>(metricsBase + "/fingerprintNew");
    REQUIRE(added);
    CHECK(*added == 2);
    auto removed = fx.space.read<uint64_t>(metricsBase + "/fingerprintRemoved");
    REQUIRE(removed);
    CHECK(*removed == 0);
    auto tilesDirty = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesDirty");
    REQUIRE(tilesDirty);
    CHECK(*tilesDirty >= 1);
    auto tilesTotal = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesTotal");
    REQUIRE(tilesTotal);
    CHECK(*tilesTotal >= 1);
    auto tilesSkipped = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesSkipped");
    REQUIRE(tilesSkipped);
    CHECK(*tilesSkipped == *tilesTotal - *tilesDirty);

    auto encode_srgb = true;
    auto clear_linear = make_linear_color(settings.clear_color);
    auto base_linear = make_linear_color(base_rect.color);
    auto overlay_linear = make_linear_color(overlay_rect.color);
    auto desc = surfaceDesc;

    auto pixel_bytes = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        auto color = clear_linear;
        color = blend(color, base_linear);
        if (x >= 1 && x < 3 && y >= 1 && y < 3) {
            color = blend(color, overlay_linear);
        }
        auto encoded = encode_linear_to_bytes(color, desc, encode_srgb);
        if (surfaceDesc.pixel_format == PixelFormat::RGBA8Unorm_sRGB) {
            return encoded;
        }
        return encoded;
    };

    auto stride = surface.row_stride_bytes();
    auto &desc_ref = surfaceDesc;
    auto check_pixel = [&](int x, int y, std::array<std::uint8_t, 4> expected) {
        auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
        CHECK(buffer[offset + 0] == expected[0]);
        CHECK(buffer[offset + 1] == expected[1]);
        CHECK(buffer[offset + 2] == expected[2]);
        CHECK(buffer[offset + 3] == expected[3]);
    };

    check_pixel(0, 0, pixel_bytes(0, 0));
    check_pixel(1, 1, pixel_bytes(1, 1));
    check_pixel(3, 3, pixel_bytes(3, 3));

    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
}

TEST_CASE("progressive tile size widens for high resolution surfaces") {
    RendererFixture fx;

    constexpr int kWidth = 6144;
    constexpr int kHeight = 256;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x0A0000u};
    bucket.drawable_fingerprints = {0x0101u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                                             static_cast<float>(kHeight) * 0.5f,
                                             0.0f},
                                            static_cast<float>(std::max(kWidth, kHeight))}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f},
                    {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "large", 0, 0},
    };

    RectCommand background{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(kWidth),
        .max_y = static_cast<float>(kHeight),
        .color = {0.2f, 0.2f, 0.2f, 1.0f},
    };
    encode_rect_command(background, bucket);

    auto scenePath = create_scene(fx, "scene_large_surface", bucket);
    auto rendererPath = create_renderer(fx, "renderer_large_surface");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_large_surface", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(result);
    CHECK(surface.progressive_buffer().tile_size() == 128);
}

TEST_CASE("progressive tile size collapses after localized damage") {
    RendererFixture fx;

    constexpr int kWidth = 6144;
    constexpr int kHeight = 256;

    auto make_bucket = [&](std::array<float, 4> overlay_color, std::uint64_t overlay_fp) {
        DrawableBucketSnapshot bucket{};
        bucket.drawable_ids = {0x0B0000u, 0x0B0001u};
        bucket.drawable_fingerprints = {0x0201u, overlay_fp};
        bucket.world_transforms = {identity_transform(), identity_transform()};
        bucket.bounds_spheres = {
            BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                            static_cast<float>(kHeight) * 0.5f,
                            0.0f},
                           static_cast<float>(std::max(kWidth, kHeight))},
            BoundingSphere{{192.0f, 80.0f, 0.0f}, 64.0f},
        };
        bucket.bounds_boxes = {
            BoundingBox{{0.0f, 0.0f, 0.0f},
                        {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
            BoundingBox{{128.0f, 32.0f, 0.0f}, {192.0f, 96.0f, 0.0f}},
        };
        bucket.bounds_box_valid = {1, 1};
        bucket.layers = {0, 0};
        bucket.z_values = {0.0f, 0.5f};
        bucket.material_ids = {1, 1};
        bucket.pipeline_flags = {0, 0};
        bucket.visibility = {1, 1};
        bucket.command_offsets = {0, 1};
        bucket.command_counts = {1, 1};
        bucket.opaque_indices = {0};
        bucket.alpha_indices = {1};
        bucket.clip_head_indices = {-1, -1};
        bucket.authoring_map = {
            DrawableAuthoringMapEntry{bucket.drawable_ids[0], "bg", 0, 0},
            DrawableAuthoringMapEntry{bucket.drawable_ids[1], "overlay", 0, 0},
        };

        RectCommand background{
            .min_x = 0.0f,
            .min_y = 0.0f,
            .max_x = static_cast<float>(kWidth),
            .max_y = static_cast<float>(kHeight),
            .color = {0.15f, 0.15f, 0.15f, 1.0f},
        };
        encode_rect_command(background, bucket);

        RectCommand overlay{
            .min_x = 128.0f,
            .min_y = 32.0f,
            .max_x = 192.0f,
            .max_y = 96.0f,
            .color = overlay_color,
        };
        encode_rect_command(overlay, bucket);

        return bucket;
    };

    auto initial_bucket = make_bucket({0.0f, 1.0f, 0.0f, 1.0f}, 0x0301u);
    auto scenePath = create_scene(fx, "scene_tile_retarget", initial_bucket);
    auto rendererPath = create_renderer(fx, "renderer_tile_retarget");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_tile_retarget", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(first);
    CHECK(surface.progressive_buffer().tile_size() == 128);

    auto updated_bucket = make_bucket({0.0f, 0.0f, 1.0f, 1.0f}, 0x0302u);
    auto revision = fx.publish_snapshot(scenePath, updated_bucket);
    REQUIRE(revision >= 2);

    settings.time.frame_index = 2;
    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);
    CHECK(surface.progressive_buffer().tile_size() == 64);
}

TEST_CASE("damage metrics reflect localized drawable updates") {
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RendererFixture fx;

    constexpr int kWidth = 512;
    constexpr int kHeight = 512;

    auto make_bucket = [&](std::array<float, 4> overlay_color, std::uint64_t overlay_fp) {
        DrawableBucketSnapshot bucket{};
        bucket.drawable_ids = {0x0C0000u, 0x0C0001u};
        bucket.drawable_fingerprints = {0x0401u, overlay_fp};
        bucket.world_transforms = {identity_transform(), identity_transform()};
        bucket.bounds_spheres = {
            BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                            static_cast<float>(kHeight) * 0.5f,
                            0.0f},
                           static_cast<float>(std::max(kWidth, kHeight))},
            BoundingSphere{{256.0f, 256.0f, 0.0f}, 96.0f},
        };
        bucket.bounds_boxes = {
            BoundingBox{{0.0f, 0.0f, 0.0f},
                        {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
            BoundingBox{{192.0f, 192.0f, 0.0f}, {320.0f, 320.0f, 0.0f}},
        };
        bucket.bounds_box_valid = {1, 1};
        bucket.layers = {0, 0};
        bucket.z_values = {0.0f, 0.5f};
        bucket.material_ids = {1, 1};
        bucket.pipeline_flags = {0, 0};
        bucket.visibility = {1, 1};
        bucket.command_offsets = {0, 1};
        bucket.command_counts = {1, 1};
        bucket.opaque_indices = {0};
        bucket.alpha_indices = {1};
        bucket.clip_head_indices = {-1, -1};
        bucket.authoring_map = {
            DrawableAuthoringMapEntry{bucket.drawable_ids[0], "background", 0, 0},
            DrawableAuthoringMapEntry{bucket.drawable_ids[1], "overlay", 0, 0},
        };

        RectCommand background{
            .min_x = 0.0f,
            .min_y = 0.0f,
            .max_x = static_cast<float>(kWidth),
            .max_y = static_cast<float>(kHeight),
            .color = {0.10f, 0.10f, 0.10f, 1.0f},
        };
        encode_rect_command(background, bucket);

        RectCommand overlay{
            .min_x = 192.0f,
            .min_y = 192.0f,
            .max_x = 320.0f,
            .max_y = 320.0f,
            .color = overlay_color,
        };
        encode_rect_command(overlay, bucket);

        return bucket;
    };

    auto initial_bucket = make_bucket({0.2f, 0.6f, 0.9f, 1.0f}, 0x0501u);
    auto scenePath = create_scene(fx, "scene_damage_metrics", initial_bucket);
    auto rendererPath = create_renderer(fx, "renderer_damage_metrics");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_damage_metrics", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(first);

    auto updated_bucket = make_bucket({0.9f, 0.3f, 0.2f, 1.0f}, 0x0502u);
    auto revision = fx.publish_snapshot(scenePath, updated_bucket);
    REQUIRE(revision >= 2);

    settings.time.frame_index = 2;
    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto damageRects = fx.space.read<uint64_t>(metricsBase + "/damageRectangles");
    REQUIRE(damageRects);
    auto damageTilesRemoval = fx.space.read<std::vector<Builders::DirtyRectHint>>(metricsBase + "/damageTiles");
    REQUIRE(damageTilesRemoval);
    CHECK(damageTilesRemoval->size() == *damageRects);

    auto damageCoverage = fx.space.read<double>(metricsBase + "/damageCoverageRatio");
    REQUIRE(damageCoverage);
    CHECK(*damageCoverage == doctest::Approx(0.065).epsilon(0.2));

    auto matchesExact = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesExact");
    REQUIRE(matchesExact);
    CHECK(*matchesExact == 1);

    auto fingerprintChanges = fx.space.read<uint64_t>(metricsBase + "/fingerprintChanges");
    REQUIRE(fingerprintChanges);
    CHECK(*fingerprintChanges == 1);

    auto fingerprintRemoved = fx.space.read<uint64_t>(metricsBase + "/fingerprintRemoved");
    REQUIRE(fingerprintRemoved);
    CHECK(*fingerprintRemoved == 0);

    auto progressiveDirty = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesDirty");
    REQUIRE(progressiveDirty);
    CHECK(*progressiveDirty >= 1);
}

TEST_CASE("damage metrics capture drawable removal") {
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RendererFixture fx;

    constexpr int kWidth = 512;
    constexpr int kHeight = 512;

    DrawableBucketSnapshot initial{};
    initial.drawable_ids = {0x0D0000u, 0x0D0001u};
    initial.drawable_fingerprints = {0x0601u, 0x0602u};
    initial.world_transforms = {identity_transform(), identity_transform()};
    initial.bounds_spheres = {
        BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                        static_cast<float>(kHeight) * 0.5f,
                        0.0f},
                       static_cast<float>(std::max(kWidth, kHeight))},
        BoundingSphere{{128.0f, 128.0f, 0.0f}, 64.0f},
    };
    initial.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f},
                    {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
        BoundingBox{{96.0f, 96.0f, 0.0f}, {160.0f, 160.0f, 0.0f}},
    };
    initial.bounds_box_valid = {1, 1};
    initial.layers = {0, 0};
    initial.z_values = {0.0f, 0.5f};
    initial.material_ids = {1, 1};
    initial.pipeline_flags = {0, 0};
    initial.visibility = {1, 1};
    initial.command_offsets = {0, 1};
    initial.command_counts = {1, 1};
    initial.opaque_indices = {0};
    initial.alpha_indices = {1};
    initial.clip_head_indices = {-1, -1};
    initial.authoring_map = {
        DrawableAuthoringMapEntry{initial.drawable_ids[0], "background", 0, 0},
        DrawableAuthoringMapEntry{initial.drawable_ids[1], "marker", 0, 0},
    };

    RectCommand background{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(kWidth),
        .max_y = static_cast<float>(kHeight),
        .color = {0.05f, 0.05f, 0.05f, 1.0f},
    };
    encode_rect_command(background, initial);

    RectCommand marker{
        .min_x = 96.0f,
        .min_y = 96.0f,
        .max_x = 160.0f,
        .max_y = 160.0f,
        .color = {0.9f, 0.1f, 0.2f, 1.0f},
    };
    encode_rect_command(marker, initial);

    auto scenePath = create_scene(fx, "scene_damage_removal", initial);
    auto rendererPath = create_renderer(fx, "renderer_damage_removal");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_damage_removal", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(first);

    DrawableBucketSnapshot removal{};
    removal.drawable_ids = {0x0D0000u};
    removal.drawable_fingerprints = {0x0601u};
    removal.world_transforms = {identity_transform()};
    removal.bounds_spheres = {initial.bounds_spheres[0]};
    removal.bounds_boxes = {initial.bounds_boxes[0]};
    removal.bounds_box_valid = {1};
    removal.layers = {0};
    removal.z_values = {0.0f};
    removal.material_ids = {1};
    removal.pipeline_flags = {0};
    removal.visibility = {1};
    removal.command_offsets = {0};
    removal.command_counts = {1};
    removal.opaque_indices = {0};
    removal.alpha_indices = {};
    removal.clip_head_indices = {-1};
    removal.authoring_map = {
        DrawableAuthoringMapEntry{removal.drawable_ids[0], "background", 0, 0},
    };
    encode_rect_command(background, removal);

    auto revision = fx.publish_snapshot(scenePath, removal);
    REQUIRE(revision >= 2);

    settings.time.frame_index = 2;
    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto fingerprintRemoved = fx.space.read<uint64_t>(metricsBase + "/fingerprintRemoved");
    REQUIRE(fingerprintRemoved);
    CHECK(*fingerprintRemoved == 1);

    auto fingerprintMatchesExact = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesExact");
    REQUIRE(fingerprintMatchesExact);
    CHECK(*fingerprintMatchesExact == 1);

    auto fingerprintNew = fx.space.read<uint64_t>(metricsBase + "/fingerprintNew");
    REQUIRE(fingerprintNew);
    CHECK(*fingerprintNew == 0);

    auto damageRects = fx.space.read<uint64_t>(metricsBase + "/damageRectangles");
    REQUIRE(damageRects);
    auto damageTilesFull = fx.space.read<std::vector<Builders::DirtyRectHint>>(metricsBase + "/damageTiles");
    REQUIRE(damageTilesFull);
    CHECK(damageTilesFull->size() == *damageRects);

    auto damageCoverage = fx.space.read<double>(metricsBase + "/damageCoverageRatio");
    REQUIRE(damageCoverage);
    CHECK(*damageCoverage == doctest::Approx(0.05).epsilon(0.3));
}

TEST_CASE("damage metrics detect clear color repaint") {
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RendererFixture fx;

    constexpr int kWidth = 256;
    constexpr int kHeight = 128;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x0E0000u};
    bucket.drawable_fingerprints = {0x0701u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                                             static_cast<float>(kHeight) * 0.5f,
                                             0.0f},
                                            static_cast<float>(std::max(kWidth, kHeight))}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f},
                    {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "fullsurface", 0, 0},
    };

    RectCommand fill{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(kWidth),
        .max_y = static_cast<float>(kHeight),
        .color = {0.4f, 0.4f, 0.4f, 1.0f},
    };
    encode_rect_command(fill, bucket);

    auto scenePath = create_scene(fx, "scene_clear_color_damage", bucket);
    auto rendererPath = create_renderer(fx, "renderer_clear_color_damage");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_clear_color_damage", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 64,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};
    settings.time.frame_index = 1;

    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(first);

    settings.time.frame_index = 2;
    settings.clear_color = {0.6f, 0.2f, 0.2f, 1.0f};

    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto damageRects = fx.space.read<uint64_t>(metricsBase + "/damageRectangles");
    REQUIRE(damageRects);
    auto repaintTiles = fx.space.read<std::vector<Builders::DirtyRectHint>>(metricsBase + "/damageTiles");
    REQUIRE(repaintTiles);
    CHECK(repaintTiles->size() == *damageRects);

    auto damageCoverage = fx.space.read<double>(metricsBase + "/damageCoverageRatio");
    REQUIRE(damageCoverage);
    CHECK(*damageCoverage == doctest::Approx(1.0));

    auto fingerprintChanges = fx.space.read<uint64_t>(metricsBase + "/fingerprintChanges");
    REQUIRE(fingerprintChanges);
    CHECK(*fingerprintChanges == 1);

    auto fingerprintMatches = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesExact");
    REQUIRE(fingerprintMatches);
    CHECK(*fingerprintMatches == 0);

    auto fingerprintRemoved = fx.space.read<uint64_t>(metricsBase + "/fingerprintRemoved");
    REQUIRE(fingerprintRemoved);
    CHECK(*fingerprintRemoved == 1);

    auto fingerprintNew = fx.space.read<uint64_t>(metricsBase + "/fingerprintNew");
    REQUIRE(fingerprintNew);
    CHECK(*fingerprintNew == 0);
}

TEST_CASE("damage metrics respect dirty rect hints") {
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RendererFixture fx;

    constexpr int kWidth = 512;
    constexpr int kHeight = 512;
    constexpr int kTile = 64;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x0F0000u};
    bucket.drawable_fingerprints = {0x0801u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {
        BoundingSphere{{static_cast<float>(kWidth) * 0.5f,
                        static_cast<float>(kHeight) * 0.5f,
                        0.0f},
                       static_cast<float>(std::max(kWidth, kHeight))},
    };
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f},
                    {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "background", 0, 0},
    };

    RectCommand fill{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = static_cast<float>(kWidth),
        .max_y = static_cast<float>(kHeight),
        .color = {0.25f, 0.25f, 0.25f, 1.0f},
    };
    encode_rect_command(fill, bucket);

    auto scenePath = create_scene(fx, "scene_dirty_rect_hints", bucket);
    auto rendererPath = create_renderer(fx, "renderer_dirty_rect_hints");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_dirty_rect_hints", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = kTile,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    settings.time.frame_index = 1;
    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(first);

    Builders::DirtyRectHint hint{};
    hint.min_x = 64.0f;
    hint.min_y = 64.0f;
    hint.max_x = 128.0f;
    hint.max_y = 128.0f;
    std::vector<Builders::DirtyRectHint> hints{hint};
    auto hintsPath = std::string(targetPath.getPath()) + "/hints/dirtyRects";
    auto insert = fx.space.insert(hintsPath, hints);
    REQUIRE(insert.errors.empty());

    settings.time.frame_index = 2;
    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto coverage = fx.space.read<double>(metricsBase + "/damageCoverageRatio");
    REQUIRE(coverage);
    double expectedCoverage = (static_cast<double>(kTile) * static_cast<double>(kTile))
                              / (static_cast<double>(kWidth) * static_cast<double>(kHeight));
    CHECK(*coverage == doctest::Approx(expectedCoverage).epsilon(0.1));

    auto rects = fx.space.read<uint64_t>(metricsBase + "/damageRectangles");
    REQUIRE(rects);
    auto hintTiles = fx.space.read<std::vector<Builders::DirtyRectHint>>(metricsBase + "/damageTiles");
    REQUIRE(hintTiles);
    CHECK(hintTiles->size() == *rects);

    auto tilesTotal = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesTotal");
    REQUIRE(tilesTotal);
    CHECK(*tilesTotal == 64);

    auto tilesDirty = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesDirty");
    REQUIRE(tilesDirty);
    CHECK(*tilesDirty == 1);

    auto tilesSkipped = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesSkipped");
    REQUIRE(tilesSkipped);
    CHECK(*tilesSkipped == *tilesTotal - *tilesDirty);

    auto fingerprintExact = fx.space.read<uint64_t>(metricsBase + "/fingerprintMatchesExact");
    REQUIRE(fingerprintExact);
    CHECK(*fingerprintExact == 1);

    auto fingerprintChanged = fx.space.read<uint64_t>(metricsBase + "/fingerprintChanges");
    REQUIRE(fingerprintChanged);
    CHECK(*fingerprintChanged == 0);
}

TEST_CASE("progressive repaint keeps backdrop when dirty hints cover a tile") {
    RendererFixture fx;

    constexpr int kWidth = 128;
    constexpr int kHeight = 128;
    constexpr int kTile = 64;
    constexpr std::uint64_t kBackgroundFingerprint = 0x20000011ull;
    constexpr std::uint64_t kStrokeFingerprint = 0x20000022ull;

    auto make_png_rgba = [](int width, int height, std::array<std::uint8_t, 4> rgba) {
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i + 0] = rgba[0];
            pixels[i + 1] = rgba[1];
            pixels[i + 2] = rgba[2];
            pixels[i + 3] = rgba[3];
        }
        int out_len = 0;
        unsigned char* encoded = stbi_write_png_to_mem(pixels.data(), width * 4, width, height, 4, &out_len);
        REQUIRE(encoded != nullptr);
        std::vector<std::uint8_t> png(encoded, encoded + out_len);
        STBIW_FREE(encoded);
        return png;
    };

    auto make_background_bucket = [&]() {
        DrawableBucketSnapshot bucket{};
        bucket.drawable_ids = {0x20000001ull};
        bucket.drawable_fingerprints = {kBackgroundFingerprint};
        bucket.world_transforms = {identity_transform()};

        BoundingBox canvas_box{{0.0f, 0.0f, 0.0f},
                               {static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f}};
        bucket.bounds_boxes = {canvas_box};
        bucket.bounds_box_valid = {1};

        auto half_width = static_cast<float>(kWidth) * 0.5f;
        auto half_height = static_cast<float>(kHeight) * 0.5f;
        BoundingSphere canvas_sphere{{half_width, half_height, 0.0f},
                                     std::hypot(half_width, half_height)};
        bucket.bounds_spheres = {canvas_sphere};

        bucket.layers = {0};
        bucket.z_values = {0.0f};
        bucket.material_ids = {0x01};
        bucket.pipeline_flags = {0};
        bucket.visibility = {1};
        bucket.command_offsets = {0};
        bucket.command_counts = {1};
        bucket.opaque_indices = {0};
        bucket.alpha_indices = {};
        bucket.clip_head_indices = {-1};
        bucket.authoring_map = {
            DrawableAuthoringMapEntry{bucket.drawable_ids[0], "background", 0, 0},
        };

        ImageCommand image{};
        image.min_x = 0.0f;
        image.min_y = 0.0f;
        image.max_x = static_cast<float>(kWidth);
        image.max_y = static_cast<float>(kHeight);
        image.uv_min_x = 0.0f;
        image.uv_min_y = 0.0f;
        image.uv_max_x = 1.0f;
        image.uv_max_y = 1.0f;
        image.image_fingerprint = kBackgroundFingerprint;
        image.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        encode_image_command(image, bucket);

        return bucket;
    };

    auto make_overlay_bucket = [&]() {
        auto bucket = make_background_bucket();

        bucket.drawable_ids.push_back(0x20000002ull);
        bucket.drawable_fingerprints.push_back(kStrokeFingerprint);
        bucket.world_transforms.push_back(identity_transform());

        BoundingBox stroke_box{{16.0f, 16.0f, 0.0f}, {32.0f, 32.0f, 0.0f}};
        bucket.bounds_boxes.push_back(stroke_box);
        bucket.bounds_box_valid.push_back(1);

        auto stroke_half_width = (stroke_box.max[0] - stroke_box.min[0]) * 0.5f;
        auto stroke_half_height = (stroke_box.max[1] - stroke_box.min[1]) * 0.5f;
        BoundingSphere stroke_sphere{{stroke_box.min[0] + stroke_half_width,
                                      stroke_box.min[1] + stroke_half_height,
                                      0.0f},
                                     std::hypot(stroke_half_width, stroke_half_height)};
        bucket.bounds_spheres.push_back(stroke_sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(1.0f);
        bucket.material_ids.push_back(0x02);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));

        RectCommand stroke{
            .min_x = stroke_box.min[0],
            .min_y = stroke_box.min[1],
            .max_x = stroke_box.max[0],
            .max_y = stroke_box.max[1],
            .color = {1.0f, 0.0f, 0.0f, 1.0f},
        };
        encode_rect_command(stroke, bucket);

        bucket.command_counts.push_back(1);
        bucket.opaque_indices.push_back(1);
        bucket.clip_head_indices.push_back(-1);
        bucket.authoring_map.push_back(
            DrawableAuthoringMapEntry{bucket.drawable_ids.back(), "stroke", 0, 0});

        return bucket;
    };

    SceneParams sceneParams{
        .name = "scene_progressive_dirty_tile",
        .description = "Progressive hint repro",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    auto base_bucket = make_background_bucket();
    auto revision1 = fx.publish_snapshot(*scene, base_bucket);
    REQUIRE(revision1 >= 1);

    auto png_bytes = make_png_rgba(1, 1, {255, 255, 255, 255});
    auto store_png_for_revision = [&](std::uint64_t revision) {
        auto revision_base = std::string(scene->getPath()) + "/builds/" + format_revision(revision);
        auto image_path = revision_base + "/assets/images/" + fingerprint_hex(kBackgroundFingerprint) + ".png";
        auto write = fx.space.insert(image_path, png_bytes);
        REQUIRE(write.errors.empty());
        auto canonical_path = std::string(scene->getPath()) + "/assets/images/" + fingerprint_hex(kBackgroundFingerprint) + ".png";
        auto canonical_write = fx.space.insert(canonical_path, png_bytes);
        REQUIRE(canonical_write.errors.empty());
    };
    store_png_for_revision(revision1);

    auto rendererPath = create_renderer(fx, "renderer_progressive_dirty_tile");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = kWidth;
    surfaceDesc.size_px.height = kHeight;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;
    surfaceDesc.progressive_tile_size_px = kTile;

    auto surfacePath = create_surface(fx, "surface_progressive_dirty_tile", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, *scene));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{};
    opts.enable_progressive = true;
    opts.enable_buffered = false;
    opts.progressive_tile_size_px = kTile;
    PathSurfaceSoftware surface{surfaceDesc, opts};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

    {
        PathRenderer2D renderer_first{fx.space};
        settings.time.frame_index = 1;
        auto first = renderer_first.render({
            .target_path = SP::ConcretePathStringView{targetPath.getPath()},
            .settings = settings,
            .surface = surface,
            .backend_kind = RendererKind::Software2D,
        });
        REQUIRE(first);
    }

    auto revision2 = fx.publish_snapshot(*scene, make_overlay_bucket());
    REQUIRE(revision2 > revision1);

    Builders::DirtyRectHint hint{};
    hint.min_x = 0.0f;
    hint.min_y = 0.0f;
    hint.max_x = static_cast<float>(kTile);
    hint.max_y = static_cast<float>(kTile);
    std::array hints{hint};
    auto submitted = Renderer::SubmitDirtyRects(fx.space,
                                                SP::ConcretePathStringView{targetPath.getPath()},
                                                std::span<const Builders::DirtyRectHint>{hints});
    REQUIRE(submitted);

    PathRenderer2D renderer_second{fx.space};
    settings.time.frame_index = 2;
    auto second = renderer_second.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(second);

    auto& progressive = surface.progressive_buffer();
    std::vector<std::uint8_t> tile_bytes(static_cast<std::size_t>(kTile) * static_cast<std::size_t>(kTile) * 4u);
    auto tile = progressive.copy_tile(0, tile_bytes);
    REQUIRE(tile.has_value());

    auto pixel_at = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        auto index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(kTile)
                      + static_cast<std::size_t>(x))
                     * 4u;
        return {tile_bytes[index + 0], tile_bytes[index + 1], tile_bytes[index + 2], tile_bytes[index + 3]};
    };

    auto outside = pixel_at(0, 0);
    CHECK(outside[0] == 255);
    CHECK(outside[1] == 255);
    CHECK(outside[2] == 255);

    auto inside = pixel_at(24, 24);
    CHECK(inside[0] == 255);
    CHECK(inside[1] == 0);
    CHECK(inside[2] == 0);
}

TEST_CASE("pipeline flags partition passes when snapshot lacks explicit indices") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x100001u, 0x100002u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 3.0f},
                             BoundingSphere{{1.0f, 1.0f, 0.0f}, 3.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
        BoundingBox{{0.5f, 0.5f, 0.0f}, {2.5f, 2.5f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 1.0f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0u, AlphaBlend};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/opaque", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node/alpha", 0, 0},
    };

    RectCommand opaque_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .color = {0.6f, 0.4f, 0.2f, 1.0f},
    };
    encode_rect_command(opaque_rect, bucket);

    RectCommand alpha_rect{
        .min_x = 0.5f,
        .min_y = 0.5f,
        .max_x = 2.5f,
        .max_y = 2.5f,
        .color = {0.2f, 0.6f, 0.8f, 0.5f},
    };
    encode_rect_command(alpha_rect, bucket);

    auto scenePath = create_scene(fx, "scene_pipeline_flags", bucket);
    auto rendererPath = create_renderer(fx, "renderer_pipeline_flags");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 3;
    surfaceDesc.size_px.height = 3;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_pipeline_flags", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 11;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaDrawables").value() == 1);
CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 2);
CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
}

TEST_CASE("incremental damage tracking limits progressive tiles") {
    RendererFixture fx;

    auto make_bucket = [&](float origin_x, float origin_y) {
        DrawableBucketSnapshot bucket{};
        bucket.drawable_ids = {0xABCDEF01u};
        bucket.world_transforms = {identity_transform()};
        bucket.bounds_boxes = {
            BoundingBox{{origin_x, origin_y, 0.0f},
                        {origin_x + 2.0f, origin_y + 2.0f, 0.0f}},
        };
        bucket.bounds_box_valid = {1};
        bucket.bounds_spheres = {
            BoundingSphere{{origin_x + 1.0f, origin_y + 1.0f, 0.0f}, 1.5f},
        };
        bucket.layers = {0};
        bucket.z_values = {0.0f};
        bucket.material_ids = {0};
        bucket.pipeline_flags = {0};
        bucket.visibility = {1};
        bucket.command_offsets = {0};
        bucket.command_counts = {1};
        bucket.opaque_indices = {0};
        bucket.alpha_indices = {};
        bucket.clip_head_indices = {-1};
        bucket.authoring_map = {
            DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0},
        };

        RectCommand rect{
            .min_x = origin_x,
            .min_y = origin_y,
            .max_x = origin_x + 2.0f,
            .max_y = origin_y + 2.0f,
            .color = {0.8f, 0.1f, 0.0f, 1.0f},
        };
        encode_rect_command(rect, bucket);

        return bucket;
    };

    auto bucket_initial = make_bucket(0.0f, 0.0f);
    auto scenePath = create_scene(fx, "scene_damage_tiles", bucket_initial);
    auto rendererPath = create_renderer(fx, "renderer_damage_tiles");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 8;
    surfaceDesc.size_px.height = 8;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_damage_tiles", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.05f, 0.05f, 0.05f, 1.0f};
    settings.time.frame_index = 1;

    auto first_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(first_result);
    (void)surface.consume_progressive_dirty_tiles();

    auto bucket_moved = make_bucket(6.0f, 6.0f);
    fx.publish_snapshot(scenePath, std::move(bucket_moved));

    settings.time.frame_index = 2;
    auto second_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(second_result);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    std::sort(dirty_tiles.begin(), dirty_tiles.end());
    std::vector<std::size_t> expected_tiles{0};
    CHECK(dirty_tiles == expected_tiles);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto updated = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesUpdated");
    REQUIRE(updated);
    CHECK(*updated == expected_tiles.size());
}

TEST_CASE("renderer skips repaint when drawable id changes but content matches") {
    RendererFixture fx;

    auto make_bucket = [&](std::uint64_t drawable_id) {
        DrawableBucketSnapshot bucket{};
        bucket.drawable_ids = {drawable_id};
        bucket.world_transforms = {identity_transform()};
        bucket.bounds_boxes = {
            BoundingBox{{1.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
        };
        bucket.bounds_box_valid = {1};
        bucket.bounds_spheres = {
            BoundingSphere{{2.0f, 2.0f, 0.0f}, 1.5f},
        };
        bucket.layers = {0};
        bucket.z_values = {0.0f};
        bucket.material_ids = {0};
        bucket.pipeline_flags = {0};
        bucket.visibility = {1};
        bucket.command_offsets = {0};
        bucket.command_counts = {1};
        bucket.opaque_indices = {0};
        bucket.alpha_indices = {};
        bucket.clip_head_indices = {-1};
        bucket.authoring_map = {
            DrawableAuthoringMapEntry{drawable_id, "node", 0, 0},
        };

        RectCommand rect{
            .min_x = 1.0f,
            .min_y = 1.0f,
            .max_x = 3.0f,
            .max_y = 3.0f,
            .color = {0.4f, 0.2f, 0.9f, 1.0f},
        };
        encode_rect_command(rect, bucket);
        return bucket;
    };

    auto bucket_initial = make_bucket(0x1001u);
    auto scenePath = create_scene(fx, "scene_id_change", bucket_initial);
    auto rendererPath = create_renderer(fx, "renderer_id_change");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 8;
    surfaceDesc.size_px.height = 8;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_id_change", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(first_result);
    (void)surface.consume_progressive_dirty_tiles();

    auto bucket_same_shape = make_bucket(0x2002u);
    fx.publish_snapshot(scenePath, std::move(bucket_same_shape));

    settings.time.frame_index = 2;
    auto second_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(second_result);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    CHECK(dirty_tiles.empty());

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto updated = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesUpdated");
    REQUIRE(updated);
    CHECK(*updated == 0);
}

TEST_CASE("dirty rect hints trigger repaint for unchanged snapshot") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x42u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.bounds_spheres = {
        BoundingSphere{{2.0f, 2.0f, 0.0f}, 3.0f},
    };
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0},
    };

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {0.4f, 0.2f, 0.9f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_hint_damage", bucket);
    auto rendererPath = create_renderer(fx, "renderer_hint_damage");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 8;
    surfaceDesc.size_px.height = 8;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_hint_damage", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(first);
    (void)surface.consume_progressive_dirty_tiles();

    std::vector<DirtyRectHint> hint_rects{
        DirtyRectHint{1.0f, 1.0f, 3.0f, 3.0f},
    };
    REQUIRE(Renderer::SubmitDirtyRects(fx.space,
                                       SP::ConcretePathStringView{targetPath.getPath()},
                                       std::span<const DirtyRectHint>{hint_rects}));

    settings.time.frame_index = 2;
    auto second = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(second);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    std::sort(dirty_tiles.begin(), dirty_tiles.end());
    std::vector<std::size_t> expected_tiles{0};
    CHECK(dirty_tiles == expected_tiles);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto updated = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesUpdated");
    REQUIRE(updated);
    CHECK(*updated == expected_tiles.size());
}

TEST_CASE("clear color change triggers full-surface repaint") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xDEADBEEF};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_boxes = {
        BoundingBox{{1.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.bounds_spheres = {
        BoundingSphere{{2.0f, 2.0f, 0.0f}, 1.5f},
    };
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0},
    };

    RectCommand rect{
        .min_x = 1.0f,
        .min_y = 1.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .color = {0.4f, 0.4f, 0.9f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_clear_color_damage", bucket);
    auto rendererPath = create_renderer(fx, "renderer_clear_color_damage");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 8;
    surfaceDesc.size_px.height = 8;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_clear_color_damage", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options opts{
        .enable_progressive = true,
        .enable_buffered = false,
        .progressive_tile_size_px = 2,
    };
    PathSurfaceSoftware surface{surfaceDesc, opts};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    settings.time.frame_index = 1;

    auto first_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(first_result);
    (void)surface.consume_progressive_dirty_tiles();

    settings.clear_color = {0.2f, 0.3f, 0.4f, 1.0f};
    settings.time.frame_index = 2;
    auto second_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(second_result);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    std::sort(dirty_tiles.begin(), dirty_tiles.end());
    auto tile_count = surface.progressive_tile_count();
    std::vector<std::size_t> expected(tile_count);
    std::iota(expected.begin(), expected.end(), 0u);
    CHECK(dirty_tiles == expected);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto updated = fx.space.read<uint64_t>(metricsBase + "/progressiveTilesUpdated");
    REQUIRE(updated);
    CHECK(*updated == tile_count);
}

TEST_CASE("records opaque sort violations when indices are unsorted") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x200001u, 0x200002u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f},
                             BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}},
        BoundingBox{{0.5f, 0.5f, 0.0f}, {2.5f, 2.5f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.1f, 0.2f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {1, 0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/opaque0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node/opaque1", 0, 0},
    };

    RectCommand rect0{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.5f, 0.1f, 0.1f, 1.0f},
    };
    RectCommand rect1{
        .min_x = 0.5f,
        .min_y = 0.5f,
        .max_x = 2.5f,
        .max_y = 2.5f,
        .color = {0.1f, 0.5f, 0.1f, 1.0f},
    };
    encode_rect_command(rect0, bucket);
    encode_rect_command(rect1, bucket);

    auto scenePath = create_scene(fx, "scene_opaque_sort_violation", bucket);
    auto rendererPath = create_renderer(fx, "renderer_opaque_sort_violation");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_opaque_sort_violation", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueSortViolations").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() == 0);
}

TEST_CASE("records alpha sort violations when depth is front-to-back") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x300001u, 0x300002u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f},
                             BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}},
        BoundingBox{{0.5f, 0.5f, 0.0f}, {2.5f, 2.5f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.5f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {AlphaBlend, AlphaBlend};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0, 1};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/alpha0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node/alpha1", 0, 0},
    };

    RectCommand rect0{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.2f, 0.2f, 0.8f, 0.5f},
    };
    RectCommand rect1{
        .min_x = 0.5f,
        .min_y = 0.5f,
        .max_x = 2.5f,
        .max_y = 2.5f,
        .color = {0.8f, 0.2f, 0.2f, 0.5f},
    };
    encode_rect_command(rect0, bucket);
    encode_rect_command(rect1, bucket);

    auto scenePath = create_scene(fx, "scene_alpha_sort_violation", bucket);
    auto rendererPath = create_renderer(fx, "renderer_alpha_sort_violation");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_alpha_sort_violation", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() >= 1);
}

TEST_CASE("renders png image command") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x900001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/image", 0, 0},
    };

    ImageCommand image{};
    image.min_x = 0.0f;
    image.min_y = 0.0f;
    image.max_x = 2.0f;
    image.max_y = 2.0f;
    image.uv_min_x = 0.0f;
    image.uv_min_y = 0.0f;
    image.uv_max_x = 1.0f;
    image.uv_max_y = 1.0f;
    image.image_fingerprint = 0xABCDEF0102030405ull;
    image.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    encode_image_command(image, bucket);

    SceneParams sceneParams{
        .name = "scene_image",
        .description = "Image test",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);
    auto revision = fx.publish_snapshot(*scene, bucket);

    auto rendererPath = create_renderer(fx, "renderer_image");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_image", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, *scene));

    auto revision_base = std::string(scene->getPath()) + "/builds/" + format_revision(revision);
    auto image_path = revision_base + "/assets/images/" + fingerprint_hex(image.image_fingerprint) + ".png";
    std::vector<std::uint8_t> png_bytes(kTestPngRgba.begin(), kTestPngRgba.end());
    auto insert_result = fx.space.insert(image_path, png_bytes);
    REQUIRE(insert_result.errors.empty());

    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 42;

    auto renderStats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(renderStats);
    CHECK(renderStats->drawable_count == 1);
    auto const surface_bytes = surface.resident_cpu_bytes();
    constexpr std::size_t kImageCacheBytes = 2u * 2u * 4u * sizeof(float);
    auto const expected_cpu_bytes = static_cast<std::uint64_t>(surface_bytes + kImageCacheBytes);
    CHECK(renderStats->resource_cpu_bytes == expected_cpu_bytes);
    CHECK(renderStats->resource_gpu_bytes == 16);
    REQUIRE(renderStats->resource_residency.size() == 1);
    CHECK(renderStats->resource_residency.front().fingerprint == image.image_fingerprint);
    CHECK(renderStats->resource_residency.front().gpu_bytes == 16);
    CHECK(renderStats->resource_residency.front().cpu_bytes == kImageCacheBytes);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
    auto resource_count = fx.space.read<uint64_t>(metricsBase + "/materialResourceCount");
    REQUIRE(resource_count);
    CHECK(*resource_count >= 1);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + static_cast<std::size_t>(4); // pixel (1,1)
    CHECK(buffer[center_offset + 3] > 0); // non-zero alpha
    auto color_sum = static_cast<int>(buffer[center_offset + 0])
                   + static_cast<int>(buffer[center_offset + 1])
                   + static_cast<int>(buffer[center_offset + 2]);
    CHECK(color_sum > 0);
}

TEST_CASE("render executes text glyphs command") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x300001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/text", 0, 0},
    };

    UIScene::TextGlyphsCommand glyphs{};
    glyphs.min_x = 0.0f;
    glyphs.min_y = 0.0f;
    glyphs.max_x = 2.0f;
    glyphs.max_y = 2.0f;
    glyphs.glyph_count = 4;
    glyphs.color = {0.2f, 0.6f, 1.0f, 0.75f};
    encode_text_glyphs_command(glyphs, bucket);

    auto scenePath = create_scene(fx, "scene_text", bucket);
    auto rendererPath = create_renderer(fx, "renderer_text");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_text", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.time.frame_index = 3;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);
    CHECK(stats->drawable_count == 1);
    CHECK(stats->text_command_count == 1);
    CHECK(stats->text_fallback_count == 0);
    CHECK(stats->text_pipeline == PathRenderer2D::TextPipeline::GlyphQuads);
    CHECK(stats->text_fallback_allowed);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + 4;
    CHECK(buffer[center_offset + 3] > 0);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/textCommandCount").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/textFallbackCount").value() == 0);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/textPipeline").value() == "GlyphQuads");
    CHECK(fx.space.read<bool>(metricsBase + "/textFallbackAllowed").value());
}

TEST_CASE("text fallback engages when shaped pipeline requested") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x300101u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/text", 0, 0},
    };

    UIScene::TextGlyphsCommand glyphs{};
    glyphs.min_x = 0.0f;
    glyphs.min_y = 0.0f;
    glyphs.max_x = 2.0f;
    glyphs.max_y = 2.0f;
    glyphs.glyph_count = 6;
    glyphs.color = {0.5f, 0.3f, 0.9f, 1.0f};
    encode_text_glyphs_command(glyphs, bucket);

    auto scenePath = create_scene(fx, "scene_text_fallback", bucket);
    auto rendererPath = create_renderer(fx, "renderer_text_fallback");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_text_fallback", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.debug.enabled = true;
    settings.debug.flags |= RenderSettings::Debug::kForceShapedText;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);
    CHECK(stats->drawable_count == 1);
    CHECK(stats->text_command_count == 1);
    CHECK(stats->text_fallback_count == 1);
    CHECK(stats->text_pipeline == PathRenderer2D::TextPipeline::Shaped);
    CHECK(stats->text_fallback_allowed);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + 4;
    CHECK(buffer[center_offset + 3] > 0);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/textCommandCount").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/textFallbackCount").value() == 1);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/textPipeline").value() == "Shaped");
    CHECK(fx.space.read<bool>(metricsBase + "/textFallbackAllowed").value());
}

TEST_CASE("render executes path command using fill color") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x400001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/path", 0, 0},
    };

    UIScene::PathCommand path{};
    path.min_x = 0.0f;
    path.min_y = 0.0f;
    path.max_x = 2.0f;
    path.max_y = 2.0f;
    path.fill_color = {0.8f, 0.3f, 0.1f, 1.0f};
    encode_path_command(path, bucket);

    auto scenePath = create_scene(fx, "scene_path", bucket);
    auto rendererPath = create_renderer(fx, "renderer_path");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_path", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.time.frame_index = 4;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + 4;
    CHECK(buffer[center_offset + 3] > 0);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
}

TEST_CASE("render executes mesh command using drawable bounds") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x500001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/mesh", 0, 0},
    };

    UIScene::MeshCommand mesh{};
    mesh.color = {0.1f, 0.8f, 0.2f, 1.0f};
    encode_mesh_command(mesh, bucket);

    auto scenePath = create_scene(fx, "scene_mesh", bucket);
    auto rendererPath = create_renderer(fx, "renderer_mesh");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_mesh", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.time.frame_index = 5;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + 4;
    CHECK(buffer[center_offset + 3] > 0);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
}

TEST_CASE("render executes stroke command draws polyline") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x600001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{4.0f, 4.0f, 0.0f}, 6.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {8.0f, 8.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices.clear();
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/stroke", 0, 0},
    };
    bucket.stroke_points = {
        UIScene::StrokePoint{1.0f, 1.0f},
        UIScene::StrokePoint{6.0f, 6.0f},
    };

    UIScene::StrokeCommand stroke{};
    stroke.min_x = 0.0f;
    stroke.min_y = 0.0f;
    stroke.max_x = 7.0f;
    stroke.max_y = 7.0f;
    stroke.thickness = 2.0f;
    stroke.point_offset = 0;
    stroke.point_count = static_cast<std::uint32_t>(bucket.stroke_points.size());
    stroke.color = {1.0f, 0.0f, 0.0f, 1.0f};
    encode_stroke_command(stroke, bucket);

    auto scenePath = create_scene(fx, "scene_stroke", bucket);
    auto rendererPath = create_renderer(fx, "renderer_stroke");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 8;
    surfaceDesc.size_px.height = 8;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_stroke", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 3;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(stats);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto pixel_offset = static_cast<std::size_t>(3) * stride + static_cast<std::size_t>(3) * 4u;
    CHECK(buffer[pixel_offset] > 0);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
}

TEST_CASE("render tracks culled drawables and executed commands") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x010000u, 0x020000u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{2.0f, 2.0f, 0.0f}, 3.0f},
                             BoundingSphere{{10.0f, 10.0f, 0.0f}, 1.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}},
        BoundingBox{{10.0f, 10.0f, 0.0f}, {12.0f, 12.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.0f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0, 1};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node1", 0, 0},
    };

    RectCommand first_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.0f, 0.0f, 1.0f, 1.0f},
    };
    encode_rect_command(first_rect, bucket);

    RectCommand offscreen_rect{
        .min_x = 10.0f,
        .min_y = 10.0f,
        .max_x = 12.0f,
        .max_y = 12.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    encode_rect_command(offscreen_rect, bucket);

    auto scenePath = create_scene(fx, "scene_cull", bucket);
    auto rendererPath = create_renderer(fx, "renderer_cull");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_cull", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 1);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/culledDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
}

TEST_CASE("unsupported commands fall back to bounds and report metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x777777u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/mesh", 0, 0}};

    bucket.command_kinds.push_back(999u); // invalid kind to trigger fallback

    auto scenePath = create_scene(fx, "scene_mesh_fallback", bucket);
    auto rendererPath = create_renderer(fx, "renderer_mesh_fallback");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_mesh_fallback", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 1);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/culledDrawables").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 1);
}

TEST_CASE("render reports error when target scene binding missing") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {};
    bucket.world_transforms = {};

    auto scenePath = create_scene(fx, "scene_error", bucket);
    auto rendererPath = create_renderer(fx, "renderer_error");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_error", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    // Remove binding to induce error.
    (void)fx.space.take<std::string>(std::string(targetPath.getPath()) + "/scene");

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    CHECK_FALSE(result);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "target missing scene binding");
}

TEST_CASE("Surface::RenderOnce drives renderer and stores metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xABCDu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.4f, 0.4f, 0.4f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_surface", bucket);
    auto rendererPath = create_renderer(fx, "renderer_surface");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_main", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto renderFuture = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(renderFuture);
    CHECK(renderFuture->ready());

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);

    auto storedSettings = Renderer::ReadSettings(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->time.frame_index == 1);

    auto second = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(second);
    CHECK(second->ready());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 2);
}

TEST_CASE("PathRenderer2D records material descriptors metrics") {
    RendererFixture fx;

    RectDrawableDef def{};
    def.id = 0x1111u;
    def.fingerprint = 0x2222u;
    def.rect = RectCommand{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {0.25f, 0.5f, 0.75f, 1.0f},
    };

    auto bucket = make_rect_bucket({def});
    REQUIRE(bucket.material_ids.size() == 1);
    bucket.material_ids[0] = 42;

    auto scenePath = create_scene(fx, "scene_material_metrics", bucket);
    auto rendererPath = create_renderer(fx, "renderer_material_metrics");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_material_metrics", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto renderFuture = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(renderFuture);
    CHECK(renderFuture->ready());

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";

    auto materialCount = fx.space.read<uint64_t>(metricsBase + "/materialCount");
    REQUIRE(materialCount);
    CHECK(*materialCount == 1);

    auto materialDescriptors = fx.space.read<std::vector<MaterialDescriptor>>(metricsBase + "/materialDescriptors");
    REQUIRE(materialDescriptors);
    REQUIRE(materialDescriptors->size() == 1);
    auto const& descriptor = materialDescriptors->front();
    CHECK(descriptor.material_id == 42);
    CHECK(descriptor.pipeline_flags == 0);
    CHECK(descriptor.primary_draw_kind == static_cast<std::uint32_t>(DrawCommandKind::Rect));
    CHECK(descriptor.drawable_count == 1);
    CHECK(descriptor.command_count >= 1);
    CHECK(descriptor.uses_image == false);
    CHECK(descriptor.resource_fingerprint == 0);
    CHECK(descriptor.tint_rgba[0] == doctest::Approx(1.0f));
    CHECK(descriptor.tint_rgba[1] == doctest::Approx(1.0f));
    CHECK(descriptor.tint_rgba[2] == doctest::Approx(1.0f));
    CHECK(descriptor.tint_rgba[3] == doctest::Approx(1.0f));
    CHECK(descriptor.color_rgba[0] == doctest::Approx(0.25f));
    CHECK(descriptor.color_rgba[1] == doctest::Approx(0.5f));
    CHECK(descriptor.color_rgba[2] == doctest::Approx(0.75f));
    CHECK(descriptor.color_rgba[3] == doctest::Approx(1.0f));
}

TEST_CASE("PathRenderer2D pulses focus highlight color over time") {
    RendererFixture fx;

    RectDrawableDef highlight{};
    highlight.id = 0xF0C0F001u;
    highlight.fingerprint = 0xAA55AA55u;
    highlight.rect = RectCommand{
        .min_x = 8.0f,
        .min_y = 8.0f,
        .max_x = 56.0f,
        .max_y = 12.0f,
        .color = {0.05f, 0.80f, 0.95f, 1.0f},
    };

    auto bucket = make_rect_bucket({highlight});
    REQUIRE(bucket.pipeline_flags.size() == 1);
    bucket.pipeline_flags[0] |= SP::UI::PipelineFlags::HighlightPulse;
    REQUIRE(bucket.authoring_map.size() == 1);
    bucket.authoring_map[0].authoring_node_id = "widget/focus/highlight";

    auto scenePath = create_scene(fx, "scene_focus_pulse", bucket);
    auto rendererPath = create_renderer(fx, "renderer_focus_pulse");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 64;
    surfaceDesc.size_px.height = 32;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_focus_pulse", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    auto render_frame = [&](double time_ms, float clear_r, std::uint64_t frame_index) {
        RenderSettings settings{};
        settings.surface.size_px.width = surfaceDesc.size_px.width;
        settings.surface.size_px.height = surfaceDesc.size_px.height;
        settings.surface.visibility = true;
        settings.clear_color = {clear_r, 0.0f, 0.0f, 1.0f};
        settings.time.time_ms = time_ms;
        settings.time.delta_ms = 16.0;
        settings.time.frame_index = frame_index;
        auto stats = renderer.render({
            .target_path = SP::ConcretePathStringView{targetPath.getPath()},
            .settings = settings,
            .surface = surface,
        });
        REQUIRE(stats);
        return copy_buffer(surface);
    };

    auto base_buffer = render_frame(0.0, 0.0f, 1);
    auto light_buffer = render_frame(250.0, 0.01f, 2);
    auto dark_buffer = render_frame(750.0, 0.02f, 3);

    auto sample_pixel = [&](std::vector<std::uint8_t> const& buffer) -> std::array<std::uint8_t, 4> {
        std::size_t stride = static_cast<std::size_t>(surfaceDesc.size_px.width) * 4u;
        std::size_t x = 16;
        std::size_t y = 9;
        std::size_t offset = y * stride + x * 4u;
        REQUIRE(offset + 3u < buffer.size());
        return {buffer[offset + 0], buffer[offset + 1], buffer[offset + 2], buffer[offset + 3]};
    };

    auto base_pixel = sample_pixel(base_buffer);
    auto light_pixel = sample_pixel(light_buffer);
    auto dark_pixel = sample_pixel(dark_buffer);

    CHECK(light_pixel[0] > base_pixel[0]);
    CHECK(light_pixel[1] > base_pixel[1]);
    CHECK(dark_pixel[0] < base_pixel[0]);
    CHECK(dark_pixel[1] < base_pixel[1]);
    CHECK(base_pixel[3] == light_pixel[3]);
    CHECK(base_pixel[3] == dark_pixel[3]);
}

TEST_CASE("Surface::RenderOnce handles repeated loop renders") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xCAFE01u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "loop_node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.2f, 0.5f, 0.8f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_surface_loop", bucket);
    auto rendererPath = create_renderer(fx, "renderer_surface_loop");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_loop", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    constexpr int kIterations = 10;
    for (int i = 0; i < kIterations; ++i) {
        auto future = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
        REQUIRE(future);
        CHECK(future->ready());
    }

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == kIterations);

    auto settings = Renderer::ReadSettings(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(settings);
    CHECK(settings->time.frame_index == kIterations);
    CHECK(fx.space.read<double>(metricsBase + "/approxOverdrawFactor").value() >= 1.0);
}

TEST_CASE("Window::Present renders and presents a frame with metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x123456u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_window", bucket);
    auto rendererPath = create_renderer(fx, "renderer_window");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "main_window";
    windowParams.title = "Test";
    windowParams.width = 640;
    windowParams.height = 480;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    auto presentResult = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(presentResult);
    CHECK(presentResult->stats.presented);
    CHECK_FALSE(presentResult->stats.skipped);
    auto height = surfaceDesc.size_px.height;
    REQUIRE(height > 0);
    auto stride = presentResult->framebuffer.size() / static_cast<std::size_t>(height);
    REQUIRE(stride >= static_cast<std::size_t>(surfaceDesc.size_px.width * 4));
    REQUIRE(presentResult->framebuffer.size() == stride * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        auto* row = presentResult->framebuffer.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < surfaceDesc.size_px.width; ++x) {
            auto idx = static_cast<std::size_t>(x) * 4;
            CHECK(row[idx + 0] == 255);
            CHECK(row[idx + 1] == 0);
            CHECK(row[idx + 2] == 0);
            CHECK(row[idx + 3] == 255);
        }
    }

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandCount").value() == 1);
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/lastPresentSkipped").value());
    CHECK(fx.space.read<bool>(metricsBase + "/presented").value());
    CHECK(fx.space.read<bool>(metricsBase + "/bufferedFrameConsumed").value()
          == !presentResult->stats.used_iosurface);
    CHECK(fx.space.read<bool>(metricsBase + "/usedProgressive").value());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRectsCoalesced").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveSkipOddSeq").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRecopyAfterSeqChange").value() == 0);
    CHECK(fx.space.read<double>(metricsBase + "/waitBudgetMs").value() == doctest::Approx(20.0).epsilon(0.1));
    CHECK(fx.space.read<double>(metricsBase + "/presentMs").value() >= 0.0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueSortViolations").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() == 0);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/presentMode").value() == "PreferLatestCompleteWithBudget");
    CHECK(fx.space.read<double, std::string>(metricsBase + "/stalenessBudgetMs").value() == doctest::Approx(8.0));
    CHECK(fx.space.read<double, std::string>(metricsBase + "/frameTimeoutMs").value() == doctest::Approx(20.0));
    CHECK(fx.space.read<uint64_t>(metricsBase + "/maxAgeFrames").value() == 1);
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/stale").value());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/presentedAgeFrames").value() == 0);
    CHECK(fx.space.read<double, std::string>(metricsBase + "/presentedAgeMs").value() == doctest::Approx(0.0));
    CHECK(fx.space.read<bool, std::string>(metricsBase + "/autoRenderOnPresent").value());
    CHECK(fx.space.read<bool, std::string>(metricsBase + "/vsyncAlign").value());
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());

    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    auto storedFramebuffer = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE(storedFramebuffer);
    CHECK(storedFramebuffer->width == surfaceDesc.size_px.width);
    CHECK(storedFramebuffer->height == surfaceDesc.size_px.height);
    CHECK(storedFramebuffer->row_stride_bytes >= static_cast<std::uint32_t>(surfaceDesc.size_px.width * 4));
    CHECK(storedFramebuffer->row_stride_bytes % 16 == 0);
    CHECK(storedFramebuffer->pixel_format == surfaceDesc.pixel_format);
    CHECK(storedFramebuffer->color_space == surfaceDesc.color_space);
    CHECK(storedFramebuffer->premultiplied_alpha == surfaceDesc.premultiplied_alpha);
    CHECK(storedFramebuffer->pixels == presentResult->framebuffer);

    auto diagnosticsFramebuffer = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(diagnosticsFramebuffer);
    CHECK(diagnosticsFramebuffer->pixels == storedFramebuffer->pixels);
}

TEST_CASE("Window::Present skips framebuffer serialization when capture disabled") {
    RendererFixture fx;

    RectDrawableDef def{};
    def.id = 0x10u;
    def.fingerprint = 0x20u;
    def.rect = RectCommand{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };

    auto bucket = make_rect_bucket({def});
    auto scenePath = create_scene(fx, "scene_no_capture", bucket);
    auto rendererPath = create_renderer(fx, "renderer_no_capture");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_no_capture", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_no_capture";
    windowParams.title = "NoCapture";
    windowParams.width = 256;
    windowParams.height = 256;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));

    auto present = Builders::Window::Present(fx.space, *windowPath, "main");
    if (!present) {
        INFO("Window::Present error code = " << static_cast<int>(present.error().code));
        INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
    }
    REQUIRE(present);
    CHECK(present->stats.presented);

    auto targetPath = resolve_target(fx, surfacePath);
    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    auto storedFramebuffer = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE_FALSE(storedFramebuffer);
    auto const code = storedFramebuffer.error().code;
    bool const absent = code == SP::Error::Code::NoObjectFound
        || code == SP::Error::Code::NoSuchPath;
    CHECK(absent);

    if (present->stats.buffered_frame_consumed) {
        CHECK_FALSE(present->framebuffer.empty());
    } else {
        CHECK(present->framebuffer.empty());
    }
}

TEST_CASE("Window::Present software path publishes residency watermarks") {
    RendererFixture fx;

    RectDrawableDef def{};
    def.id = 0xCAFEu;
    def.fingerprint = 0xBEEFu;
    def.rect = RectCommand{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.6f, 0.3f, 0.2f, 1.0f},
    };

    auto bucket = make_rect_bucket({def});
    auto scenePath = create_scene(fx, "scene_software_residency", bucket);
    auto rendererPath = create_renderer(fx, "renderer_software_residency");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_software_residency", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "residency_window";
    windowParams.title = "Residency";
    windowParams.width = 256;
    windowParams.height = 256;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));

    constexpr std::uint64_t kCpuSoftBytes = 1024;
    constexpr std::uint64_t kCpuHardBytes = 4096;
    constexpr std::uint64_t kGpuSoftBytes = 512;
    constexpr std::uint64_t kGpuHardBytes = 1024;

    Builders::RenderSettings overrides{};
    overrides.surface.size_px.width = surfaceDesc.size_px.width;
    overrides.surface.size_px.height = surfaceDesc.size_px.height;
    overrides.surface.dpi_scale = 1.0f;
    overrides.surface.visibility = true;
    overrides.surface.metal = surfaceDesc.metal;
    overrides.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};
    overrides.time.delta_ms = 16.0;
    overrides.time.frame_index = 0;
    overrides.time.time_ms = 0.0;
    overrides.renderer.backend_kind = Builders::RendererKind::Software2D;
    overrides.renderer.metal_uploads_enabled = false;
    overrides.cache.cpu_soft_bytes = kCpuSoftBytes;
    overrides.cache.cpu_hard_bytes = kCpuHardBytes;
    overrides.cache.gpu_soft_bytes = kGpuSoftBytes;
    overrides.cache.gpu_hard_bytes = kGpuHardBytes;

    auto renderFuture = Surface::RenderOnce(fx.space, surfacePath, overrides);
    REQUIRE(renderFuture);
    CHECK(renderFuture->ready());

    auto present = Builders::Window::Present(fx.space, *windowPath, "main");
    if (!present) {
        INFO("Window::Present error code = " << static_cast<int>(present.error().code));
        INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
    }
    REQUIRE(present);
    CHECK(present->stats.presented);
    CHECK_FALSE(present->stats.used_metal_texture);
    CHECK(present->stats.backend_kind == "Software2D");

    auto targetPath = resolve_target(fx, surfacePath);
    auto metrics = Builders::Diagnostics::ReadTargetMetrics(fx.space,
                                                            SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(metrics);
    CHECK(metrics->backend_kind == "Software2D");
    CHECK_FALSE(metrics->used_metal_texture);
    CHECK(metrics->cpu_bytes > 0);
    CHECK(metrics->cpu_soft_bytes == kCpuSoftBytes);
    CHECK(metrics->cpu_hard_bytes == kCpuHardBytes);
    CHECK(metrics->gpu_soft_bytes == kGpuSoftBytes);
    CHECK(metrics->gpu_hard_bytes == kGpuHardBytes);

    auto residencyBase = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";
    auto cpuSoft = fx.space.read<std::uint64_t>(residencyBase + "/cpuSoftBytes");
    REQUIRE(cpuSoft);
    CHECK(*cpuSoft == kCpuSoftBytes);
    auto cpuHard = fx.space.read<std::uint64_t>(residencyBase + "/cpuHardBytes");
    REQUIRE(cpuHard);
    CHECK(*cpuHard == kCpuHardBytes);
    auto gpuSoft = fx.space.read<std::uint64_t>(residencyBase + "/gpuSoftBytes");
    REQUIRE(gpuSoft);
    CHECK(*gpuSoft == kGpuSoftBytes);
    auto gpuHard = fx.space.read<std::uint64_t>(residencyBase + "/gpuHardBytes");
    REQUIRE(gpuHard);
    CHECK(*gpuHard == kGpuHardBytes);

    auto settings = Builders::Renderer::ReadSettings(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(settings);
    CHECK(settings->renderer.backend_kind == Builders::RendererKind::Software2D);
    CHECK_FALSE(settings->renderer.metal_uploads_enabled);
    CHECK(settings->cache.cpu_soft_bytes == kCpuSoftBytes);
    CHECK(settings->cache.cpu_hard_bytes == kCpuHardBytes);
    CHECK(settings->cache.gpu_soft_bytes == kGpuSoftBytes);
    CHECK(settings->cache.gpu_hard_bytes == kGpuHardBytes);
}

TEST_CASE("Window::Present progressive updates preserve prior content") {
    RendererFixture fx;

    RectCommand rect_a{
        .min_x = 10.0f,
        .min_y = 12.0f,
        .max_x = 18.0f,
        .max_y = 20.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    RectCommand rect_b{
        .min_x = 70.0f,
        .min_y = 50.0f,
        .max_x = 78.0f,
        .max_y = 58.0f,
        .color = {0.0f, 0.0f, 1.0f, 1.0f},
    };

    auto scenePath = create_scene(fx,
                                  "scene_window_progressive",
                                  make_rect_bucket({
                                      RectDrawableDef{
                                          .id = 0x100u,
                                          .fingerprint = 0xAAAABBBBCCCCDDDDu,
                                          .rect = rect_a,
                                      },
                                  }));
    auto rendererPath = create_renderer(fx, "renderer_window_progressive");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 128;
    surfaceDesc.size_px.height = 96;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window_progressive", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_progressive";
    windowParams.title = "Progressive Window";
    windowParams.width = surfaceDesc.size_px.width;
    windowParams.height = surfaceDesc.size_px.height;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    auto targetPath = resolve_target(fx, surfacePath);
    auto submit_hint = [&](RectCommand const& rect) {
        DirtyRectHint hint{
            .min_x = rect.min_x,
            .min_y = rect.min_y,
            .max_x = rect.max_x,
            .max_y = rect.max_y,
        };
        REQUIRE(Builders::Renderer::SubmitDirtyRects(fx.space,
                                                     SP::ConcretePathStringView{targetPath.getPath()},
                                                     std::span<const DirtyRectHint>{&hint, 1}));
    };

    auto color_to_bytes = [&](RectCommand const& rect) {
        return encode_linear_to_bytes(make_linear_color(rect.color), surfaceDesc, true);
    };
    auto red_bytes = color_to_bytes(rect_a);
    auto blue_bytes = color_to_bytes(rect_b);

    auto sample_pixel = [](Builders::SoftwareFramebuffer const& fb, int x, int y) {
        auto stride = static_cast<std::size_t>(fb.row_stride_bytes);
        auto offset = stride * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4u;
        return std::array<std::uint8_t, 4>{
            fb.pixels[offset + 0],
            fb.pixels[offset + 1],
            fb.pixels[offset + 2],
            fb.pixels[offset + 3],
        };
    };

    // Frame 1: render rect A
    submit_hint(rect_a);
    auto present_first = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(present_first);
    CHECK(present_first->stats.presented);

    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    auto framebuffer_first = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE(framebuffer_first);

    auto center_a_x = static_cast<int>((rect_a.min_x + rect_a.max_x) * 0.5f);
    auto center_a_y = static_cast<int>((rect_a.min_y + rect_a.max_y) * 0.5f);
    auto pixel_first_a = sample_pixel(*framebuffer_first, center_a_x, center_a_y);
    CHECK(pixel_first_a[0] == red_bytes[0]);
    CHECK(pixel_first_a[1] == red_bytes[1]);
    CHECK(pixel_first_a[2] == red_bytes[2]);
    CHECK(pixel_first_a[3] == red_bytes[3]);

    // Frame 2: add rect B with hints covering only rect B.
    fx.publish_snapshot(scenePath,
                        make_rect_bucket({
                            RectDrawableDef{
                                .id = 0x100u,
                                .fingerprint = 0xAAAABBBBCCCCDDDDu,
                                .rect = rect_a,
                            },
                            RectDrawableDef{
                                .id = 0x200u,
                                .fingerprint = 0x1111222233334444u,
                                .rect = rect_b,
                            },
                        }));
    submit_hint(rect_b);

    auto present_second = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(present_second);
    CHECK(present_second->stats.presented);
    CHECK(present_second->stats.progressive_tiles_copied >= 1);

    auto framebuffer_second = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE(framebuffer_second);

    auto center_b_x = static_cast<int>((rect_b.min_x + rect_b.max_x) * 0.5f);
    auto center_b_y = static_cast<int>((rect_b.min_y + rect_b.max_y) * 0.5f);

    auto pixel_second_a = sample_pixel(*framebuffer_second, center_a_x, center_a_y);
    CHECK(pixel_second_a[0] == red_bytes[0]);
    CHECK(pixel_second_a[1] == red_bytes[1]);
    CHECK(pixel_second_a[2] == red_bytes[2]);
    CHECK(pixel_second_a[3] == red_bytes[3]);

    auto pixel_second_b = sample_pixel(*framebuffer_second, center_b_x, center_b_y);
    CHECK(pixel_second_b[0] == blue_bytes[0]);
    CHECK(pixel_second_b[1] == blue_bytes[1]);
    CHECK(pixel_second_b[2] == blue_bytes[2]);
    CHECK(pixel_second_b[3] == blue_bytes[3]);
}

TEST_CASE("Window::Present handles repeated loop without dropping metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xDEADBEEFu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "window_loop_node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.7f, 0.3f, 0.2f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_window_loop", bucket);
    auto rendererPath = create_renderer(fx, "renderer_window_loop");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window_loop", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_loop";
    windowParams.title = "Loop";
    windowParams.width = 320;
    windowParams.height = 240;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);

    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    constexpr int kIterations = 6;
    for (int i = 0; i < kIterations; ++i) {
        auto present = Builders::Window::Present(fx.space, *windowPath, "main");
        REQUIRE(present);
        CHECK(present->stats.presented);
    }

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == kIterations);
    CHECK(fx.space.read<bool>(metricsBase + "/presented").value());
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/lastPresentSkipped").value());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() >= 1);
    CHECK(fx.space.read<double>(metricsBase + "/renderMs").value() >= 0.0);
    CHECK(fx.space.read<double>(metricsBase + "/presentMs").value() >= 0.0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueSortViolations").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() == 0);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/presentMode").value() == "PreferLatestCompleteWithBudget");
    CHECK_FALSE(fx.space.read<bool, std::string>(metricsBase + "/stale").value());

    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    auto storedFramebuffer = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE(storedFramebuffer);
    auto storedStride = storedFramebuffer->row_stride_bytes;
    CHECK(storedStride >= surfaceDesc.size_px.width * 4);
    CHECK(storedStride % 16 == 0);
    CHECK(storedFramebuffer->pixels.size() == static_cast<std::size_t>(storedStride) * static_cast<std::size_t>(surfaceDesc.size_px.height));
}

TEST_CASE("Window::Present handles multiple renderer targets") {
    RendererFixture fx;

    RectCommand left_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    RectCommand right_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {0.0f, 1.0f, 0.0f, 1.0f},
    };

    auto sceneLeft = create_scene(fx,
                                  "scene_multi_left",
                                  make_rect_bucket({
                                      RectDrawableDef{
                                          .id = 0x10u,
                                          .fingerprint = 0x1111222233334444u,
                                          .rect = left_rect,
                                      },
                                  }));
    auto sceneRight = create_scene(fx,
                                   "scene_multi_right",
                                   make_rect_bucket({
                                       RectDrawableDef{
                                           .id = 0x20u,
                                           .fingerprint = 0x5555666677778888u,
                                           .rect = right_rect,
                                       },
                                   }));
    auto rendererPath = create_renderer(fx, "renderer_multi_target");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfaceLeft = create_surface(fx, "surface_multi_left", surfaceDesc, rendererPath.getPath());
    auto surfaceRight = create_surface(fx, "surface_multi_right", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfaceLeft, sceneLeft));
    REQUIRE(Surface::SetScene(fx.space, surfaceRight, sceneRight));

    Builders::WindowParams windowParams{};
    windowParams.name = "multi_target_window";
    windowParams.title = "MultiTarget";
    windowParams.width = surfaceDesc.size_px.width * 2;
    windowParams.height = surfaceDesc.size_px.height;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "left", surfaceLeft));
    enable_framebuffer_capture(fx.space, *windowPath, "left");
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "right", surfaceRight));
    enable_framebuffer_capture(fx.space, *windowPath, "right");

    auto targetLeft = resolve_target(fx, surfaceLeft);
    auto targetRight = resolve_target(fx, surfaceRight);
    auto metricsBaseLeft = std::string(targetLeft.getPath()) + "/output/v1/common";
    auto metricsBaseRight = std::string(targetRight.getPath()) + "/output/v1/common";

    auto expected_left_bytes = encode_linear_to_bytes(make_linear_color(left_rect.color),
                                                      surfaceDesc,
                                                      true);
    auto expected_right_bytes = encode_linear_to_bytes(make_linear_color(right_rect.color),
                                                       surfaceDesc,
                                                       true);

    auto sample_pixel = [](Builders::SoftwareFramebuffer const& fb, int x, int y) {
        auto stride = static_cast<std::size_t>(fb.row_stride_bytes);
        auto offset = stride * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4u;
        return std::array<std::uint8_t, 4>{
            fb.pixels[offset + 0],
            fb.pixels[offset + 1],
            fb.pixels[offset + 2],
            fb.pixels[offset + 3],
        };
    };

    auto presentLeft = Builders::Window::Present(fx.space, *windowPath, "left");
    REQUIRE(presentLeft);
    CHECK(presentLeft->stats.presented);
    CHECK(presentLeft->stats.progressive_tiles_copied >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBaseLeft + "/frameIndex").value() == 1);

    auto framebufferLeft = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space,
                                                                          SP::ConcretePathStringView{targetLeft.getPath()});
    REQUIRE(framebufferLeft);
    auto center = surfaceDesc.size_px.width / 2;
    auto pixelLeft = sample_pixel(*framebufferLeft, center, center);
    CHECK(pixelLeft == expected_left_bytes);

    auto presentRight = Builders::Window::Present(fx.space, *windowPath, "right");
    REQUIRE(presentRight);
    CHECK(presentRight->stats.presented);
    CHECK(presentRight->stats.progressive_tiles_copied >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBaseRight + "/frameIndex").value() == 1);

    auto framebufferRight = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space,
                                                                           SP::ConcretePathStringView{targetRight.getPath()});
    REQUIRE(framebufferRight);
    auto pixelRight = sample_pixel(*framebufferRight, center, center);
    CHECK(pixelRight == expected_right_bytes);

    auto presentLeftAgain = Builders::Window::Present(fx.space, *windowPath, "left");
    REQUIRE(presentLeftAgain);
    CHECK(presentLeftAgain->stats.presented);
    CHECK(fx.space.read<uint64_t>(metricsBaseLeft + "/frameIndex").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBaseRight + "/frameIndex").value() == 1);
}

TEST_CASE("Window::Present handles multi-window multi-surface wiring") {
    RendererFixture fx;

    RectCommand red_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    RectCommand blue_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {0.0f, 0.0f, 1.0f, 1.0f},
    };

    auto sceneRed = create_scene(fx,
                                 "scene_multi_window_red",
                                 make_rect_bucket({
                                     RectDrawableDef{
                                         .id = 0x31u,
                                         .fingerprint = 0xAAAA555533337777u,
                                         .rect = red_rect,
                                     },
                                 }));
    auto sceneBlue = create_scene(fx,
                                  "scene_multi_window_blue",
                                  make_rect_bucket({
                                      RectDrawableDef{
                                          .id = 0x32u,
                                          .fingerprint = 0xBBBB666644448888u,
                                          .rect = blue_rect,
                                      },
                                  }));
    auto rendererPath = create_renderer(fx, "renderer_multi_window");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfaceRed = create_surface(fx, "surface_multi_window_red", surfaceDesc, rendererPath.getPath());
    auto surfaceBlue = create_surface(fx, "surface_multi_window_blue", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfaceRed, sceneRed));
    REQUIRE(Surface::SetScene(fx.space, surfaceBlue, sceneBlue));

    Builders::WindowParams primaryWindow{};
    primaryWindow.name = "primary_window";
    primaryWindow.title = "Primary";
    primaryWindow.width = surfaceDesc.size_px.width;
    primaryWindow.height = surfaceDesc.size_px.height;
    auto windowPrimary = Builders::Window::Create(fx.space, fx.root_view(), primaryWindow);
    REQUIRE(windowPrimary);

    Builders::WindowParams mirrorWindow{};
    mirrorWindow.name = "mirror_window";
    mirrorWindow.title = "Mirror";
    mirrorWindow.width = surfaceDesc.size_px.width;
    mirrorWindow.height = surfaceDesc.size_px.height;
    auto windowMirror = Builders::Window::Create(fx.space, fx.root_view(), mirrorWindow);
    REQUIRE(windowMirror);

    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPrimary, "main", surfaceRed));
    enable_framebuffer_capture(fx.space, *windowPrimary, "main");
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowMirror, "main", surfaceBlue));
    enable_framebuffer_capture(fx.space, *windowMirror, "main");
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowMirror, "mirror", surfaceRed));
    enable_framebuffer_capture(fx.space, *windowMirror, "mirror");

    auto targetRed = resolve_target(fx, surfaceRed);
    auto targetBlue = resolve_target(fx, surfaceBlue);
    auto metricsBaseRed = std::string(targetRed.getPath()) + "/output/v1/common";
    auto metricsBaseBlue = std::string(targetBlue.getPath()) + "/output/v1/common";

    auto expected_red_bytes = encode_linear_to_bytes(make_linear_color(red_rect.color),
                                                     surfaceDesc,
                                                     true);
    auto expected_blue_bytes = encode_linear_to_bytes(make_linear_color(blue_rect.color),
                                                      surfaceDesc,
                                                      true);

    auto sample_pixel = [](Builders::SoftwareFramebuffer const& fb, int x, int y) {
        auto stride = static_cast<std::size_t>(fb.row_stride_bytes);
        auto offset = stride * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4u;
        return std::array<std::uint8_t, 4>{
            fb.pixels[offset + 0],
            fb.pixels[offset + 1],
            fb.pixels[offset + 2],
            fb.pixels[offset + 3],
        };
    };

    auto presentPrimary = Builders::Window::Present(fx.space, *windowPrimary, "main");
    REQUIRE(presentPrimary);
    CHECK(presentPrimary->stats.presented);
    CHECK(presentPrimary->stats.frame.frame_index == 1);
    CHECK(fx.space.read<uint64_t>(metricsBaseRed + "/frameIndex").value() == 1);

    auto framebufferRed = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space,
                                                                         SP::ConcretePathStringView{targetRed.getPath()});
    REQUIRE(framebufferRed);
    auto center = surfaceDesc.size_px.width / 2;
    auto pixelRed = sample_pixel(*framebufferRed, center, center);
    CHECK(pixelRed == expected_red_bytes);

    auto presentMirrorMain = Builders::Window::Present(fx.space, *windowMirror, "main");
    REQUIRE(presentMirrorMain);
    CHECK(presentMirrorMain->stats.presented);
    CHECK(presentMirrorMain->stats.frame.frame_index == 1);
    CHECK(fx.space.read<uint64_t>(metricsBaseBlue + "/frameIndex").value() == 1);

    auto framebufferBlue = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space,
                                                                          SP::ConcretePathStringView{targetBlue.getPath()});
    REQUIRE(framebufferBlue);
    auto pixelBlue = sample_pixel(*framebufferBlue, center, center);
    CHECK(pixelBlue == expected_blue_bytes);

    auto presentMirrorShared = Builders::Window::Present(fx.space, *windowMirror, "mirror");
    REQUIRE(presentMirrorShared);
    CHECK(presentMirrorShared->stats.presented);
    CHECK(presentMirrorShared->stats.frame.frame_index == 2);
    CHECK(fx.space.read<uint64_t>(metricsBaseRed + "/frameIndex").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBaseBlue + "/frameIndex").value() == 1);

    auto sharedFramebuffer = Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space,
                                                                            SP::ConcretePathStringView{targetRed.getPath()});
    REQUIRE(sharedFramebuffer);
    auto sharedPixel = sample_pixel(*sharedFramebuffer, center, center);
    CHECK(sharedPixel == expected_red_bytes);
}

TEST_CASE("Window::Present reads present policy overrides from PathSpace") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x55AAu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "policy_node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.3f, 0.6f, 0.9f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_policy", bucket);
    auto rendererPath = create_renderer(fx, "renderer_policy");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_policy", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "policy_window";
    windowParams.title = "Policy";
    windowParams.width = 320;
    windowParams.height = 240;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    auto viewBase = std::string(windowPath->getPath()) + "/views/main";
    REQUIRE(fx.space.insert(viewBase + "/present/policy", std::string{"AlwaysFresh"}).errors.empty());
    REQUIRE(fx.space.insert(viewBase + "/present/params/staleness_budget_ms", 4.5).errors.empty());
    REQUIRE(fx.space.insert(viewBase + "/present/params/frame_timeout_ms", 12.0).errors.empty());
    REQUIRE(fx.space.insert(viewBase + "/present/params/max_age_frames", static_cast<std::uint64_t>(2)).errors.empty());
    REQUIRE(fx.space.insert(viewBase + "/present/params/auto_render_on_present", false).errors.empty());
    REQUIRE(fx.space.insert(viewBase + "/present/params/vsync_align", false).errors.empty());

    auto presentStatus = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(presentStatus);
    CHECK(presentStatus->stats.mode == PathWindowView::PresentMode::AlwaysFresh);
    CHECK(presentStatus->stats.wait_budget_ms == doctest::Approx(12.0).epsilon(0.1));

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/presentMode").value() == "AlwaysFresh");
    CHECK(fx.space.read<double, std::string>(metricsBase + "/stalenessBudgetMs").value() == doctest::Approx(4.5));
    CHECK(fx.space.read<double, std::string>(metricsBase + "/frameTimeoutMs").value() == doctest::Approx(12.0));
    CHECK(fx.space.read<double>(metricsBase + "/waitBudgetMs").value() == doctest::Approx(12.0).epsilon(0.1));
    CHECK(fx.space.read<uint64_t>(metricsBase + "/maxAgeFrames").value() == 2);
    CHECK_FALSE(fx.space.read<bool, std::string>(metricsBase + "/autoRenderOnPresent").value());
    CHECK_FALSE(fx.space.read<bool, std::string>(metricsBase + "/vsyncAlign").value());
    CHECK_FALSE(fx.space.read<bool, std::string>(metricsBase + "/stale").value());

    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    auto storedFramebuffer = fx.space.read<Builders::SoftwareFramebuffer, std::string>(framebufferPath);
    REQUIRE(storedFramebuffer);
    CHECK(storedFramebuffer->width == surfaceDesc.size_px.width);
    CHECK(storedFramebuffer->height == surfaceDesc.size_px.height);
}

TEST_CASE("Window auto render scheduling enqueues render request when frame stays stale") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xCAFEu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "auto_render/stale", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.6f, 0.4f, 0.2f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_auto_render", bucket);
    auto rendererPath = create_renderer(fx, "renderer_auto_render");
    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_auto_render", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto targetPath = resolve_target(fx, surfacePath);
    auto queuePath = std::string(targetPath.getPath()) + "/events/renderRequested/queue";
    while (true) {
        auto drained = fx.space.take<Builders::AutoRenderRequestEvent>(queuePath);
        if (!drained) {
            break;
        }
    }

    PathWindowView::PresentStats stats{};
    stats.skipped = true;
    stats.buffered_frame_consumed = false;
    stats.mode = PathWindowView::PresentMode::PreferLatestCompleteWithBudget;
    stats.frame.frame_index = 7;
    stats.frame.revision = 3;
    stats.frame_age_frames = 2;
    stats.frame_age_ms = 40.0;

    PathWindowView::PresentPolicy policy{};
    policy.auto_render_on_present = true;
    policy.max_age_frames = 1;
    policy.staleness_budget = std::chrono::milliseconds{8};
    policy.staleness_budget_ms_value = 8.0;
    policy.frame_timeout = std::chrono::milliseconds{20};
    policy.frame_timeout_ms_value = 20.0;

    auto scheduled = maybe_schedule_auto_render(fx.space,
                                                std::string(targetPath.getPath()),
                                                stats,
                                                policy);
    REQUIRE(scheduled);
    CHECK(*scheduled);

    auto event = fx.space.take<Builders::AutoRenderRequestEvent>(queuePath);
    REQUIRE(event);
    CHECK(event->frame_index == stats.frame.frame_index);
    CHECK(event->reason.find("present-skipped") != std::string::npos);
    CHECK(event->reason.find("age-frames") != std::string::npos);
}

TEST_CASE("Window auto render scheduling no-ops when disabled") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xBEEFu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "auto_render/disabled", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.4f, 0.6f, 0.2f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_auto_render_disabled", bucket);
    auto rendererPath = create_renderer(fx, "renderer_auto_render_disabled");
    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_auto_render_disabled", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto targetPath = resolve_target(fx, surfacePath);
    auto queuePath = std::string(targetPath.getPath()) + "/events/renderRequested/queue";
    while (true) {
        auto drained = fx.space.take<Builders::AutoRenderRequestEvent>(queuePath);
        if (!drained) {
            break;
        }
    }

    PathWindowView::PresentStats stats{};
    stats.skipped = true;
    stats.buffered_frame_consumed = false;
    stats.frame.frame_index = 11;
    stats.frame.revision = 5;
    stats.frame_age_frames = 4;
    stats.frame_age_ms = 80.0;

    PathWindowView::PresentPolicy policy{};
    policy.auto_render_on_present = false;
    policy.max_age_frames = 1;
    policy.staleness_budget_ms_value = 8.0;
    policy.frame_timeout = std::chrono::milliseconds{20};
    policy.frame_timeout_ms_value = 20.0;

    auto scheduled = maybe_schedule_auto_render(fx.space,
                                                std::string(targetPath.getPath()),
                                                stats,
                                                policy);
    REQUIRE(scheduled);
    CHECK_FALSE(*scheduled);

    auto no_event = fx.space.take<Builders::AutoRenderRequestEvent>(queuePath);
    CHECK_FALSE(no_event);
    auto code = no_event.error().code;
    bool is_expected_code = (code == Error::Code::NoObjectFound)
                            || (code == Error::Code::NoSuchPath);
    CHECK(is_expected_code);
}

TEST_CASE("PathWindowView reports progressive seqlock skips") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x330001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node/progressive", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.5f, 0.5f, 0.5f, 0.5f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_seqlock", bucket);
    auto rendererPath = create_renderer(fx, "renderer_seqlock");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_seqlock", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 1;

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    REQUIRE_FALSE(dirty_tiles.empty());

    auto writer = surface.begin_progressive_tile(dirty_tiles.front(), TilePass::OpaqueInProgress);

    PathWindowView view;
    PathWindowView::PresentPolicy policy{};
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    PathWindowView::PresentRequest request{
        .now = std::chrono::steady_clock::now(),
        .vsync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{16},
        .framebuffer = framebuffer,
        .dirty_tiles = std::span<const std::size_t>(dirty_tiles.data(), dirty_tiles.size()),
    };

    auto presentStats = view.present(surface, policy, request);
    CHECK(presentStats.progressive_skip_seq_odd >= 1);
    CHECK(presentStats.progressive_tiles_copied == 0);

    writer.abort();
}

TEST_CASE("Window::Present records progressive seqlock metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x440001u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {AlphaBlend};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "window/seqlock", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.6f, 0.4f, 0.8f, 0.5f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_window_seqlock", bucket);
    auto rendererPath = create_renderer(fx, "renderer_window_seqlock");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window_seqlock", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_seqlock";
    windowParams.title = "Seqlock";
    windowParams.width = 64;
    windowParams.height = 64;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    std::optional<ProgressiveSurfaceBuffer::TileWriter> locked_tile;
    struct HookReset {
        ~HookReset() { Window::TestHooks::ResetBeforePresentHook(); }
    } hook_reset;

    Window::TestHooks::SetBeforePresentHook(
        [&](PathSurfaceSoftware& surface,
            PathWindowView::PresentPolicy& /*policy*/,
            std::vector<std::size_t>& dirty_tiles) {
            REQUIRE_FALSE(dirty_tiles.empty());
            locked_tile.emplace(surface.begin_progressive_tile(dirty_tiles.front(),
                                                               TilePass::OpaqueInProgress));
        });

    auto present = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(present);
    CHECK(present->stats.progressive_skip_seq_odd >= 1);
    CHECK(present->stats.progressive_tiles_copied == 0);

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveSkipOddSeq").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() == 0);

    if (locked_tile) {
        locked_tile->abort();
        locked_tile.reset();
    }
}

TEST_CASE("Window::Present AlwaysFresh skip records deadline metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x501u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "always_fresh/node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.4f, 0.3f, 0.7f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_window_always_fresh", bucket);
    auto rendererPath = create_renderer(fx, "renderer_window_always_fresh");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window_always_fresh", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_always_fresh";
    windowParams.title = "AlwaysFresh";
    windowParams.width = 320;
    windowParams.height = 240;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));
    enable_framebuffer_capture(fx.space, *windowPath, "main");

    auto baseline = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(baseline);
    CHECK(baseline->stats.presented);

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);

    struct HookGuard {
        ~HookGuard() { Window::TestHooks::ResetBeforePresentHook(); }
    } guard{};

    Window::TestHooks::SetBeforePresentHook(
        [&](PathSurfaceSoftware& surface,
            PathWindowView::PresentPolicy& policy,
            std::vector<std::size_t>& dirty_tiles) {
            policy.mode = PathWindowView::PresentMode::AlwaysFresh;
            policy.frame_timeout = std::chrono::milliseconds{1};
            policy.frame_timeout_ms_value = 1.0;
            policy.staleness_budget_ms_value = 1.0;
            policy.auto_render_on_present = false;
            dirty_tiles.clear();
            surface.resize(surface.desc());
        });

    auto skipped = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(skipped);
    CHECK_FALSE(skipped->stats.presented);
    CHECK(skipped->stats.skipped);
    CHECK(skipped->stats.mode == PathWindowView::PresentMode::AlwaysFresh);
    CHECK_FALSE(skipped->stats.buffered_frame_consumed);
    CHECK(skipped->stats.wait_budget_ms == doctest::Approx(1.0).epsilon(0.2));

    auto queuePath = std::string(targetPath.getPath()) + "/events/renderRequested/queue";
    auto autoRenderEvent = fx.space.take<Builders::AutoRenderRequestEvent>(queuePath);
    CHECK_FALSE(autoRenderEvent);
    auto code = autoRenderEvent.error().code;
    bool expected_code = (code == Error::Code::NoObjectFound)
                         || (code == Error::Code::NoSuchPath);
    CHECK(expected_code);

    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() >= 2);
    CHECK(fx.space.read<bool>(metricsBase + "/lastPresentSkipped").value());
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/presented").value());
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/bufferedFrameConsumed").value());
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/presentMode").value() == "AlwaysFresh");
    CHECK(fx.space.read<double>(metricsBase + "/waitBudgetMs").value() == doctest::Approx(1.0).epsilon(0.2));
    CHECK(fx.space.read<uint64_t>(metricsBase + "/presentedAgeFrames").value() >= 1);
}

TEST_CASE("Window::Present progressive diagnostics reflect dirty hints") {
    RendererFixture fx;
    ScopedEnv metrics_env{"PATHSPACE_UI_DAMAGE_METRICS", "1"};

    RectCommand rect_initial{
        .min_x = 1.0f,
        .min_y = 1.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .color = {0.8f, 0.1f, 0.1f, 1.0f},
    };

    auto scenePath = create_scene(fx,
                                  "scene_window_progressive_diagnostics",
                                  make_rect_bucket({
                                      RectDrawableDef{
                                          .id = 0x301u,
                                          .fingerprint = 0x1010101010101010u,
                                          .rect = rect_initial,
                                      },
                                  }));
    auto rendererPath = create_renderer(fx, "renderer_window_progressive_diagnostics");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx,
                                      "surface_window_progressive_diagnostics",
                                      surfaceDesc,
                                      rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "window_progressive_diagnostics";
    windowParams.title = "Progressive Diagnostics";
    windowParams.width = 160;
    windowParams.height = 160;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto windowMetricsBase = std::string(windowPath->getPath()) + "/diagnostics/metrics/live/views/main/present";

    auto submit_hint = [&](RectCommand const& rect) {
        DirtyRectHint hint{
            .min_x = rect.min_x,
            .min_y = rect.min_y,
            .max_x = rect.max_x,
            .max_y = rect.max_y,
        };
        auto status = Builders::Renderer::SubmitDirtyRects(
            fx.space,
            SP::ConcretePathStringView{targetPath.getPath()},
            std::span<const DirtyRectHint>{&hint, 1});
        REQUIRE(status);
    };

    submit_hint(rect_initial);
    auto firstPresent = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(firstPresent);
    CHECK(firstPresent->stats.presented);
    CHECK(firstPresent->stats.progressive_tiles_copied >= 1);
    CHECK(firstPresent->stats.progressive_rects_coalesced >= 1);
    CHECK(firstPresent->stats.progressive_rects_coalesced >= 1);

    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRectsCoalesced").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveSkipOddSeq").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRecopyAfterSeqChange").value() == 0);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/lastError").value().empty());
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesUpdated").value() >= 1);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveBytesCopied").value() > 0);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTileSize").value() >= 32);
    CHECK(fx.space.read<bool>(windowMetricsBase + "/progressiveTileDiagnosticsEnabled").value());
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesDirty").value() >= 1);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesTotal").value() >= 1);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesSkipped").value() == 0);

    RectCommand rect_update{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.1f, 0.8f, 0.1f, 1.0f},
    };
    fx.publish_snapshot(scenePath,
                        make_rect_bucket({
                            RectDrawableDef{
                                .id = 0x301u,
                                .fingerprint = 0x2020202020202020u,
                                .rect = rect_update,
                            },
                        }));
    submit_hint(rect_update);

    auto secondPresent = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(secondPresent);
    CHECK(secondPresent->stats.presented);
    CHECK(secondPresent->stats.progressive_tiles_copied >= 1);
    CHECK(secondPresent->stats.progressive_rects_coalesced >= 1);
    CHECK(secondPresent->stats.progressive_rects_coalesced >= 1);

    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() >= 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRectsCoalesced").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveBytesCopied").value() > 0);
    CHECK(fx.space.read<std::string, std::string>(metricsBase + "/lastError").value().empty());
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesUpdated").value() >= 1);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveBytesCopied").value() > 0);
    CHECK(fx.space.read<uint64_t>(windowMetricsBase + "/progressiveTilesDirty").value() >= 1);
}

TEST_CASE("rounded rectangles respect per-corner radii") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xABCDEFu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{2.5f, 2.5f, 0.0f}, 3.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "rounded_rect", 0, 0},
    };

    RoundedRectCommand rounded{
        .min_x = 0.5f,
        .min_y = 0.5f,
        .max_x = 4.5f,
        .max_y = 4.5f,
        .radius_top_left = 1.5f,
        .radius_top_right = 1.0f,
        .radius_bottom_right = 1.5f,
        .radius_bottom_left = 1.0f,
        .color = {0.0f, 1.0f, 0.0f, 1.0f},
    };
    encode_rounded_rect_command(rounded, bucket);

    auto scenePath = create_scene(fx, "scene_round", bucket);
    auto rendererPath = create_renderer(fx, "renderer_round");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 5;
    surfaceDesc.size_px.height = 5;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_round", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 1;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto encode_srgb = true;
    auto desc = surfaceDesc;

    auto read_pixel = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
        return {
            buffer[offset + 0],
            buffer[offset + 1],
            buffer[offset + 2],
            buffer[offset + 3]
        };
    };

    auto clear_bytes = encode_linear_to_bytes(make_linear_color(settings.clear_color),
                                              desc,
                                              encode_srgb);
    auto fill_bytes = encode_linear_to_bytes(make_linear_color(rounded.color),
                                             desc,
                                             encode_srgb);

    // Corners should remain clear due to rounded radii.
    CHECK(read_pixel(0, 0) == clear_bytes);
    CHECK(read_pixel(4, 0) == clear_bytes);
    CHECK(read_pixel(0, 4) == clear_bytes);
    CHECK(read_pixel(4, 4) == clear_bytes);

    // Interior pixels remain filled.
    CHECK(read_pixel(2, 2) == fill_bytes);
    CHECK(read_pixel(2, 1) == fill_bytes);
    CHECK(read_pixel(1, 2) == fill_bytes);
}

TEST_CASE("linear BGRA framebuffer respects color management settings") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x010101u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 3.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "bgra_rect", 0, 0},
    };

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .color = {0.8f, 0.2f, 0.4f, 0.5f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_bgra", bucket);
    auto rendererPath = create_renderer(fx, "renderer_bgra");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 3;
    surfaceDesc.size_px.height = 3;
    surfaceDesc.pixel_format = PixelFormat::BGRA8Unorm;
    surfaceDesc.color_space = ColorSpace::Linear;
    surfaceDesc.premultiplied_alpha = false;

    auto surfacePath = create_surface(fx, "surface_bgra", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 5;

    auto rendered = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(rendered);

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();

    auto expected_rgba = encode_linear_to_bytes(make_linear_color(rect.color),
                                                surfaceDesc,
                                                /*encode_srgb=*/false);

    auto read_pixel = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
        return {
            buffer[offset + 0],
            buffer[offset + 1],
            buffer[offset + 2],
            buffer[offset + 3]
        };
    };

    auto pixel = read_pixel(1, 1);
    CHECK(pixel[0] == expected_rgba[2]); // B channel
    CHECK(pixel[1] == expected_rgba[1]); // G channel
    CHECK(pixel[2] == expected_rgba[0]); // R channel
    CHECK(pixel[3] == expected_rgba[3]); // Alpha unchanged

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
}

TEST_CASE("progressive-only surfaces present via progressive path") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x222222u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {1};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "progressive_node", 0, 0},
    };

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.3f, 0.5f, 0.7f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_progressive_only", bucket);
    auto rendererPath = create_renderer(fx, "renderer_progressive_only");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_progressive_only", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware::Options options{};
    options.enable_buffered = false;
    options.enable_progressive = true;
    options.progressive_tile_size_px = 1;

    PathSurfaceSoftware surface{surfaceDesc, options};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.time.frame_index = 3;

    auto renderStats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(renderStats);

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    REQUIRE_FALSE(dirty_tiles.empty());

    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    PathWindowView presenter;
    PathWindowView::PresentPolicy policy{};
    policy.mode = PathWindowPresentMode::AlwaysLatestComplete;

    auto now = std::chrono::steady_clock::now();
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + std::chrono::milliseconds{16},
        .framebuffer = framebuffer,
        .dirty_tiles = dirty_tiles,
    };

    auto stats = presenter.present(surface, policy, request);
    CHECK(stats.presented);
    CHECK_FALSE(stats.buffered_frame_consumed);
    CHECK(stats.used_progressive);
    CHECK(stats.progressive_tiles_copied == dirty_tiles.size());
    CHECK_FALSE(stats.skipped);
    CHECK(stats.frame.frame_index == settings.time.frame_index);
    CHECK(stats.frame.revision == renderStats->revision);
    CHECK(stats.present_ms >= 0.0);
    CHECK(stats.error.empty());
}

} // TEST_SUITE
