#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathWindowView.hpp>
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
#include <optional>
#include <span>
#include <string>
#include <sstream>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
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
using namespace SP::UI::PipelineFlags;
namespace UIScene = SP::UI::Scene;

namespace {

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
        .description = "Test renderer",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params, RendererKind::Software2D);
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

auto encode_image_command(ImageCommand const& image,
                          DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(ImageCommand));
    std::memcpy(bucket.command_payload.data() + offset, &image, sizeof(ImageCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Image));
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

    PathSurfaceSoftware surface{surfaceDesc};
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
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 2);

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

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);

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

    auto buffer = copy_buffer(surface);
    auto stride = surface.row_stride_bytes();
    auto center_offset = stride + 4;
    CHECK(buffer[center_offset + 3] > 0);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/unsupportedCommands").value() == 0);
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
        .color = {0.2f, 0.3f, 0.4f, 1.0f},
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

    auto presentStatus = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(presentStatus);

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandCount").value() == 1);
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/lastPresentSkipped").value());
    CHECK(fx.space.read<bool>(metricsBase + "/presented").value());
    CHECK(fx.space.read<bool>(metricsBase + "/bufferedFrameConsumed").value());
    CHECK(fx.space.read<bool>(metricsBase + "/usedProgressive").value());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRectsCoalesced").value() >= 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveSkipOddSeq").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveRecopyAfterSeqChange").value() == 0);
    CHECK(fx.space.read<double>(metricsBase + "/waitBudgetMs").value() == doctest::Approx(16.0).epsilon(0.1));
    CHECK(fx.space.read<double>(metricsBase + "/presentMs").value() >= 0.0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueSortViolations").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaSortViolations").value() == 0);
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
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

    constexpr int kIterations = 6;
    for (int i = 0; i < kIterations; ++i) {
        auto present = Builders::Window::Present(fx.space, *windowPath, "main");
        REQUIRE(present);
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
