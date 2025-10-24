#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <span>

namespace {

using namespace SP;
using namespace SP::UI::Builders;
using SP::UI::PathWindowPresentPolicy;
using SP::UI::PathWindowPresentStats;
using SP::UI::PathWindowView;
namespace UIScene = SP::UI::Scene;
using SP::UI::Builders::Diagnostics::PathSpaceError;
namespace Widgets = SP::UI::Builders::Widgets;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;
namespace WidgetFocus = SP::UI::Builders::Widgets::Focus;
namespace Html = SP::UI::Html;
namespace AppBootstrap = SP::UI::Builders::App;
using SP::UI::Builders::AutoRenderRequestEvent;

using UIScene::DrawableBucketSnapshot;
using UIScene::SceneSnapshotBuilder;
using UIScene::SnapshotPublishOptions;
using UIScene::RectCommand;
using UIScene::RoundedRectCommand;
using UIScene::DrawCommandKind;
using UIScene::ImageCommand;
using UIScene::Transform;
using UIScene::BoundingSphere;
using UIScene::BoundingBox;
using UIScene::DrawableAuthoringMapEntry;
using SP::UI::MaterialDescriptor;
namespace Pipeline = SP::UI::PipelineFlags;
using SP::UI::MaterialResourceResidency;
using SP::UI::Html::Asset;

struct BuildersFixture {
    PathSpace     space;
    AppRootPath   app_root{"/system/applications/test_app"};
    auto root_view() const -> SP::App::AppRootPathView { return SP::App::AppRootPathView{app_root.getPath()}; }
};

constexpr std::array<std::uint8_t, 78> kTestPngRgba = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,
    0,0,0,21,73,68,65,84,120,156,99,248,207,192,240,31,8,27,24,128,52,8,56,0,0,68,19,8,185,
    109,230,62,33,0,0,0,0,73,69,78,68,174,66,96,130
};

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

namespace fs = std::filesystem;

struct WidgetGoldenData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

auto widget_golden_dir() -> fs::path {
    return fs::path{PATHSPACE_SOURCE_DIR} / "tests" / "ui" / "golden";
}

auto widget_golden_path(std::string_view name) -> fs::path {
    return widget_golden_dir() / fs::path{name};
}

auto widget_env_update_goldens() -> bool {
    if (auto* value = std::getenv("PATHSPACE_UPDATE_GOLDENS")) {
        std::string_view view{value};
        if (view.empty() || view == "0" || view == "false" || view == "FALSE") {
            return false;
        }
        return true;
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
            return std::uint8_t{0};
        };
        auto high = from_hex(static_cast<unsigned char>(hex[i]));
        auto low = from_hex(static_cast<unsigned char>(hex[i + 1]));
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

auto read_widget_golden(std::string_view name) -> std::optional<WidgetGoldenData> {
    auto path = widget_golden_path(name);
    std::ifstream file{path};
    if (!file) {
        return std::nullopt;
    }

    WidgetGoldenData data{};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream iss{line};
        if (iss >> data.width >> data.height) {
            break;
        }
    }

    std::string hex_data;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        hex_data.append(strip_non_hex(line));
    }

    data.pixels = hex_to_bytes(hex_data);
    return data;
}

void write_widget_golden(std::string_view name,
                         int width,
                         int height,
                         std::span<std::uint8_t const> pixels) {
    auto path = widget_golden_path(name);
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::out | std::ios::trunc};
    REQUIRE(file.good());
    file << "# Widget golden framebuffer\n";
    file << width << " " << height << "\n";

    auto row_bytes = static_cast<std::size_t>(width) * 4u;
    for (int y = 0; y < height; ++y) {
        auto row_start = static_cast<std::size_t>(y) * row_bytes;
        std::ostringstream line;
        line << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < row_bytes; ++i) {
            line << std::setw(2) << static_cast<int>(pixels[row_start + i]);
        }
        file << line.str() << "\n";
    }
}

auto trim_framebuffer(SoftwareFramebuffer const& fb) -> std::vector<std::uint8_t> {
    auto row_bytes = static_cast<std::size_t>(fb.width) * 4u;
    std::vector<std::uint8_t> trimmed(static_cast<std::size_t>(fb.height) * row_bytes);
    for (int y = 0; y < fb.height; ++y) {
        auto src = fb.pixels.data() + static_cast<std::size_t>(y) * fb.row_stride_bytes;
        auto dst = trimmed.data() + static_cast<std::size_t>(y) * row_bytes;
        std::memcpy(dst, src, row_bytes);
    }
    return trimmed;
}

auto format_pixel(std::span<std::uint8_t const> rgba) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        oss << std::setw(2) << static_cast<int>(rgba[i]);
    }
    return oss.str();
}

void expect_matches_widget_golden(std::string_view name,
                                  SoftwareFramebuffer const& fb) {
    auto row_bytes = static_cast<std::size_t>(fb.width) * 4u;
    REQUIRE(row_bytes > 0);
    REQUIRE(fb.row_stride_bytes >= row_bytes);
    auto trimmed = trim_framebuffer(fb);

    if (widget_env_update_goldens()) {
        write_widget_golden(name, fb.width, fb.height, trimmed);
        return;
    }

    auto golden = read_widget_golden(name);
    REQUIRE_MESSAGE(golden.has_value(),
                    "Missing golden '" << widget_golden_path(name).string()
                    << "'. Set PATHSPACE_UPDATE_GOLDENS=1 to generate.");
    CHECK(golden->width == fb.width);
    CHECK(golden->height == fb.height);
    REQUIRE(golden->pixels.size() == trimmed.size());

    bool mismatch_found = false;
    std::size_t mismatch_index = 0;
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] != golden->pixels[i]) {
            mismatch_found = true;
            mismatch_index = i;
            break;
        }
    }

    if (mismatch_found) {
        auto pixel_index = mismatch_index / 4;
        auto x = static_cast<int>(pixel_index % fb.width);
        auto y = static_cast<int>(pixel_index / fb.width);
        auto actual = format_pixel(std::span<const std::uint8_t>{trimmed.data() + mismatch_index, 4});
        auto expected = format_pixel(std::span<const std::uint8_t>{golden->pixels.data() + mismatch_index, 4});
        FAIL("Golden mismatch in '" << name << "' at (" << x << ", " << y
             << "): expected " << expected << " got " << actual
             << ". Set PATHSPACE_UPDATE_GOLDENS=1 to refresh.");
    }
}

auto decode_state_bucket(BuildersFixture& fx,
                         ScenePath const& scene) -> DrawableBucketSnapshot {
    auto revision = Scene::ReadCurrentRevision(fx.space, scene);
    REQUIRE(revision);
    auto base = std::string(scene.getPath()) + "/builds/" + format_revision(revision->revision);
    auto bucket = SceneSnapshotBuilder::decode_bucket(fx.space, base);
    REQUIRE(bucket);
    return *bucket;
}

struct WidgetDimensions {
    int width = 0;
    int height = 0;
};

auto compute_widget_dimensions(BuildersFixture& fx,
                               Widgets::WidgetStateScenes const& scenes) -> WidgetDimensions {
    std::array<ScenePath const*, 4> all{
        &scenes.idle,
        &scenes.hover,
        &scenes.pressed,
        &scenes.disabled,
    };
    float max_width = 0.0f;
    float max_height = 0.0f;
    for (auto* scene : all) {
        auto bucket = decode_state_bucket(fx, *scene);
        bool any_box = false;
        for (std::size_t i = 0; i < bucket.bounds_boxes.size(); ++i) {
            bool valid = bucket.bounds_box_valid.empty()
                         || i >= bucket.bounds_box_valid.size()
                         || bucket.bounds_box_valid[i] != 0;
            if (!valid) {
                continue;
            }
            any_box = true;
            max_width = std::max(max_width, bucket.bounds_boxes[i].max[0]);
            max_height = std::max(max_height, bucket.bounds_boxes[i].max[1]);
        }
        if (!any_box) {
            for (auto const& sphere : bucket.bounds_spheres) {
                max_width = std::max(max_width, sphere.center[0] + sphere.radius);
                max_height = std::max(max_height, sphere.center[1] + sphere.radius);
            }
        }
    }
    int width = std::max(1, static_cast<int>(std::ceil(max_width))) + 4;
    int height = std::max(1, static_cast<int>(std::ceil(max_height))) + 4;
    return {width, height};
}

void enable_framebuffer_capture(PathSpace& space,
                                WindowPath const& windowPath,
                                std::string_view viewName) {
    auto viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    auto result = space.insert(viewBase + "/present/params/capture_framebuffer", true);
    REQUIRE(result.errors.empty());
}

struct WidgetGoldenRenderer {
    BuildersFixture& fx;
    std::string prefix;
    std::string view_name{"view"};
    RendererPath renderer;
    SurfacePath surface;
    WindowPath window;
    SP::ConcretePathString target;
    SurfaceDesc desc{};

    WidgetGoldenRenderer(BuildersFixture& fx,
                         std::string prefix,
                         int width,
                         int height)
        : fx(fx),
          prefix(std::move(prefix)) {
        RendererParams rendererParams{
            .name = this->prefix + "_renderer",
            .kind = RendererKind::Software2D,
            .description = "widget golden renderer",
        };
        auto rendererResult = Renderer::Create(fx.space, fx.root_view(), rendererParams);
        REQUIRE(rendererResult);
        renderer = *rendererResult;

        desc.size_px.width = width;
        desc.size_px.height = height;
        desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
        desc.color_space = ColorSpace::sRGB;
        desc.premultiplied_alpha = true;
        desc.progressive_tile_size_px = 32;

        SurfaceParams surfaceParams{};
        surfaceParams.name = this->prefix + "_surface";
        surfaceParams.desc = desc;
        surfaceParams.renderer = "renderers/" + rendererParams.name;
        auto surfaceResult = Surface::Create(fx.space, fx.root_view(), surfaceParams);
        REQUIRE(surfaceResult);
        surface = *surfaceResult;

        WindowParams windowParams{
            .name = this->prefix + "_window",
            .title = "widget golden",
            .width = width,
            .height = height,
            .scale = 1.0f,
            .background = "#000000",
        };
        auto windowResult = Window::Create(fx.space, fx.root_view(), windowParams);
        REQUIRE(windowResult);
        window = *windowResult;

        auto attached = Window::AttachSurface(fx.space, window, view_name, surface);
        REQUIRE(attached);

        enable_framebuffer_capture(fx.space, window, view_name);

        auto targetRel = "targets/surfaces/" + surfaceParams.name;
        auto resolved = Renderer::ResolveTargetBase(fx.space,
                                                    fx.root_view(),
                                                    renderer,
                                                    targetRel);
        REQUIRE(resolved);
        target = *resolved;
    }

    void render(ScenePath const& scene, std::string_view golden_name) {
        auto setScene = Surface::SetScene(fx.space, surface, scene);
        REQUIRE(setScene);

        auto present = Window::Present(fx.space, window, view_name);
        if (!present) {
            INFO("Window::Present error code = " << static_cast<int>(present.error().code));
            INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
        }
        REQUIRE(present);

        auto framebufferPath = std::string(target.getPath()) + "/output/v1/software/framebuffer";
        auto framebuffer = fx.space.read<SoftwareFramebuffer, std::string>(framebufferPath);
        REQUIRE(framebuffer);
        expect_matches_widget_golden(golden_name, *framebuffer);
    }
};

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

auto encode_image_command(ImageCommand const& image,
                          DrawableBucketSnapshot& bucket) -> void {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(ImageCommand));
    std::memcpy(bucket.command_payload.data() + offset, &image, sizeof(ImageCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Image));
}

auto make_image_bucket(std::uint64_t fingerprint) -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x1234u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, std::sqrt(2.0f)}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
    bucket.bounds_box_valid = {1};
    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {};
    bucket.alpha_indices = {0};
    bucket.layer_indices = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids.front(), "image_node", 0, 0}};
    bucket.drawable_fingerprints = {fingerprint};

    ImageCommand image{};
    image.min_x = 0.0f;
    image.min_y = 0.0f;
    image.max_x = 2.0f;
    image.max_y = 2.0f;
    image.uv_min_x = 0.0f;
    image.uv_min_y = 0.0f;
    image.uv_max_x = 1.0f;
    image.uv_max_y = 1.0f;
    image.image_fingerprint = fingerprint;
    image.tint = {1.0f, 1.0f, 1.0f, 1.0f};

    encode_image_command(image, bucket);
    return bucket;
}

auto make_rect_bucket() -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xABCDu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{0.0f, 0.0f, 0.0f}, 1.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}};
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
    bucket.layer_indices = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};
    bucket.drawable_fingerprints = {0};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 1.0f,
        .max_y = 1.0f,
        .color = {0.4f, 0.4f, 0.4f, 1.0f},
    };
    encode_rect_command(rect, bucket);
    return bucket;
}

TEST_CASE("Material shader key derives from pipeline flags") {
    MaterialDescriptor blended{};
    blended.pipeline_flags = Pipeline::AlphaBlend | Pipeline::ClipRect | Pipeline::DebugWireframe;
    blended.uses_image = true;

    SurfaceDesc srgb_desc{};
    srgb_desc.color_space = ColorSpace::sRGB;
    srgb_desc.premultiplied_alpha = true;

    auto blended_key = make_shader_key(blended, srgb_desc);
    CHECK(blended_key.pipeline_flags == blended.pipeline_flags);
    CHECK(blended_key.alpha_blend);
    CHECK_FALSE(blended_key.requires_unpremultiplied);
    CHECK(blended_key.srgb_framebuffer);
    CHECK(blended_key.uses_image);
    CHECK_FALSE(blended_key.debug_overdraw);
    CHECK(blended_key.debug_wireframe);

    MaterialDescriptor unpremult{};
    unpremult.pipeline_flags = Pipeline::AlphaBlend
                               | Pipeline::UnpremultipliedSrc
                               | Pipeline::DebugOverdraw;
    unpremult.uses_image = false;

    SurfaceDesc linear_desc{};
    linear_desc.color_space = ColorSpace::Linear;
    linear_desc.premultiplied_alpha = false;

    auto unpremult_key = make_shader_key(unpremult, linear_desc);
    CHECK(unpremult_key.pipeline_flags == unpremult.pipeline_flags);
    CHECK(unpremult_key.alpha_blend);
    CHECK(unpremult_key.requires_unpremultiplied);
    CHECK_FALSE(unpremult_key.srgb_framebuffer);
    CHECK_FALSE(unpremult_key.uses_image);
    CHECK(unpremult_key.debug_overdraw);
    CHECK_FALSE(unpremult_key.debug_wireframe);
}

auto publish_minimal_scene(BuildersFixture& fx, ScenePath const& scenePath) -> void {
    auto bucket = make_rect_bucket();
    SceneSnapshotBuilder builder{fx.space, fx.root_view(), scenePath};
    SnapshotPublishOptions opts{};
    opts.metadata.author = "tests";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);
    auto ready = Scene::WaitUntilReady(fx.space, scenePath, std::chrono::milliseconds{10});
    REQUIRE(ready);
}

template <typename T>
auto read_value(PathSpace const& space, std::string const& path) -> SP::Expected<T> {
    auto const& base = static_cast<SP::PathSpaceBase const&>(space);
    return base.template read<T, std::string>(path);
}

auto make_sample_settings() -> RenderSettings {
    RenderSettings settings;
    settings.time.time_ms   = 120.0;
    settings.time.delta_ms  = 16.0;
    settings.time.frame_index = 5;
    settings.pacing.has_user_cap_fps = true;
    settings.pacing.user_cap_fps    = 60.0;
    settings.surface.size_px = {1920, 1080};
    settings.surface.dpi_scale = 2.0f;
    settings.surface.visibility = false;
    settings.surface.metal.storage_mode = MetalStorageMode::Shared;
    settings.surface.metal.texture_usage = static_cast<std::uint8_t>(MetalTextureUsage::ShaderRead)
                                           | static_cast<std::uint8_t>(MetalTextureUsage::RenderTarget);
    settings.surface.metal.iosurface_backing = true;
    settings.clear_color = {0.1f, 0.2f, 0.3f, 0.4f};
    RenderSettings::Camera camera;
    camera.projection = RenderSettings::Camera::Projection::Perspective;
    camera.z_near = 0.25f;
    camera.z_far  = 250.0f;
    camera.enabled = true;
    settings.camera = camera;
    RenderSettings::Debug debug;
    debug.flags = 0xABCDu;
    debug.enabled = true;
    settings.debug = debug;
    settings.microtri_rt.enabled = true;
    settings.microtri_rt.budget.microtri_edge_px = 0.75f;
    settings.microtri_rt.budget.max_microtris_per_frame = 150000;
    settings.microtri_rt.budget.rays_per_vertex = 2;
    settings.microtri_rt.path.max_bounces = 2;
    settings.microtri_rt.path.rr_start_bounce = 1;
    settings.microtri_rt.use_hardware_rt = RenderSettings::MicrotriRT::HardwareMode::ForceOn;
    settings.microtri_rt.environment.hdr_path = "/assets/hdr/sunrise.hdr";
    settings.microtri_rt.environment.intensity = 1.5f;
    settings.microtri_rt.environment.rotation = 0.25f;
    settings.microtri_rt.path.allow_caustics = true;
    settings.microtri_rt.clamp.direct = 5.0f;
    settings.microtri_rt.clamp.indirect = 10.0f;
    settings.microtri_rt.clamp.has_direct = true;
    settings.microtri_rt.clamp.has_indirect = true;
    settings.microtri_rt.progressive_accumulation = true;
    settings.microtri_rt.vertex_accum_half_life = 0.4f;
    settings.microtri_rt.seed = 12345u;
    settings.renderer.backend_kind = RendererKind::Software2D;
    settings.renderer.metal_uploads_enabled = false;
    return settings;
}

auto approx_ms(std::chrono::system_clock::time_point tp) -> std::chrono::milliseconds {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
}

} // namespace

TEST_SUITE("UIBuilders") {

TEST_CASE("Scene publish and read current revision") {
    BuildersFixture fx;

    SceneParams sceneParams{ .name = "main", .description = "Main scene" };
    auto scenePath = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    SceneRevisionDesc revision{};
    revision.revision     = 42;
    revision.published_at = std::chrono::system_clock::now();
    revision.author       = "tester";

    std::vector<std::byte> bucket(8, std::byte{0x1F});
    std::vector<std::byte> metadata(4, std::byte{0x2A});

    auto publish = Scene::PublishRevision(fx.space,
                                          *scenePath,
                                          revision,
                                          std::span<const std::byte>(bucket.data(), bucket.size()),
                                          std::span<const std::byte>(metadata.data(), metadata.size()));
    REQUIRE(publish);

    auto wait = Scene::WaitUntilReady(fx.space, *scenePath, std::chrono::milliseconds{10});
    REQUIRE(wait);

    auto current = Scene::ReadCurrentRevision(fx.space, *scenePath);
    REQUIRE(current);
    CHECK(current->revision == revision.revision);
    CHECK(current->author == revision.author);
    CHECK(approx_ms(current->published_at) == approx_ms(revision.published_at));
}

TEST_CASE("Renderer settings round-trip") {
    BuildersFixture fx;

    RendererParams rendererParams{
        .name = "2d",
        .kind = RendererKind::Software2D,
        .description = "Software renderer",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/editor");
    REQUIRE(targetBase);

    auto settings = make_sample_settings();
    auto updated  = Renderer::UpdateSettings(fx.space,
                                             ConcretePathView{targetBase->getPath()},
                                             settings);
    REQUIRE(updated);

    auto stored = Renderer::ReadSettings(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(stored);
    CHECK(stored->time.time_ms == doctest::Approx(settings.time.time_ms));
    CHECK(stored->time.delta_ms == doctest::Approx(settings.time.delta_ms));
    CHECK(stored->time.frame_index == settings.time.frame_index);
    CHECK(stored->pacing.has_user_cap_fps == settings.pacing.has_user_cap_fps);
    CHECK(stored->pacing.user_cap_fps == doctest::Approx(settings.pacing.user_cap_fps));
    CHECK(stored->surface.size_px.width == settings.surface.size_px.width);
    CHECK(stored->surface.size_px.height == settings.surface.size_px.height);
    CHECK(stored->surface.dpi_scale == doctest::Approx(settings.surface.dpi_scale));
    CHECK(stored->surface.visibility == settings.surface.visibility);
    CHECK(stored->clear_color == settings.clear_color);
    CHECK(stored->camera.enabled == settings.camera.enabled);
    CHECK(stored->camera.projection == settings.camera.projection);
    CHECK(stored->camera.z_near == doctest::Approx(settings.camera.z_near));
    CHECK(stored->camera.z_far == doctest::Approx(settings.camera.z_far));
    CHECK(stored->debug.enabled == settings.debug.enabled);
    CHECK(stored->debug.flags == settings.debug.flags);
    CHECK(stored->microtri_rt.enabled == settings.microtri_rt.enabled);
    CHECK(stored->microtri_rt.use_hardware_rt == settings.microtri_rt.use_hardware_rt);
    CHECK(stored->microtri_rt.budget.microtri_edge_px == doctest::Approx(settings.microtri_rt.budget.microtri_edge_px));
    CHECK(stored->microtri_rt.budget.max_microtris_per_frame == settings.microtri_rt.budget.max_microtris_per_frame);
    CHECK(stored->microtri_rt.budget.rays_per_vertex == settings.microtri_rt.budget.rays_per_vertex);
    CHECK(stored->microtri_rt.path.max_bounces == settings.microtri_rt.path.max_bounces);
    CHECK(stored->microtri_rt.path.rr_start_bounce == settings.microtri_rt.path.rr_start_bounce);
    CHECK(stored->microtri_rt.environment.hdr_path == settings.microtri_rt.environment.hdr_path);
    CHECK(stored->microtri_rt.environment.intensity == doctest::Approx(settings.microtri_rt.environment.intensity));
    CHECK(stored->microtri_rt.environment.rotation == doctest::Approx(settings.microtri_rt.environment.rotation));
    CHECK(stored->microtri_rt.path.allow_caustics == settings.microtri_rt.path.allow_caustics);
    CHECK(stored->microtri_rt.clamp.direct == doctest::Approx(settings.microtri_rt.clamp.direct));
    CHECK(stored->microtri_rt.clamp.indirect == doctest::Approx(settings.microtri_rt.clamp.indirect));
    CHECK(stored->microtri_rt.clamp.has_direct == settings.microtri_rt.clamp.has_direct);
    CHECK(stored->microtri_rt.clamp.has_indirect == settings.microtri_rt.clamp.has_indirect);
    CHECK(stored->microtri_rt.progressive_accumulation == settings.microtri_rt.progressive_accumulation);
    CHECK(stored->microtri_rt.vertex_accum_half_life == doctest::Approx(settings.microtri_rt.vertex_accum_half_life));
    CHECK(stored->microtri_rt.seed == settings.microtri_rt.seed);
}

TEST_CASE("Renderer::Create stores renderer kind metadata and updates existing renderer") {
    BuildersFixture fx;

    RendererParams params{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };

    auto first = Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(first);

    auto kindPath = std::string(first->getPath()) + "/meta/kind";
    auto storedKind = read_value<RendererKind>(fx.space, kindPath);
    REQUIRE(storedKind);
    CHECK(*storedKind == RendererKind::Software2D);

    params.kind = RendererKind::Metal2D;
    auto second = Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(second);
    CHECK(second->getPath() == first->getPath());

    auto updatedKind = read_value<RendererKind>(fx.space, kindPath);
    REQUIRE(updatedKind);
    CHECK(*updatedKind == RendererKind::Metal2D);
}

TEST_CASE("Renderer::Create upgrades legacy string kind metadata") {
    BuildersFixture fx;

    auto rendererPath = std::string(fx.app_root.getPath()) + "/renderers/legacy";
    auto metaBase = rendererPath + "/meta";

    {
        auto result = fx.space.insert(metaBase + "/name", std::string{"legacy"});
        REQUIRE(result.errors.empty());
    }
    {
        auto result = fx.space.insert(metaBase + "/description", std::string{"Legacy renderer"});
        REQUIRE(result.errors.empty());
    }
    {
        auto result = fx.space.insert(metaBase + "/kind", std::string{"software"});
        REQUIRE(result.errors.empty());
    }

    RendererParams params{ .name = "legacy", .kind = RendererKind::Software2D, .description = "Upgraded renderer" };
    auto created = Renderer::Create(fx.space, fx.root_view(), params);
    if (!created) {
        INFO("Renderer::Create error code = " << static_cast<int>(created.error().code));
        INFO("Renderer::Create error message = " << created.error().message.value_or("<none>"));
    }
    REQUIRE(created);
    CHECK(created->getPath() == rendererPath);

    auto storedKind = read_value<RendererKind>(fx.space, metaBase + "/kind");
    REQUIRE(storedKind);
    CHECK(*storedKind == RendererKind::Software2D);
}

TEST_CASE("Surface::RenderOnce handles metal renderer targets") {
    BuildersFixture fx;

    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") != nullptr) {
        INFO("Surface::RenderOnce metal path exercised by dedicated PATHSPACE_ENABLE_METAL_UPLOADS UITest; skipping builders coverage");
        return;
    }

    RendererParams params{ .name = "metal", .kind = RendererKind::Metal2D, .description = "Metal renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);

    SurfaceDesc desc;
    desc.size_px = {640, 360};
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    SurfaceParams surfaceParams{ .name = "panel", .desc = desc, .renderer = "renderers/metal" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    SceneParams sceneParams{ .name = "main", .description = "scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    publish_minimal_scene(fx, *scene);

    auto linked = Surface::SetScene(fx.space, *surface, *scene);
    REQUIRE(linked);

    auto render = Surface::RenderOnce(fx.space, *surface, std::nullopt);
    if (!render) {
        INFO("Surface::RenderOnce error code = " << static_cast<int>(render.error().code));
        INFO("Surface::RenderOnce error message = " << render.error().message.value_or("<none>"));
    }
    CHECK(render);

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/panel");
    REQUIRE(targetBase);

    auto storedSettings = Renderer::ReadSettings(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->renderer.backend_kind == RendererKind::Software2D);
    CHECK_FALSE(storedSettings->renderer.metal_uploads_enabled);
    CHECK(storedSettings->surface.metal.storage_mode == desc.metal.storage_mode);
    CHECK(storedSettings->surface.metal.texture_usage == desc.metal.texture_usage);
}

TEST_CASE("Window::Present handles metal renderer targets") {
    BuildersFixture fx;

    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") != nullptr) {
        INFO("Window::Present metal path exercised by dedicated PATHSPACE_ENABLE_METAL_UPLOADS UITest; skipping builders coverage");
        return;
    }

    RendererParams params{ .name = "metal", .kind = RendererKind::Metal2D, .description = "Metal renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);

    SurfaceDesc desc;
    desc.size_px = {800, 600};
    SurfaceParams surfaceParams{ .name = "panel", .desc = desc, .renderer = "renderers/metal" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    SceneParams sceneParams{ .name = "main", .description = "scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    publish_minimal_scene(fx, *scene);

    auto linked = Surface::SetScene(fx.space, *surface, *scene);
    REQUIRE(linked);

    WindowParams windowParams{ .name = "Main", .title = "Window", .width = 1024, .height = 768, .scale = 1.0f, .background = "#000" };
    auto window = Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    auto attached = Window::AttachSurface(fx.space, *window, "view", *surface);
    REQUIRE(attached);

    auto present = Window::Present(fx.space, *window, "view");
    if (!present) {
        INFO("Window::Present error code = " << static_cast<int>(present.error().code));
        INFO("Window::Present error message = " << present.error().message.value_or("<none>"));
    }
    CHECK(present);

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/panel");
    REQUIRE(targetBase);
    auto storedSettings = Renderer::ReadSettings(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->renderer.backend_kind == RendererKind::Software2D);
    CHECK_FALSE(storedSettings->renderer.metal_uploads_enabled);
}

TEST_CASE("Scene::Create is idempotent and preserves metadata") {
    BuildersFixture fx;

    SceneParams firstParams{ .name = "main", .description = "First description" };
    auto first = Scene::Create(fx.space, fx.root_view(), firstParams);
    REQUIRE(first);

    SceneParams secondParams{ .name = "main", .description = "Second description" };
    auto second = Scene::Create(fx.space, fx.root_view(), secondParams);
    REQUIRE(second);
    CHECK(second->getPath() == first->getPath());

    auto storedDesc = read_value<std::string>(fx.space, std::string(first->getPath()) + "/meta/description");
    REQUIRE(storedDesc);
    CHECK(*storedDesc == "First description");
}

TEST_CASE("Renderer::UpdateSettings replaces any queued values atomically") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/editor");
    REQUIRE(targetBase);

    auto settingsPath = targetBase->getPath() + std::string{"/settings"};
    RenderSettings staleA;
    staleA.time.frame_index = 1;
    RenderSettings staleB;
    staleB.time.frame_index = 2;
    fx.space.insert(settingsPath, staleA);
    fx.space.insert(settingsPath, staleB);

    auto latest = make_sample_settings();
    latest.time.frame_index = 99;
    auto updated = Renderer::UpdateSettings(fx.space, ConcretePathView{targetBase->getPath()}, latest);
    REQUIRE(updated);

    auto taken = fx.space.take<RenderSettings>(settingsPath);
    REQUIRE(taken);
    CHECK(taken->time.frame_index == latest.time.frame_index);
    CHECK(taken->surface.metal.storage_mode == latest.surface.metal.storage_mode);
    CHECK(taken->surface.metal.texture_usage == latest.surface.metal.texture_usage);
    CHECK(taken->renderer.backend_kind == latest.renderer.backend_kind);
    CHECK(taken->renderer.metal_uploads_enabled == latest.renderer.metal_uploads_enabled);

    auto empty = fx.space.take<RenderSettings>(settingsPath);
    CHECK_FALSE(empty);
    auto code = empty.error().code;
    bool is_expected = (code == Error::Code::NoObjectFound) || (code == Error::Code::NoSuchPath);
    CHECK(is_expected);
}

TEST_CASE("Surface creation binds renderer and scene") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc;
    desc.size_px = {1280, 720};
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.color_space = ColorSpace::DisplayP3;
    desc.premultiplied_alpha = false;
    desc.metal.storage_mode = MetalStorageMode::Shared;
    desc.metal.texture_usage = static_cast<std::uint8_t>(MetalTextureUsage::ShaderRead)
                               | static_cast<std::uint8_t>(MetalTextureUsage::RenderTarget);
    desc.metal.iosurface_backing = true;

    SurfaceParams surfaceParams{ .name = "editor", .desc = desc, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto storedDesc = fx.space.read<SurfaceDesc>(std::string(surface->getPath()) + "/desc");
    REQUIRE(storedDesc);
    CHECK(storedDesc->size_px.width == desc.size_px.width);
    CHECK(storedDesc->size_px.height == desc.size_px.height);
    CHECK(storedDesc->pixel_format == desc.pixel_format);
    CHECK(storedDesc->color_space == desc.color_space);
    CHECK(storedDesc->premultiplied_alpha == desc.premultiplied_alpha);
    CHECK(storedDesc->metal.storage_mode == desc.metal.storage_mode);
    CHECK(storedDesc->metal.texture_usage == desc.metal.texture_usage);
    CHECK(storedDesc->metal.iosurface_backing == desc.metal.iosurface_backing);

    auto rendererStr = read_value<std::string>(fx.space, std::string(surface->getPath()) + "/renderer");
    REQUIRE(rendererStr);
    CHECK(*rendererStr == "renderers/2d");

    SceneParams sceneParams{ .name = "main", .description = "scene" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    auto link = Surface::SetScene(fx.space, *surface, *scene);
    REQUIRE(link);

    auto surfaceScene = read_value<std::string>(fx.space, std::string(surface->getPath()) + "/scene");
    REQUIRE(surfaceScene);
    CHECK(*surfaceScene == "scenes/main");

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/editor");
    REQUIRE(targetBase);

    auto targetScene = read_value<std::string>(fx.space, targetBase->getPath() + std::string{"/scene"});
    REQUIRE(targetScene);
    CHECK(*targetScene == "scenes/main");
}

TEST_CASE("Scene dirty markers update state and queue") {
    BuildersFixture fx;

    SceneParams sceneParams{ .name = "dirty_scene", .description = "Dirty scene" };
    auto scenePath = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    auto initialState = Scene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(initialState);
    CHECK(initialState->sequence == 0);
    CHECK(initialState->pending == Scene::DirtyKind::None);

    auto seq1 = Scene::MarkDirty(fx.space, *scenePath, Scene::DirtyKind::Structure);
    REQUIRE(seq1);
    CHECK(*seq1 > 0);

    auto stateAfterFirst = Scene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterFirst);
    CHECK(stateAfterFirst->sequence == *seq1);
    CHECK((stateAfterFirst->pending & Scene::DirtyKind::Structure) == Scene::DirtyKind::Structure);

    auto event1 = Scene::TakeDirtyEvent(fx.space, *scenePath, std::chrono::milliseconds{20});
    REQUIRE(event1);
    CHECK(event1->sequence == *seq1);
    CHECK(event1->kinds == Scene::DirtyKind::Structure);

    auto seq2 = Scene::MarkDirty(fx.space, *scenePath, Scene::DirtyKind::Visual | Scene::DirtyKind::Text);
    REQUIRE(seq2);
    CHECK(*seq2 > *seq1);

    auto event2 = Scene::TakeDirtyEvent(fx.space, *scenePath, std::chrono::milliseconds{20});
    REQUIRE(event2);
    CHECK(event2->sequence == *seq2);
    CHECK((event2->kinds & Scene::DirtyKind::Visual) == Scene::DirtyKind::Visual);
    CHECK((event2->kinds & Scene::DirtyKind::Text) == Scene::DirtyKind::Text);

    auto stateAfterSecond = Scene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterSecond);
    CHECK(stateAfterSecond->sequence == *seq2);
    CHECK((stateAfterSecond->pending & Scene::DirtyKind::Structure) == Scene::DirtyKind::Structure);
    CHECK((stateAfterSecond->pending & Scene::DirtyKind::Visual) == Scene::DirtyKind::Visual);
    CHECK((stateAfterSecond->pending & Scene::DirtyKind::Text) == Scene::DirtyKind::Text);

    auto cleared = Scene::ClearDirty(fx.space, *scenePath, Scene::DirtyKind::Visual);
    REQUIRE(cleared);

    auto stateAfterClear = Scene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterClear);
    CHECK((stateAfterClear->pending & Scene::DirtyKind::Visual) == Scene::DirtyKind::None);
    CHECK((stateAfterClear->pending & Scene::DirtyKind::Structure) == Scene::DirtyKind::Structure);
    CHECK((stateAfterClear->pending & Scene::DirtyKind::Text) == Scene::DirtyKind::Text);
}

TEST_CASE("Scene dirty event wait-notify latency stays within budget") {
    using namespace std::chrono_literals;

    BuildersFixture fx;

    SceneParams sceneParams{ .name = "dirty_notify_scene", .description = "Dirty notifications" };
    auto scenePath = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    std::atomic<bool> waiterReady{false};
    bool              eventSucceeded = false;
    Scene::DirtyEvent event{};
    std::chrono::milliseconds observedLatency{0};

    std::thread waiter([&]() {
        waiterReady.store(true, std::memory_order_release);
        auto start = std::chrono::steady_clock::now();
        auto taken = Scene::TakeDirtyEvent(fx.space, *scenePath, 500ms);
        auto end = std::chrono::steady_clock::now();

        observedLatency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (taken) {
            event = *taken;
            eventSucceeded = true;
        }
    });

    while (!waiterReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(20ms);

    auto seq = Scene::MarkDirty(fx.space, *scenePath, Scene::DirtyKind::Structure);
    REQUIRE(seq);

    waiter.join();

    REQUIRE(eventSucceeded);
    CHECK(event.sequence == *seq);
    CHECK(event.kinds == Scene::DirtyKind::Structure);
    CHECK(observedLatency >= 20ms);
    CHECK(observedLatency < 200ms);
}

TEST_CASE("Window attach surface records binding") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc;
    desc.size_px = {640, 480};
    SurfaceParams surfaceParams{ .name = "pane", .desc = desc, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    WindowParams windowParams{ .name = "Main", .title = "app", .width = 800, .height = 600, .scale = 1.0f, .background = "#000" };
    auto window = Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    auto attached = Window::AttachSurface(fx.space, *window, "view", *surface);
    REQUIRE(attached);

    auto surfaceBinding = read_value<std::string>(fx.space, std::string(window->getPath()) + "/views/view/surface");
    REQUIRE(surfaceBinding);
    CHECK(*surfaceBinding == "surfaces/pane");

    auto present = Window::Present(fx.space, *window, "view");
    CHECK_FALSE(present);
    CHECK(present.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("Renderer::ResolveTargetBase rejects empty specifications") {
    BuildersFixture fx;
    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    auto target = Renderer::ResolveTargetBase(fx.space, fx.root_view(), *renderer, "");
    CHECK_FALSE(target);
    CHECK(target.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Window::AttachSurface enforces shared app roots") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceParams surfaceParams{ .name = "pane", .desc = {}, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    WindowParams windowParams{ .name = "Main", .title = "app", .width = 800, .height = 600, .scale = 1.0f, .background = "#000" };
    auto window = Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    SurfacePath foreignSurface{ "/system/applications/other_app/surfaces/pane" };
    auto attached = Window::AttachSurface(fx.space, *window, "view", foreignSurface);
    CHECK_FALSE(attached);
    CHECK(attached.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Diagnostics read metrics and clear error") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    auto targetBase = Renderer::ResolveTargetBase(fx.space,
                                                  fx.root_view(),
                                                  *renderer,
                                                  "targets/surfaces/editor");
    REQUIRE(targetBase);

    auto metrics = Diagnostics::ReadTargetMetrics(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(metrics);
    CHECK(metrics->frame_index == 0);
    CHECK(metrics->revision == 0);
    CHECK(metrics->render_ms == 0.0);
    CHECK(metrics->present_ms == 0.0);
    CHECK(metrics->gpu_encode_ms == 0.0);
    CHECK(metrics->gpu_present_ms == 0.0);
    CHECK(metrics->last_present_skipped == false);
    CHECK(metrics->used_metal_texture == false);
    CHECK(metrics->backend_kind.empty());
    CHECK(metrics->last_error.empty());
    CHECK(metrics->last_error_code == 0);
    CHECK(metrics->last_error_revision == 0);
    CHECK(metrics->last_error_severity == PathSpaceError::Severity::Info);
    CHECK(metrics->last_error_timestamp_ns == 0);
    CHECK(metrics->last_error_detail.empty());
    CHECK(metrics->material_count == 0);
    CHECK(metrics->materials.empty());
    CHECK(metrics->cpu_bytes == 0);
    CHECK(metrics->cpu_soft_bytes == 0);
    CHECK(metrics->cpu_hard_bytes == 0);
    CHECK(metrics->gpu_bytes == 0);
    CHECK(metrics->gpu_soft_bytes == 0);
    CHECK(metrics->gpu_hard_bytes == 0);

    auto common = std::string(targetBase->getPath()) + "/output/v1/common";
    fx.space.insert(common + "/frameIndex", uint64_t{7});
    fx.space.insert(common + "/revision", uint64_t{13});
    fx.space.insert(common + "/renderMs", 8.5);
    fx.space.insert(common + "/presentMs", 4.25);
    fx.space.insert(common + "/lastPresentSkipped", true);
    fx.space.insert(common + "/gpuEncodeMs", 1.5);
    fx.space.insert(common + "/gpuPresentMs", 2.0);
    fx.space.insert(common + "/usedMetalTexture", true);
    fx.space.insert(common + "/backendKind", std::string{"Software2D"});
    fx.space.insert(common + "/lastError", std::string{"failure"});
    fx.space.insert(common + "/materialCount", uint64_t{2});
    std::vector<MaterialDescriptor> expected_descriptors{};
    MaterialDescriptor mat0{};
    mat0.material_id = 7;
    mat0.pipeline_flags = 0x10u;
    mat0.primary_draw_kind = static_cast<std::uint32_t>(DrawCommandKind::Rect);
    mat0.command_count = 3;
    mat0.drawable_count = 2;
    mat0.color_rgba = {0.1f, 0.2f, 0.3f, 0.4f};
    mat0.tint_rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    mat0.resource_fingerprint = 0u;
    mat0.uses_image = false;
    expected_descriptors.push_back(mat0);
    MaterialDescriptor mat1{};
    mat1.material_id = 12;
    mat1.pipeline_flags = 0x20u;
    mat1.primary_draw_kind = static_cast<std::uint32_t>(DrawCommandKind::Image);
    mat1.command_count = 5;
    mat1.drawable_count = 1;
    mat1.color_rgba = {0.0f, 0.0f, 0.0f, 0.0f};
    mat1.tint_rgba = {0.7f, 0.8f, 0.9f, 1.0f};
    mat1.resource_fingerprint = 0xABCDEFu;
    mat1.uses_image = true;
    expected_descriptors.push_back(mat1);
    fx.space.insert(common + "/materialDescriptors", expected_descriptors);
    std::vector<MaterialResourceResidency> expected_resources{};
    MaterialResourceResidency res0{};
    res0.fingerprint = 0xABCDEFu;
    res0.cpu_bytes = 4096;
    res0.gpu_bytes = 2048;
    res0.width = 64;
    res0.height = 16;
    res0.uses_image = true;
    expected_resources.push_back(res0);
    fx.space.insert(common + "/materialResourceCount", static_cast<uint64_t>(expected_resources.size()));
    fx.space.insert(common + "/materialResources", expected_resources);

    auto residency = std::string(targetBase->getPath()) + "/diagnostics/metrics/residency";
    fx.space.insert(residency + "/cpuBytes", uint64_t{64});
    fx.space.insert(residency + "/cpuSoftBytes", uint64_t{128});
    fx.space.insert(residency + "/cpuHardBytes", uint64_t{256});
    fx.space.insert(residency + "/gpuBytes", uint64_t{32});
    fx.space.insert(residency + "/gpuSoftBytes", uint64_t{96});
    fx.space.insert(residency + "/gpuHardBytes", uint64_t{192});

    auto updated = Diagnostics::ReadTargetMetrics(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(updated);
    CHECK(updated->frame_index == 7);
    CHECK(updated->revision == 13);
    CHECK(updated->render_ms == doctest::Approx(8.5));
    CHECK(updated->present_ms == doctest::Approx(4.25));
    CHECK(updated->gpu_encode_ms == doctest::Approx(1.5));
    CHECK(updated->gpu_present_ms == doctest::Approx(2.0));
    CHECK(updated->last_present_skipped == true);
    CHECK(updated->used_metal_texture == true);
    CHECK(updated->backend_kind == "Software2D");
    CHECK(updated->last_error == "failure");
    CHECK(updated->last_error_code == 0);
    CHECK(updated->last_error_revision == 0);
    CHECK(updated->last_error_severity == PathSpaceError::Severity::Info);
    CHECK(updated->last_error_timestamp_ns == 0);
    CHECK(updated->last_error_detail.empty());
    CHECK(updated->material_resource_count == expected_resources.size());
    REQUIRE(updated->material_resources.size() == expected_resources.size());
    CHECK(updated->material_resources.front().fingerprint == expected_resources.front().fingerprint);
    CHECK(updated->material_count == 2);
    REQUIRE(updated->materials.size() == 2);
    CHECK(updated->materials[0].material_id == 7);
    CHECK(updated->materials[0].pipeline_flags == 0x10u);
    CHECK(updated->materials[0].primary_draw_kind == static_cast<std::uint32_t>(DrawCommandKind::Rect));
    CHECK(updated->materials[0].drawable_count == 2);
    CHECK(updated->materials[0].command_count == 3);
    CHECK(updated->materials[0].uses_image == false);
    CHECK(updated->materials[1].material_id == 12);
    CHECK(updated->materials[1].uses_image == true);
    CHECK(updated->materials[1].resource_fingerprint == 0xABCDEFu);
    CHECK(updated->cpu_bytes == 64);
    CHECK(updated->cpu_soft_bytes == 128);
    CHECK(updated->cpu_hard_bytes == 256);
    CHECK(updated->gpu_bytes == 32);
    CHECK(updated->gpu_soft_bytes == 96);
    CHECK(updated->gpu_hard_bytes == 192);

    auto cleared = Diagnostics::ClearTargetError(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(cleared);

    auto clearedValue = read_value<std::string>(fx.space, common + "/lastError");
    REQUIRE(clearedValue);
    CHECK(clearedValue->empty());

    auto afterClear = Diagnostics::ReadTargetMetrics(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(afterClear);
    CHECK(afterClear->last_error.empty());
    CHECK(afterClear->last_error_code == 0);
    CHECK(afterClear->last_error_revision == 0);
    CHECK(afterClear->last_error_severity == PathSpaceError::Severity::Info);
    CHECK(afterClear->last_error_timestamp_ns == 0);
    CHECK(afterClear->last_error_detail.empty());

    PathWindowPresentStats writeStats{};
    writeStats.presented = true;
    writeStats.buffered_frame_consumed = true;
    writeStats.used_progressive = true;
    writeStats.used_metal_texture = true;
    writeStats.wait_budget_ms = 7.5;
    writeStats.present_ms = 8.75;
    writeStats.gpu_encode_ms = 4.5;
    writeStats.gpu_present_ms = 5.25;
    writeStats.frame_age_ms = 3.0;
    writeStats.frame_age_frames = 2;
    writeStats.stale = true;
    writeStats.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    writeStats.progressive_tiles_copied = 4;
    writeStats.progressive_rects_coalesced = 3;
    writeStats.progressive_skip_seq_odd = 1;
    writeStats.progressive_recopy_after_seq_change = 2;
    writeStats.frame.frame_index = 21;
    writeStats.frame.revision = 9;
    writeStats.frame.render_ms = 6.25;
    writeStats.backend_kind = "Metal2D";
    writeStats.error = "post-write-error";

    PathWindowPresentPolicy writePolicy{};
    writePolicy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    writePolicy.staleness_budget = std::chrono::milliseconds{12};
    writePolicy.staleness_budget_ms_value = 12.0;
    writePolicy.frame_timeout = std::chrono::milliseconds{24};
    writePolicy.frame_timeout_ms_value = 24.0;
    writePolicy.max_age_frames = 3;
    writePolicy.auto_render_on_present = false;
    writePolicy.vsync_align = false;
    writePolicy.capture_framebuffer = true;

    auto write = Diagnostics::WritePresentMetrics(fx.space,
                                                  ConcretePathView{targetBase->getPath()},
                                                  writeStats,
                                                  writePolicy);
    REQUIRE(write);

    auto writeResidency = Diagnostics::WriteResidencyMetrics(fx.space,
                                                            ConcretePathView{targetBase->getPath()},
                                                            /*cpu_bytes*/ 512,
                                                            /*gpu_bytes*/ 1024,
                                                            /*cpu_soft_bytes*/ 384,
                                                            /*cpu_hard_bytes*/ 768,
                                                            /*gpu_soft_bytes*/ 2048,
                                                            /*gpu_hard_bytes*/ 4096);
    REQUIRE(writeResidency);

    auto afterWrite = Diagnostics::ReadTargetMetrics(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(afterWrite);
    CHECK(afterWrite->frame_index == 21);
    CHECK(afterWrite->revision == 9);
    CHECK(afterWrite->render_ms == doctest::Approx(6.25));
    CHECK(afterWrite->present_ms == doctest::Approx(8.75));
    CHECK(afterWrite->gpu_encode_ms == doctest::Approx(4.5));
    CHECK(afterWrite->gpu_present_ms == doctest::Approx(5.25));
    CHECK_FALSE(afterWrite->last_present_skipped);
    CHECK(afterWrite->used_metal_texture);
    CHECK(afterWrite->backend_kind == "Metal2D");
    CHECK(afterWrite->last_error == "post-write-error");
    CHECK(afterWrite->last_error_code == 3000);
    CHECK(afterWrite->last_error_revision == 9);
    CHECK(afterWrite->last_error_severity == PathSpaceError::Severity::Recoverable);
    CHECK(afterWrite->last_error_timestamp_ns > 0);
    CHECK(afterWrite->last_error_detail.empty());
    CHECK(afterWrite->material_count == 2);
    REQUIRE(afterWrite->materials.size() == 2);
    CHECK(afterWrite->materials[0].material_id == 7);
    CHECK(afterWrite->materials[1].material_id == 12);
    CHECK(afterWrite->material_resource_count == expected_resources.size());
    REQUIRE(afterWrite->material_resources.size() == expected_resources.size());
    CHECK(afterWrite->cpu_soft_budget_ratio == doctest::Approx(static_cast<double>(512) / 384.0));
    CHECK(afterWrite->cpu_hard_budget_ratio == doctest::Approx(static_cast<double>(512) / 768.0));
    CHECK(afterWrite->gpu_soft_budget_ratio == doctest::Approx(static_cast<double>(1024) / 2048.0));
    CHECK(afterWrite->gpu_hard_budget_ratio == doctest::Approx(static_cast<double>(1024) / 4096.0));
    CHECK(afterWrite->cpu_soft_exceeded);
    CHECK_FALSE(afterWrite->cpu_hard_exceeded);
    CHECK_FALSE(afterWrite->gpu_soft_exceeded);
    CHECK_FALSE(afterWrite->gpu_hard_exceeded);
    CHECK(afterWrite->cpu_residency_status == "soft");
    CHECK(afterWrite->gpu_residency_status == "ok");
    CHECK(afterWrite->residency_overall_status == "soft");
    CHECK(afterWrite->material_resources.front().fingerprint == expected_resources.front().fingerprint);
    CHECK(afterWrite->material_resources.front().gpu_bytes == expected_resources.front().gpu_bytes);
    CHECK(afterWrite->cpu_bytes == 512);
    CHECK(afterWrite->cpu_soft_bytes == 384);
    CHECK(afterWrite->cpu_hard_bytes == 768);
    CHECK(afterWrite->gpu_bytes == 1024);
    CHECK(afterWrite->gpu_soft_bytes == 2048);
    CHECK(afterWrite->gpu_hard_bytes == 4096);

    auto staleFlag = read_value<bool>(fx.space, common + "/stale");
    REQUIRE(staleFlag);
    CHECK(*staleFlag);

    auto modeString = read_value<std::string>(fx.space, common + "/presentMode");
    REQUIRE(modeString);
    CHECK(*modeString == "AlwaysLatestComplete");

    auto autoRender = read_value<bool>(fx.space, common + "/autoRenderOnPresent");
    REQUIRE(autoRender);
    CHECK_FALSE(*autoRender);

    auto vsyncAlign = read_value<bool>(fx.space, common + "/vsyncAlign");
    REQUIRE(vsyncAlign);
    CHECK_FALSE(*vsyncAlign);

    auto stalenessMs = read_value<double>(fx.space, common + "/stalenessBudgetMs");
    REQUIRE(stalenessMs);
    CHECK(*stalenessMs == doctest::Approx(12.0));

    auto frameTimeoutMs = read_value<double>(fx.space, common + "/frameTimeoutMs");
    REQUIRE(frameTimeoutMs);
    CHECK(*frameTimeoutMs == doctest::Approx(24.0));
}

TEST_CASE("Diagnostics::WriteResidencyMetrics handles zero limits without alerts") {
    BuildersFixture fx;
    auto targetPath = SP::ConcretePathString{std::string(fx.app_root.getPath()) + "/renderers/test/targets/surfaces/zero"};

    auto status = Diagnostics::WriteResidencyMetrics(fx.space,
                                                     SP::ConcretePathStringView{targetPath.getPath()},
                                                     /*cpu_bytes*/128,
                                                     /*gpu_bytes*/64,
                                                     /*cpu_soft_bytes*/0,
                                                     /*cpu_hard_bytes*/0,
                                                     /*gpu_soft_bytes*/0,
                                                     /*gpu_hard_bytes*/0);
    REQUIRE(status);

    auto metrics = Diagnostics::ReadTargetMetrics(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(metrics);
    CHECK(metrics->cpu_bytes == 128);
    CHECK(metrics->gpu_bytes == 64);
    CHECK(metrics->cpu_soft_budget_ratio == doctest::Approx(0.0));
    CHECK(metrics->cpu_hard_budget_ratio == doctest::Approx(0.0));
    CHECK(metrics->gpu_soft_budget_ratio == doctest::Approx(0.0));
    CHECK(metrics->gpu_hard_budget_ratio == doctest::Approx(0.0));
    CHECK_FALSE(metrics->cpu_soft_exceeded);
    CHECK_FALSE(metrics->cpu_hard_exceeded);
    CHECK_FALSE(metrics->gpu_soft_exceeded);
    CHECK_FALSE(metrics->gpu_hard_exceeded);
    CHECK(metrics->cpu_residency_status == "ok");
    CHECK(metrics->gpu_residency_status == "ok");
    CHECK(metrics->residency_overall_status == "ok");
}

TEST_CASE("Renderer::RenderHtml writes DOM outputs for html targets") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "html_renderer", .kind = RendererKind::Software2D, .description = "HTML" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SceneParams sceneParams{ .name = "scene_html_dom", .description = "html dom" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);
    publish_minimal_scene(fx, *scene);

    HtmlTargetParams targetParams{};
    targetParams.name = "preview";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    if (!render_html.has_value()) {
        auto const& err = render_html.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("RenderHtml failed"));
        auto diagValue = fx.space.read<PathSpaceError>(htmlBase + "/diagnostics/errors/live");
        if (diagValue) {
            CAPTURE(diagValue->message);
            CAPTURE(diagValue->detail);
        } else {
            CAPTURE(static_cast<int>(diagValue.error().code));
            CAPTURE(diagValue.error().message.value_or("diagnostics read failed"));
        }
    }
    REQUIRE(render_html.has_value());

    auto dom = read_value<std::string>(fx.space, htmlBase + "/dom");
    REQUIRE(dom);
    CHECK_FALSE(dom->empty());
    auto css = read_value<std::string>(fx.space, htmlBase + "/css");
    REQUIRE(css);
    CHECK_FALSE(css->empty());
    auto usedCanvas = read_value<bool>(fx.space, htmlBase + "/usedCanvasFallback");
    REQUIRE(usedCanvas);
    CHECK_FALSE(*usedCanvas);
    auto assets = read_value<std::vector<Asset>>(fx.space, htmlBase + "/assets");
    REQUIRE(assets);
    if (!assets->empty()) {
        CHECK(assets->front().logical_path.find("images/") == 0);
        CHECK(assets->front().mime_type != "application/vnd.pathspace.image+ref");
        CHECK_FALSE(assets->front().bytes.empty());
    }
}

TEST_CASE("Renderer::RenderHtml falls back to canvas when DOM budget exceeded") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "html_renderer_canvas", .kind = RendererKind::Software2D, .description = "HTML" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SceneParams sceneParams{ .name = "scene_html_canvas", .description = "html canvas" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);
    publish_minimal_scene(fx, *scene);

    HtmlTargetParams targetParams{};
    targetParams.name = "preview_canvas";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    targetParams.desc.max_dom_nodes = 0;
    targetParams.desc.prefer_dom = false;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    if (!render_html.has_value()) {
        auto const& err = render_html.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("RenderHtml failed"));
        auto diagValue = fx.space.read<PathSpaceError>(htmlBase + "/diagnostics/errors/live");
        if (diagValue) {
            CAPTURE(diagValue->message);
            CAPTURE(diagValue->detail);
        } else {
            CAPTURE(static_cast<int>(diagValue.error().code));
            CAPTURE(diagValue.error().message.value_or("diagnostics read failed"));
        }
    }
    REQUIRE(render_html.has_value());
    auto usedCanvas = read_value<bool>(fx.space, htmlBase + "/usedCanvasFallback");
    REQUIRE(usedCanvas);
    CHECK(*usedCanvas);
    auto commands = read_value<std::string>(fx.space, htmlBase + "/commands");
    REQUIRE(commands);
    CHECK_FALSE(commands->empty());
    auto dom = read_value<std::string>(fx.space, htmlBase + "/dom");
    REQUIRE(dom);
    CHECK(dom->empty());
}

TEST_CASE("Renderer::RenderHtml writes DOM outputs for html targets") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "html_renderer", .kind = RendererKind::Software2D, .description = "HTML" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SceneParams sceneParams{ .name = "scene_html_dom", .description = "html dom" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);
    publish_minimal_scene(fx, *scene);

    HtmlTargetParams targetParams{};
    targetParams.name = "preview";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    if (!render_html.has_value()) {
        auto const& err = render_html.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("RenderHtml failed"));
        auto diagValue = fx.space.read<PathSpaceError>(htmlBase + "/diagnostics/errors/live");
        if (diagValue) {
            CAPTURE(diagValue->message);
            CAPTURE(diagValue->detail);
        } else {
            CAPTURE(static_cast<int>(diagValue.error().code));
            CAPTURE(diagValue.error().message.value_or("diagnostics read failed"));
        }
    }
    REQUIRE(render_html.has_value());
    auto dom = read_value<std::string>(fx.space, htmlBase + "/dom");
    REQUIRE(dom);
    CHECK_FALSE(dom->empty());
    auto css = read_value<std::string>(fx.space, htmlBase + "/css");
    REQUIRE(css);
    CHECK_FALSE(css->empty());
    auto usedCanvas = read_value<bool>(fx.space, htmlBase + "/usedCanvasFallback");
    REQUIRE(usedCanvas);
    CHECK_FALSE(*usedCanvas);
    auto assets = read_value<std::vector<Asset>>(fx.space, htmlBase + "/assets");
    REQUIRE(assets);
    if (!assets->empty()) {
        CHECK(assets->front().logical_path.find("images/") == 0);
        CHECK(assets->front().mime_type != "application/vnd.pathspace.image+ref");
        CHECK_FALSE(assets->front().bytes.empty());
    }
}

TEST_CASE("Widgets::CreateButton publishes snapshot and state") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "primary";
    params.label = "Primary";
    params.style.width = 180.0f;
    params.style.height = 44.0f;

    auto created = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(created);

    auto state = read_value<Widgets::ButtonState>(fx.space,
                                                  std::string(created->state.getPath()));
    REQUIRE(state);
    CHECK(state->enabled);
    CHECK_FALSE(state->pressed);
    CHECK_FALSE(state->hovered);

    auto label = read_value<std::string>(fx.space, std::string(created->label.getPath()));
    REQUIRE(label);
    CHECK(*label == params.label);

    auto style = read_value<Widgets::ButtonStyle>(fx.space,
                                                 std::string(created->root.getPath()) + "/meta/style");
    REQUIRE(style);
    CHECK(style->width == doctest::Approx(params.style.width));
    CHECK(style->height == doctest::Approx(params.style.height));
    CHECK(style->corner_radius == doctest::Approx(params.style.corner_radius));
    CHECK(style->typography.font_size == doctest::Approx(28.0f));
    CHECK(style->typography.line_height == doctest::Approx(28.0f));

    CHECK(created->states.idle.getPath()
          == "/system/applications/test_app/scenes/widgets/primary/states/idle");
    CHECK(created->states.hover.getPath()
          == "/system/applications/test_app/scenes/widgets/primary/states/hover");
    CHECK(created->states.pressed.getPath()
          == "/system/applications/test_app/scenes/widgets/primary/states/pressed");
    CHECK(created->states.disabled.getPath()
          == "/system/applications/test_app/scenes/widgets/primary/states/disabled");

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision > 0);

    auto read_scene_bucket = [&](SP::UI::Builders::ScenePath const& scene) {
        auto stateRevision = Scene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(stateRevision);
        auto base = std::string(scene.getPath()) + "/builds/" + format_revision(stateRevision->revision);
        auto bucket = SceneSnapshotBuilder::decode_bucket(fx.space, base);
        REQUIRE(bucket);
        REQUIRE_FALSE(bucket->command_kinds.empty());
        auto kind = static_cast<DrawCommandKind>(bucket->command_kinds.front());
        if (kind == DrawCommandKind::RoundedRect) {
            REQUIRE(bucket->command_payload.size() >= sizeof(RoundedRectCommand));
            RoundedRectCommand rect{};
            std::memcpy(&rect, bucket->command_payload.data(), sizeof(RoundedRectCommand));
            return rect;
        }
        REQUIRE(kind == DrawCommandKind::Rect);
        REQUIRE(bucket->command_payload.size() >= sizeof(RectCommand));
        RoundedRectCommand rect{};
        RectCommand legacy{};
        std::memcpy(&legacy, bucket->command_payload.data(), sizeof(RectCommand));
        rect.min_x = legacy.min_x;
        rect.min_y = legacy.min_y;
        rect.max_x = legacy.max_x;
        rect.max_y = legacy.max_y;
        rect.radius_top_left = rect.radius_top_right = rect.radius_bottom_left = rect.radius_bottom_right = 0.0f;
        rect.color = legacy.color;
        return rect;
    };

    auto idleRect = read_scene_bucket(created->states.idle);
    auto hoverRect = read_scene_bucket(created->states.hover);
    auto pressedRect = read_scene_bucket(created->states.pressed);
    auto disabledRect = read_scene_bucket(created->states.disabled);

    CHECK(hoverRect.color[0] > idleRect.color[0]);
    CHECK(pressedRect.color[0] < idleRect.color[0]);
    CHECK(disabledRect.color[3] < idleRect.color[3]);

    Widgets::ButtonState pressed_state = *state;
    pressed_state.pressed = true;
    auto changed = Widgets::UpdateButtonState(fx.space, *created, pressed_state);
    REQUIRE(changed);
    CHECK(*changed);

    auto updatedRevision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto updated = read_value<Widgets::ButtonState>(fx.space,
                                                    std::string(created->state.getPath()));
    REQUIRE(updated);
    CHECK(updated->pressed);

    auto unchanged = Widgets::UpdateButtonState(fx.space, *created, pressed_state);
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
}

TEST_CASE("Widgets::WidgetTheme hot swap repaints button scenes and marks dirty") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "button_hot_swap";
    params.label = "Theme Swap";

    auto defaultTheme = Widgets::MakeDefaultWidgetTheme();
    Widgets::ApplyTheme(defaultTheme, params);

    auto createdResult = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(createdResult);
    auto const created = *createdResult;

    auto read_button_color = [&](ScenePath const& scene) -> std::array<float, 4> {
        auto bucket = decode_state_bucket(fx, scene);
        REQUIRE_FALSE(bucket.command_kinds.empty());
        auto kind = static_cast<DrawCommandKind>(bucket.command_kinds.front());
        std::array<float, 4> color{};
        if (kind == DrawCommandKind::RoundedRect) {
            REQUIRE(bucket.command_payload.size() >= sizeof(RoundedRectCommand));
            RoundedRectCommand rect{};
            std::memcpy(&rect, bucket.command_payload.data(), sizeof(RoundedRectCommand));
            color = rect.color;
        } else {
            REQUIRE(kind == DrawCommandKind::Rect);
            REQUIRE(bucket.command_payload.size() >= sizeof(RectCommand));
            RectCommand rect{};
            std::memcpy(&rect, bucket.command_payload.data(), sizeof(RectCommand));
            color = rect.color;
        }
        return color;
    };

    auto read_revision = [&](ScenePath const& scene) -> std::uint64_t {
        auto revision = Scene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(revision);
        return revision->revision;
    };

    auto stylePath = std::string(created.root.getPath()) + "/meta/style";
    auto defaultStyle = read_value<Widgets::ButtonStyle>(fx.space, stylePath);
    REQUIRE(defaultStyle);
    CHECK(defaultStyle->background_color[0] == doctest::Approx(defaultTheme.button.background_color[0]));
    CHECK(defaultStyle->text_color[0] == doctest::Approx(defaultTheme.button.text_color[0]));
    CHECK(defaultStyle->typography.font_size == doctest::Approx(defaultTheme.button.typography.font_size));

    auto initialSceneRevision = read_revision(created.scene);
    auto initialIdleRevision = read_revision(created.states.idle);
    auto initialHoverRevision = read_revision(created.states.hover);
    auto initialPressedRevision = read_revision(created.states.pressed);
    auto initialDisabledRevision = read_revision(created.states.disabled);

    auto initialIdleColor = read_button_color(created.states.idle);
    CHECK(initialIdleColor[0] == doctest::Approx(defaultTheme.button.background_color[0]));

    auto drain_dirty_queue = [&](ScenePath const& scene) {
        while (true) {
            auto event = Scene::TakeDirtyEvent(fx.space, scene, std::chrono::milliseconds{1});
            if (event) {
                continue;
            }
            auto code = event.error().code;
            bool const expected_empty = code == Error::Code::Timeout
                                        || code == Error::Code::NoObjectFound
                                        || code == Error::Code::NoSuchPath;
            if (!expected_empty) {
                FAIL("Unexpected dirty queue error: " << static_cast<int>(code));
            }
            break;
        }
    };

    drain_dirty_queue(created.scene);

    auto sunsetTheme = Widgets::MakeSunsetWidgetTheme();
    Widgets::ApplyTheme(sunsetTheme, params);

    auto updatedResult = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(updatedResult);
    auto const updated = *updatedResult;
    CHECK(updated.scene.getPath() == created.scene.getPath());

    auto updatedStyle = read_value<Widgets::ButtonStyle>(fx.space, stylePath);
    REQUIRE(updatedStyle);
    CHECK(updatedStyle->background_color[0] == doctest::Approx(sunsetTheme.button.background_color[0]));
    CHECK(updatedStyle->text_color[0] == doctest::Approx(sunsetTheme.button.text_color[0]));
    CHECK(updatedStyle->typography.font_size == doctest::Approx(sunsetTheme.button.typography.font_size));
    CHECK(updatedStyle->typography.line_height == doctest::Approx(sunsetTheme.button.typography.line_height));

    auto updatedIdleColor = read_button_color(updated.states.idle);
    CHECK(updatedIdleColor[0] == doctest::Approx(sunsetTheme.button.background_color[0]));
    CHECK(updatedIdleColor[0] != doctest::Approx(initialIdleColor[0]));

    auto updatedSceneRevision = read_revision(updated.scene);
    CHECK(updatedSceneRevision > initialSceneRevision);
    CHECK(read_revision(updated.states.idle) > initialIdleRevision);
    CHECK(read_revision(updated.states.hover) > initialHoverRevision);
    CHECK(read_revision(updated.states.pressed) > initialPressedRevision);
    CHECK(read_revision(updated.states.disabled) > initialDisabledRevision);

    drain_dirty_queue(updated.scene);
}

TEST_CASE("Widgets::CreateToggle publishes snapshot and state") {
    BuildersFixture fx;

    Widgets::ToggleParams params{};
    params.name = "toggle_primary";
    params.style.width = 60.0f;
    params.style.height = 32.0f;
    params.style.track_on_color = {0.2f, 0.6f, 0.3f, 1.0f};

    auto created = Widgets::CreateToggle(fx.space, fx.root_view(), params);
    REQUIRE(created);

    auto state = read_value<Widgets::ToggleState>(fx.space,
                                                  std::string(created->state.getPath()));
    REQUIRE(state);
    CHECK(state->enabled);
    CHECK_FALSE(state->hovered);
    CHECK_FALSE(state->checked);

    auto style = read_value<Widgets::ToggleStyle>(fx.space,
                                                 std::string(created->root.getPath()) + "/meta/style");
    REQUIRE(style);
    CHECK(style->width == doctest::Approx(params.style.width));
    CHECK(style->height == doctest::Approx(params.style.height));
    CHECK(style->track_on_color[0] == doctest::Approx(params.style.track_on_color[0]));

    CHECK(created->states.idle.getPath()
          == "/system/applications/test_app/scenes/widgets/toggle_primary/states/idle");
    CHECK(created->states.hover.getPath()
          == "/system/applications/test_app/scenes/widgets/toggle_primary/states/hover");
    CHECK(created->states.pressed.getPath()
          == "/system/applications/test_app/scenes/widgets/toggle_primary/states/pressed");
    CHECK(created->states.disabled.getPath()
          == "/system/applications/test_app/scenes/widgets/toggle_primary/states/disabled");

    auto ensure_state_scene = [&](SP::UI::Builders::ScenePath const& scene) {
        auto rev = Scene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision > 0);

    Widgets::ToggleState toggled = *state;
    toggled.checked = true;
    auto toggle_changed = Widgets::UpdateToggleState(fx.space, *created, toggled);
    REQUIRE(toggle_changed);
    CHECK(*toggle_changed);

    auto toggle_state = read_value<Widgets::ToggleState>(fx.space,
                                                         std::string(created->state.getPath()));
    REQUIRE(toggle_state);
    CHECK(toggle_state->checked);

    auto updatedRevision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto toggle_unchanged = Widgets::UpdateToggleState(fx.space, *created, toggled);
    REQUIRE(toggle_unchanged);
    CHECK_FALSE(*toggle_unchanged);
}

TEST_CASE("Widgets::CreateSlider publishes snapshot and state") {
    BuildersFixture fx;

    Widgets::SliderParams params{};
    params.name = "slider_primary";
    params.minimum = -1.0f;
    params.maximum = 1.0f;
    params.value = 0.25f;
    params.step = 0.25f;
    params.style.width = 320.0f;
    params.style.height = 36.0f;
    params.style.track_height = 8.0f;
    params.style.thumb_radius = 14.0f;

    auto created = Widgets::CreateSlider(fx.space, fx.root_view(), params);
    REQUIRE(created);

    auto state = read_value<Widgets::SliderState>(fx.space,
                                                 std::string(created->state.getPath()));
    REQUIRE(state);
    CHECK(state->enabled);
    CHECK_FALSE(state->hovered);
    CHECK_FALSE(state->dragging);
    CHECK(state->value == doctest::Approx(0.25f));

    auto style = read_value<Widgets::SliderStyle>(fx.space,
                                                 std::string(created->root.getPath()) + "/meta/style");
    REQUIRE(style);
    CHECK(style->width == doctest::Approx(320.0f));
    CHECK(style->height == doctest::Approx(36.0f));
    CHECK(style->track_height == doctest::Approx(8.0f));
    CHECK(style->thumb_radius == doctest::Approx(14.0f));
    CHECK(style->label_color[0] == doctest::Approx(params.style.label_color[0]));
    CHECK(style->label_typography.font_size == doctest::Approx(24.0f));

    auto range = read_value<Widgets::SliderRange>(fx.space,
                                                  std::string(created->range.getPath()));
    REQUIRE(range);
    CHECK(range->minimum == doctest::Approx(-1.0f));
    CHECK(range->maximum == doctest::Approx(1.0f));
    CHECK(range->step == doctest::Approx(0.25f));

    CHECK(created->states.idle.getPath()
          == "/system/applications/test_app/scenes/widgets/slider_primary/states/idle");
    CHECK(created->states.hover.getPath()
          == "/system/applications/test_app/scenes/widgets/slider_primary/states/hover");
    CHECK(created->states.pressed.getPath()
          == "/system/applications/test_app/scenes/widgets/slider_primary/states/pressed");
    CHECK(created->states.disabled.getPath()
          == "/system/applications/test_app/scenes/widgets/slider_primary/states/disabled");

    auto ensure_state_scene = [&](SP::UI::Builders::ScenePath const& scene) {
        auto rev = Scene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision > 0);

    Widgets::SliderState dragged = *state;
    dragged.dragging = true;
    dragged.value = 0.63f;
    auto slider_changed = Widgets::UpdateSliderState(fx.space, *created, dragged);
    REQUIRE(slider_changed);
    CHECK(*slider_changed);

    auto updated = read_value<Widgets::SliderState>(fx.space,
                                                   std::string(created->state.getPath()));
    REQUIRE(updated);
    CHECK(updated->value == doctest::Approx(0.75f));
    CHECK(updated->dragging);

    auto updatedRevision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto slider_unchanged = Widgets::UpdateSliderState(fx.space, *created, *updated);
    REQUIRE(slider_unchanged);
    CHECK_FALSE(*slider_unchanged);
}

TEST_CASE("Widgets::Bindings::DispatchButton emits dirty hints and widget ops") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_button_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 128};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_button_surface", .desc = desc, .renderer = "renderers/bindings_button_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_button_surface");
    REQUIRE(target);

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "primary_button";
    buttonParams.label = "Primary";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()});
    REQUIRE(binding);

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 12.0f;
    pointer.scene_y = 6.0f;
    pointer.inside = true;

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    Widgets::ButtonState hovered{};
    hovered.hovered = true;

    auto hoverEnter = WidgetBindings::DispatchButton(fx.space,
                                                     *binding,
                                                     hovered,
                                                     WidgetBindings::WidgetOpKind::HoverEnter,
                                                     pointer);
    REQUIRE(hoverEnter);
    CHECK(*hoverEnter);

    auto hoverEnterEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(hoverEnterEvent);
    CHECK(hoverEnterEvent->reason == "widget/button");

    auto hoverEnterOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverEnterOp);
    CHECK(hoverEnterOp->kind == WidgetBindings::WidgetOpKind::HoverEnter);
    CHECK(hoverEnterOp->value == doctest::Approx(0.0f));

    Widgets::ButtonState pressed{};
    pressed.hovered = true;
    pressed.pressed = true;

    auto pressResult = WidgetBindings::DispatchButton(fx.space,
                                                      *binding,
                                                      pressed,
                                                      WidgetBindings::WidgetOpKind::Press,
                                                      pointer);
    REQUIRE(pressResult);
    CHECK(*pressResult);

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());
    auto const& hint = hints->front();
    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    auto tile = static_cast<float>(desc.progressive_tile_size_px);
    auto expected_width = std::ceil(buttonStyle->width / tile) * tile;
    auto expected_height = std::ceil(buttonStyle->height / tile) * tile;
    CHECK(hint.min_x == doctest::Approx(0.0f));
    CHECK(hint.min_y == doctest::Approx(0.0f));
    CHECK(hint.max_x == doctest::Approx(expected_width));
    CHECK(hint.max_y == doctest::Approx(expected_height));

    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(renderEvent);
    CHECK(renderEvent->reason == "widget/button");

    auto pressOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(pressOp);
    CHECK(pressOp->kind == WidgetBindings::WidgetOpKind::Press);
    CHECK(pressOp->pointer.inside);
    CHECK(pressOp->value == doctest::Approx(1.0f));
    CHECK(pressOp->widget_path == binding->widget.root.getPath());

    Widgets::ButtonState released = pressed;
    released.pressed = false;

    auto releaseResult = WidgetBindings::DispatchButton(fx.space,
                                                        *binding,
                                                        released,
                                                        WidgetBindings::WidgetOpKind::Release,
                                                        pointer);
    REQUIRE(releaseResult);
    CHECK(*releaseResult);

    auto releaseEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(releaseEvent);
    CHECK(releaseEvent->reason == "widget/button");

    auto releaseOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(releaseOp);
    CHECK(releaseOp->kind == WidgetBindings::WidgetOpKind::Release);
    CHECK(releaseOp->value == doctest::Approx(0.0f));
    CHECK(releaseOp->sequence > pressOp->sequence);

    auto hoverExit = WidgetBindings::DispatchButton(fx.space,
                                                    *binding,
                                                    released,
                                                    WidgetBindings::WidgetOpKind::HoverExit,
                                                    pointer);
    REQUIRE(hoverExit);
    CHECK_FALSE(*hoverExit);

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::HoverExit);

    auto noEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(noEvent);
    if (!noEvent) {
        CHECK((noEvent.error().code == Error::Code::NoObjectFound || noEvent.error().code == Error::Code::NoSuchPath));
    }

    Widgets::ButtonState disabled = released;
    disabled.enabled = false;

    auto disableResult = Widgets::UpdateButtonState(fx.space, *button, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(disableEvent);
    if (!disableEvent) {
        CHECK((disableEvent.error().code == Error::Code::NoObjectFound || disableEvent.error().code == Error::Code::NoSuchPath));
    }

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto disabledState = read_value<Widgets::ButtonState>(fx.space, std::string(button->state.getPath()));
    REQUIRE(disabledState);
    CHECK_FALSE(disabledState->enabled);
}

TEST_CASE("Widgets::Bindings::DispatchButton honors auto-render flag") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_button_manual_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 128};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_button_manual_surface", .desc = desc, .renderer = "renderers/bindings_button_manual_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_button_manual_surface");
    REQUIRE(target);

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "manual_button";
    buttonParams.label = "Manual";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       std::nullopt,
                                                       /*auto_render=*/false);
    REQUIRE(binding);

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 4.0f;
    pointer.scene_y = 3.0f;
    pointer.inside = true;

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    Widgets::ButtonState hover{};
    hover.hovered = true;

    auto hoverEnter = WidgetBindings::DispatchButton(fx.space,
                                                     *binding,
                                                     hover,
                                                     WidgetBindings::WidgetOpKind::HoverEnter,
                                                     pointer);
    REQUIRE(hoverEnter);
    CHECK(*hoverEnter);

    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(renderEvent);
    if (!renderEvent) {
        CHECK((renderEvent.error().code == Error::Code::NoObjectFound || renderEvent.error().code == Error::Code::NoSuchPath));
    }

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::HoverEnter);

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());
}

TEST_CASE("Widgets::Bindings::DispatchToggle handles hover/toggle/disable sequence") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_toggle_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {196, 96};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_toggle_surface", .desc = desc, .renderer = "renderers/bindings_toggle_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_toggle_surface");
    REQUIRE(target);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "primary_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto binding = WidgetBindings::CreateToggleBinding(fx.space,
                                                       fx.root_view(),
                                                       *toggle,
                                                       SP::ConcretePathStringView{target->getPath()});
    REQUIRE(binding);

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 18.0f;
    pointer.scene_y = 12.0f;
    pointer.inside = true;

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    Widgets::ToggleState hoverState{};
    hoverState.hovered = true;

    auto hoverEnter = WidgetBindings::DispatchToggle(fx.space,
                                                     *binding,
                                                     hoverState,
                                                     WidgetBindings::WidgetOpKind::HoverEnter,
                                                     pointer);
    REQUIRE(hoverEnter);
    CHECK(*hoverEnter);

    auto hoverEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(hoverEvent);
    CHECK(hoverEvent->reason == "widget/toggle");

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::HoverEnter);
    CHECK(hoverOp->value == doctest::Approx(0.0f));

    Widgets::ToggleState toggledState{};
    toggledState.hovered = true;
    toggledState.checked = true;

    auto toggleResult = WidgetBindings::DispatchToggle(fx.space,
                                                       *binding,
                                                       toggledState,
                                                       WidgetBindings::WidgetOpKind::Toggle,
                                                       pointer);
    REQUIRE(toggleResult);
    CHECK(*toggleResult);

    auto toggleEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(toggleEvent);
    CHECK(toggleEvent->reason == "widget/toggle");

    auto toggleOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(toggleOp);
    CHECK(toggleOp->kind == WidgetBindings::WidgetOpKind::Toggle);
    CHECK(toggleOp->value == doctest::Approx(1.0f));

    Widgets::ToggleState hoverExitState = toggledState;
    hoverExitState.hovered = false;

    auto hoverExit = WidgetBindings::DispatchToggle(fx.space,
                                                    *binding,
                                                    hoverExitState,
                                                    WidgetBindings::WidgetOpKind::HoverExit,
                                                    pointer);
    REQUIRE(hoverExit);
    CHECK(*hoverExit);

    auto hoverExitEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(hoverExitEvent);
    CHECK(hoverExitEvent->reason == "widget/toggle");

    auto hoverExitOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverExitOp);
    CHECK(hoverExitOp->kind == WidgetBindings::WidgetOpKind::HoverExit);
    CHECK(hoverExitOp->value == doctest::Approx(1.0f));

    Widgets::ToggleState disabledState = hoverExitState;
    disabledState.enabled = false;

    auto disableResult = Widgets::UpdateToggleState(fx.space, *toggle, disabledState);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(disableEvent);
    if (!disableEvent) {
        CHECK((disableEvent.error().code == Error::Code::NoObjectFound || disableEvent.error().code == Error::Code::NoSuchPath));
    }

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto storedState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(storedState);
    CHECK_FALSE(storedState->enabled);
    CHECK(storedState->checked);
}

TEST_CASE("Widgets dirty hints cover adjacent widget bindings") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_adjacent_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {192, 96};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_adjacent_surface", .desc = desc, .renderer = "renderers/bindings_adjacent_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_adjacent_surface");
    REQUIRE(target);

    Widgets::ButtonParams leftParams{};
    leftParams.name = "left_button";
    leftParams.label = "Left";
    leftParams.style.width = 96.0f;
    leftParams.style.height = 64.0f;
    auto left = Widgets::CreateButton(fx.space, fx.root_view(), leftParams);
    REQUIRE(left);

    Widgets::ButtonParams rightParams{};
    rightParams.name = "right_button";
    rightParams.label = "Right";
    rightParams.style.width = 96.0f;
    rightParams.style.height = 64.0f;
    auto right = Widgets::CreateButton(fx.space, fx.root_view(), rightParams);
    REQUIRE(right);

    DirtyRectHint leftHint{0.0f, 0.0f, 96.0f, 64.0f};
    DirtyRectHint rightHint{64.0f, 0.0f, 160.0f, 64.0f};

    auto leftBinding = WidgetBindings::CreateButtonBinding(fx.space,
                                                           fx.root_view(),
                                                           *left,
                                                           SP::ConcretePathStringView{target->getPath()},
                                                           leftHint);
    REQUIRE(leftBinding);

    auto rightBinding = WidgetBindings::CreateButtonBinding(fx.space,
                                                            fx.root_view(),
                                                            *right,
                                                            SP::ConcretePathStringView{target->getPath()},
                                                            rightHint);
    REQUIRE(rightBinding);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto hintsPath = std::string(target->getPath()) + "/hints/dirtyRects";

    auto preEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(preEvent);
    if (!preEvent) {
        CHECK((preEvent.error().code == Error::Code::NoObjectFound || preEvent.error().code == Error::Code::NoSuchPath));
    }

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 12.0f;
    pointer.scene_y = 8.0f;
    pointer.inside = true;

    Widgets::ButtonState hover{};
    hover.hovered = true;

    auto changed = WidgetBindings::DispatchButton(fx.space,
                                                  *leftBinding,
                                                  hover,
                                                  WidgetBindings::WidgetOpKind::HoverEnter,
                                                  pointer);
    REQUIRE(changed);
    CHECK(*changed);

    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(renderEvent);
    CHECK(renderEvent->reason == "widget/button");

    auto dirtyRects = fx.space.read<std::vector<DirtyRectHint>, std::string>(hintsPath);
    REQUIRE(dirtyRects);
    REQUIRE(dirtyRects->size() == 1);
    auto const& stored = dirtyRects->front();
    CHECK(stored.min_x == doctest::Approx(leftHint.min_x));
    CHECK(stored.min_y == doctest::Approx(leftHint.min_y));
    CHECK(stored.max_x == doctest::Approx(leftHint.max_x));
    CHECK(stored.max_y == doctest::Approx(leftHint.max_y));

    auto overlaps = [](DirtyRectHint const& a, DirtyRectHint const& b) {
        auto overlaps_axis = [](float min_a, float max_a, float min_b, float max_b) {
            return !(max_a <= min_b || min_a >= max_b);
        };
        return overlaps_axis(a.min_x, a.max_x, b.min_x, b.max_x)
            && overlaps_axis(a.min_y, a.max_y, b.min_y, b.max_y);
    };
    CHECK(overlaps(stored, rightHint));

    auto rightState = read_value<Widgets::ButtonState>(fx.space, std::string(rightBinding->widget.state.getPath()));
    REQUIRE(rightState);
    CHECK(rightState->enabled);
    CHECK_FALSE(rightState->hovered);
    CHECK_FALSE(rightState->pressed);

    auto leftState = read_value<Widgets::ButtonState>(fx.space, std::string(leftBinding->widget.state.getPath()));
    REQUIRE(leftState);
    CHECK(leftState->hovered);
    CHECK_FALSE(leftState->pressed);

    auto noExtraEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(noExtraEvent);
    if (!noExtraEvent) {
        CHECK((noExtraEvent.error().code == Error::Code::NoObjectFound || noExtraEvent.error().code == Error::Code::NoSuchPath));
    }
}

TEST_CASE("Widgets::Bindings::DispatchSlider clamps values and schedules ops") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_slider_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 192};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_slider_surface", .desc = desc, .renderer = "renderers/bindings_slider_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_slider_surface");
    REQUIRE(target);

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "volume";
    sliderParams.maximum = 1.0f;
    sliderParams.value = 0.25f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto binding = WidgetBindings::CreateSliderBinding(fx.space,
                                                       fx.root_view(),
                                                       *slider,
                                                       SP::ConcretePathStringView{target->getPath()});
    REQUIRE(binding);

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 120.0f;
    pointer.scene_y = 12.0f;
    pointer.primary = true;

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    Widgets::SliderState beginState{};
    beginState.enabled = true;
    beginState.dragging = true;
    beginState.value = 0.15f;

    auto beginResult = WidgetBindings::DispatchSlider(fx.space,
                                                      *binding,
                                                      beginState,
                                                      WidgetBindings::WidgetOpKind::SliderBegin,
                                                      pointer);
    REQUIRE(beginResult);
    CHECK(*beginResult);

    auto beginEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(beginEvent);
    CHECK(beginEvent->reason == "widget/slider");

    auto beginOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(beginOp);
    CHECK(beginOp->kind == WidgetBindings::WidgetOpKind::SliderBegin);
    CHECK(beginOp->value == doctest::Approx(0.15f));

    Widgets::SliderState dragState{};
    dragState.enabled = true;
    dragState.dragging = true;
    dragState.value = 2.0f;

    auto updateResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *binding,
                                                       dragState,
                                                       WidgetBindings::WidgetOpKind::SliderUpdate,
                                                       pointer);
    REQUIRE(updateResult);
    CHECK(*updateResult);

    auto updateEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(updateEvent);
    CHECK(updateEvent->reason == "widget/slider");

    auto updateOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(updateOp);
    CHECK(updateOp->kind == WidgetBindings::WidgetOpKind::SliderUpdate);
    CHECK(updateOp->value == doctest::Approx(1.0f));

    Widgets::SliderState commitState{};
    commitState.enabled = true;
    commitState.dragging = false;
    commitState.value = 0.6f;

    auto commitResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *binding,
                                                       commitState,
                                                       WidgetBindings::WidgetOpKind::SliderCommit,
                                                       pointer);
    REQUIRE(commitResult);
    CHECK(*commitResult);

    auto commitEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(commitEvent);
    CHECK(commitEvent->reason == "widget/slider");

    auto commitOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(commitOp);
    CHECK(commitOp->kind == WidgetBindings::WidgetOpKind::SliderCommit);
    CHECK(commitOp->value == doctest::Approx(0.6f));

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());

    auto noExtraEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(noExtraEvent);
    if (!noExtraEvent) {
        CHECK((noExtraEvent.error().code == Error::Code::NoObjectFound || noExtraEvent.error().code == Error::Code::NoSuchPath));
    }

    Widgets::SliderState disabled{};
    disabled.enabled = false;
    disabled.value = 0.6f;

    auto disableResult = Widgets::UpdateSliderState(fx.space, *slider, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(disableEvent);
    if (!disableEvent) {
        CHECK((disableEvent.error().code == Error::Code::NoObjectFound || disableEvent.error().code == Error::Code::NoSuchPath));
    }

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto storedState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(storedState);
    CHECK_FALSE(storedState->enabled);
}

TEST_CASE("Widgets::CreateList publishes snapshot and metadata") {
    BuildersFixture fx;

    Widgets::ListParams listParams{};
    listParams.name = "inventory";
    listParams.items = {
        Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
        Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
        Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = false},
    };
    listParams.style.width = 220.0f;
    listParams.style.item_height = 40.0f;

    auto created = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(created);

    auto state = read_value<Widgets::ListState>(fx.space, created->state.getPath());
    REQUIRE(state);
    CHECK(state->selected_index == 0);
    CHECK(state->hovered_index == -1);

    auto storedItems = read_value<std::vector<Widgets::ListItem>>(fx.space, created->items.getPath());
    REQUIRE(storedItems);
    REQUIRE(storedItems->size() == 3);
    CHECK((*storedItems)[1].label == "Ether");
    CHECK_FALSE((*storedItems)[2].enabled);

    auto stylePath = std::string(created->root.getPath()) + "/meta/style";
    auto storedStyle = read_value<Widgets::ListStyle>(fx.space, stylePath);
    REQUIRE(storedStyle);
    CHECK(storedStyle->width == doctest::Approx(220.0f));
    CHECK(storedStyle->item_height == doctest::Approx(40.0f));
    CHECK(storedStyle->item_text_color[0] == doctest::Approx(listParams.style.item_text_color[0]));
    CHECK(storedStyle->item_typography.font_size == doctest::Approx(21.0f));

    CHECK(created->states.idle.getPath()
          == "/system/applications/test_app/scenes/widgets/inventory/states/idle");
    CHECK(created->states.hover.getPath()
          == "/system/applications/test_app/scenes/widgets/inventory/states/hover");
    CHECK(created->states.pressed.getPath()
          == "/system/applications/test_app/scenes/widgets/inventory/states/pressed");
    CHECK(created->states.disabled.getPath()
          == "/system/applications/test_app/scenes/widgets/inventory/states/disabled");

    auto ensure_state_scene = [&](SP::UI::Builders::ScenePath const& scene) {
        auto rev = Scene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::CreateTree publishes snapshot and metadata") {
    BuildersFixture fx;

    Widgets::TreeParams treeParams{};
    treeParams.name = "filesystem";
    treeParams.nodes = {
        Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "docs", .parent_id = "root", .label = "Docs", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
        Widgets::TreeNode{.id = "tests", .parent_id = "src", .label = "Tests", .enabled = true, .expandable = false, .loaded = false},
    };

    auto created = Widgets::CreateTree(fx.space, fx.root_view(), treeParams);
    REQUIRE(created);

    auto storedNodes = read_value<std::vector<Widgets::TreeNode>>(fx.space, created->nodes.getPath());
    REQUIRE(storedNodes);
    CHECK(storedNodes->size() == 4);
    CHECK((*storedNodes)[0].loaded);
    CHECK((*storedNodes)[2].expandable);

    auto state = read_value<Widgets::TreeState>(fx.space, created->state.getPath());
    REQUIRE(state);
    CHECK(state->expanded_ids.empty());
    CHECK(state->hovered_id.empty());

    auto kindPath = std::string(created->root.getPath()) + "/meta/kind";
    auto storedKind = read_value<std::string>(fx.space, kindPath);
    REQUIRE(storedKind);
    CHECK(*storedKind == "tree");

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::UpdateTreeState toggles expansion and clamps state") {
    BuildersFixture fx;

    Widgets::TreeParams treeParams{};
    treeParams.name = "project";
    treeParams.nodes = {
        Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
        Widgets::TreeNode{.id = "include", .parent_id = "root", .label = "Include", .enabled = false, .expandable = false, .loaded = false},
    };

    auto tree = Widgets::CreateTree(fx.space, fx.root_view(), treeParams);
    REQUIRE(tree);

    Widgets::TreeState desired{};
    desired.enabled = true;
    desired.hovered_id = "include"; // disabled, should clear
    desired.selected_id = "src";
    desired.expanded_ids = {"root"};
    desired.loading_ids = {"src"};
    desired.scroll_offset = 100.0f;

    auto changed = Widgets::UpdateTreeState(fx.space, *tree, desired);
    REQUIRE(changed);
    CHECK(*changed);

    auto updated = read_value<Widgets::TreeState>(fx.space, tree->state.getPath());
    REQUIRE(updated);
    CHECK(updated->selected_id == "src");
    CHECK(updated->hovered_id.empty());
    CHECK(std::find(updated->expanded_ids.begin(), updated->expanded_ids.end(), "root")
          != updated->expanded_ids.end());

    Widgets::TreeState collapse{};
    collapse.enabled = true;
    collapse.selected_id = "src";
    collapse.expanded_ids = {};
    auto collapsed = Widgets::UpdateTreeState(fx.space, *tree, collapse);
    REQUIRE(collapsed);
    CHECK(*collapsed);

    auto collapsedState = read_value<Widgets::TreeState>(fx.space, tree->state.getPath());
    REQUIRE(collapsedState);
    CHECK(collapsedState->expanded_ids.empty());
}

TEST_CASE("Widgets::Bindings::DispatchTree enqueues ops and schedules renders") {
    BuildersFixture fx;

    Widgets::TreeParams treeParams{};
    treeParams.name = "bindings_tree";
    treeParams.nodes = {
        Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
        Widgets::TreeNode{.id = "docs", .parent_id = "root", .label = "Docs", .enabled = true, .expandable = false, .loaded = false},
    };

    auto tree = Widgets::CreateTree(fx.space, fx.root_view(), treeParams);
    REQUIRE(tree);

    RendererParams rendererParams{ .name = "bindings_tree_renderer",
                                   .kind = RendererKind::Software2D,
                                   .description = "Tree renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px = {320, 240};

    SurfaceParams surfaceParams{ .name = "bindings_tree_surface",
                                 .desc = surfaceDesc,
                                 .renderer = "renderers/bindings_tree_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, tree->scene));

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_tree_surface");
    REQUIRE(target);

    auto binding = WidgetBindings::CreateTreeBinding(fx.space,
                                                     fx.root_view(),
                                                     *tree,
                                                     SP::ConcretePathStringView{target->getPath()},
                                                     std::nullopt,
                                                     true);
    REQUIRE(binding);

    auto currentState = fx.space.read<Widgets::TreeState, std::string>(tree->state.getPath());
    REQUIRE(currentState);

    auto toggle = WidgetBindings::DispatchTree(fx.space,
                                               *binding,
                                               *currentState,
                                               WidgetBindings::WidgetOpKind::TreeToggle,
                                               "src",
                                               WidgetBindings::PointerInfo{},
                                               0.0f);
    if (!toggle) {
        auto err = toggle.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message);
    }
    REQUIRE(toggle);
    CHECK(*toggle);

    auto updatedState = fx.space.read<Widgets::TreeState, std::string>(tree->state.getPath());
    REQUIRE(updatedState);
    CHECK(std::find(updatedState->expanded_ids.begin(),
                    updatedState->expanded_ids.end(),
                    "src") != updatedState->expanded_ids.end());

    auto opQueuePath = binding->options.ops_queue.getPath();
    auto toggleOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(toggleOp);
    CHECK(toggleOp->kind == WidgetBindings::WidgetOpKind::TreeToggle);
    CHECK(toggleOp->target_id == "src");

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(renderEvent);
    CHECK(renderEvent->reason == "widget/tree");

    auto loadOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(loadOp);
    CHECK(loadOp->kind == WidgetBindings::WidgetOpKind::TreeRequestLoad);
    CHECK(loadOp->target_id == "src");

    auto scrollOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE_FALSE(scrollOp);
}

TEST_CASE("Widgets::CreateStack composes vertical layout") {
    BuildersFixture fx;

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "stack_button";
    buttonParams.label = "Stack Button";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "stack_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "stack_slider";
    sliderParams.minimum = 0.0f;
    sliderParams.maximum = 1.0f;
    sliderParams.value = 0.5f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    Widgets::StackLayoutParams stackParams{};
    stackParams.name = "column";
    stackParams.style.axis = Widgets::StackAxis::Vertical;
    stackParams.style.spacing = 24.0f;
    stackParams.style.padding_main_start = 16.0f;
    stackParams.style.padding_cross_start = 20.0f;
    stackParams.children = {
        Widgets::StackChildSpec{
            .id = "button",
            .widget_path = button->root.getPath(),
            .scene_path = button->scene.getPath(),
        },
        Widgets::StackChildSpec{
            .id = "toggle",
            .widget_path = toggle->root.getPath(),
            .scene_path = toggle->scene.getPath(),
        },
        Widgets::StackChildSpec{
            .id = "slider",
            .widget_path = slider->root.getPath(),
            .scene_path = slider->scene.getPath(),
        },
    };

    auto stack = Widgets::CreateStack(fx.space, fx.root_view(), stackParams);
    REQUIRE(stack);
    CHECK(stack->scene.getPath() == "/system/applications/test_app/scenes/widgets/column");

    auto layout = Widgets::ReadStackLayout(fx.space, *stack);
    REQUIRE(layout);
    REQUIRE(layout->children.size() == 3);
    CHECK(layout->width >= buttonParams.style.width);
    CHECK(layout->height > 0.0f);

    auto& buttonChild = layout->children[0];
    auto& toggleChild = layout->children[1];
    auto& sliderChild = layout->children[2];

    CHECK(buttonChild.id == "button");
    CHECK(toggleChild.id == "toggle");
    CHECK(sliderChild.id == "slider");

    CHECK(buttonChild.x == doctest::Approx(stackParams.style.padding_cross_start));
    CHECK(buttonChild.y == doctest::Approx(stackParams.style.padding_main_start));
    CHECK(toggleChild.y > buttonChild.y);
    CHECK(sliderChild.y > toggleChild.y);

    auto revision = Scene::ReadCurrentRevision(fx.space, stack->scene);
    REQUIRE(revision);
    auto base = std::string(stack->scene.getPath()) + "/builds/" + format_revision(revision->revision);
    auto bucket = SceneSnapshotBuilder::decode_bucket(fx.space, base);
    REQUIRE(bucket);
    CHECK(bucket->drawable_ids.size() >= 3);
}

TEST_CASE("Widgets::UpdateListState clamps indices and marks dirty") {
    BuildersFixture fx;

    Widgets::ListParams listParams{};
    listParams.name = "inventory_updates";
    listParams.items = {
        Widgets::ListItem{.id = "sword", .label = "Sword", .enabled = false},
        Widgets::ListItem{.id = "shield", .label = "Shield", .enabled = true},
        Widgets::ListItem{.id = "bow", .label = "Bow", .enabled = true},
    };
    listParams.style.item_height = 32.0f;

    auto created = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(created);

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);

    Widgets::ListState desired{};
    desired.enabled = true;
    desired.selected_index = 0;
    desired.hovered_index = 5;
    desired.scroll_offset = 120.0f;

    auto changed = Widgets::UpdateListState(fx.space, *created, desired);
    REQUIRE(changed);
    CHECK(*changed);

    auto updated = read_value<Widgets::ListState>(fx.space, created->state.getPath());
    REQUIRE(updated);
    CHECK(updated->selected_index == 1);
    CHECK(updated->hovered_index == 2);
    CHECK(updated->scroll_offset == doctest::Approx(64.0f)); // two rows * 32 - 32

    auto updatedRevision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto unchanged = Widgets::UpdateListState(fx.space, *created, *updated);
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
}

TEST_CASE("Widgets::ResolveHitTarget extracts canonical widget path from hit test") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "resolve_hit_button";
    params.label = "Resolve";

    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    Scene::HitTestRequest request{};
    request.x = 12.0f;
    request.y = 16.0f;

    auto hit = Scene::HitTest(fx.space, button->scene, request);
    REQUIRE(hit);
    REQUIRE(hit->hit);

    auto resolved = Widgets::ResolveHitTarget(*hit);
    REQUIRE(resolved);
    CHECK(resolved->widget.getPath() == button->root.getPath());
    CHECK(resolved->component == std::string{"button/background"});

    auto pointer = WidgetBindings::PointerFromHit(*hit);
    CHECK(pointer.scene_x == doctest::Approx(request.x));
    CHECK(pointer.scene_y == doctest::Approx(request.y));
    CHECK(pointer.inside);
    CHECK(pointer.primary);
}

TEST_CASE("Widget button states match golden snapshots") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "golden_button";
    params.label = "Golden";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    auto dims = compute_widget_dimensions(fx, button->states);
    CHECK(dims.width > 0);
    CHECK(dims.height > 0);

    WidgetGoldenRenderer renderer{fx, "widget_button_golden", dims.width, dims.height};
    renderer.render(button->states.idle, "widget_button_idle.golden");
    renderer.render(button->states.hover, "widget_button_hover.golden");
    renderer.render(button->states.pressed, "widget_button_pressed.golden");
    renderer.render(button->states.disabled, "widget_button_disabled.golden");
}

TEST_CASE("Widget toggle states match golden snapshots") {
    BuildersFixture fx;

    Widgets::ToggleParams params{};
    params.name = "golden_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), params);
    REQUIRE(toggle);

    auto dims = compute_widget_dimensions(fx, toggle->states);
    CHECK(dims.width > 0);
    CHECK(dims.height > 0);

    WidgetGoldenRenderer renderer{fx, "widget_toggle_golden", dims.width, dims.height};
    renderer.render(toggle->states.idle, "widget_toggle_idle.golden");
    renderer.render(toggle->states.hover, "widget_toggle_hover.golden");
    renderer.render(toggle->states.pressed, "widget_toggle_pressed.golden");
    renderer.render(toggle->states.disabled, "widget_toggle_disabled.golden");
}

TEST_CASE("Widget slider states match golden snapshots") {
    BuildersFixture fx;

    Widgets::SliderParams params{};
    params.name = "golden_slider";
    params.minimum = 0.0f;
    params.maximum = 1.0f;
    params.value = 0.35f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), params);
    REQUIRE(slider);

    auto dims = compute_widget_dimensions(fx, slider->states);
    CHECK(dims.width > 0);
    CHECK(dims.height > 0);

    WidgetGoldenRenderer renderer{fx, "widget_slider_golden", dims.width, dims.height};
    renderer.render(slider->states.idle, "widget_slider_idle.golden");
    renderer.render(slider->states.hover, "widget_slider_hover.golden");
    renderer.render(slider->states.pressed, "widget_slider_pressed.golden");
    renderer.render(slider->states.disabled, "widget_slider_disabled.golden");
}

TEST_CASE("Widget list states match golden snapshots") {
    BuildersFixture fx;

    Widgets::ListParams params{};
    params.name = "golden_list";
    params.items = {
        Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
        Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
        Widgets::ListItem{.id = "gamma", .label = "Gamma", .enabled = false},
    };
    params.style.width = 260.0f;
    params.style.item_height = 38.0f;
    auto list = Widgets::CreateList(fx.space, fx.root_view(), params);
    REQUIRE(list);

    auto dims = compute_widget_dimensions(fx, list->states);
    CHECK(dims.width > 0);
    CHECK(dims.height > 0);

    WidgetGoldenRenderer renderer{fx, "widget_list_golden", dims.width, dims.height};
    renderer.render(list->states.idle, "widget_list_idle.golden");
    renderer.render(list->states.hover, "widget_list_hover.golden");
    renderer.render(list->states.pressed, "widget_list_pressed.golden");
    renderer.render(list->states.disabled, "widget_list_disabled.golden");
}

TEST_CASE("Widgets::Focus::Set and Move update widget states") {
    BuildersFixture fx;

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "focus_button";
    buttonParams.label = "Focus";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "focus_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "focus_slider";
    sliderParams.minimum = 0.0f;
    sliderParams.maximum = 1.0f;
    sliderParams.value = 0.25f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    Widgets::ListParams listParams{};
    listParams.name = "focus_list";
    listParams.items = {
        Widgets::ListItem{.id = "alpha", .label = "Alpha"},
        Widgets::ListItem{.id = "beta", .label = "Beta"},
        Widgets::ListItem{.id = "gamma", .label = "Gamma"},
    };
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    auto config = WidgetFocus::MakeConfig(fx.root_view());

    auto setButton = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(setButton);
    CHECK(setButton->changed);

    auto buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK(buttonState->hovered);
    CHECK(buttonState->focused);

    auto toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK_FALSE(toggleState->hovered);
    CHECK_FALSE(toggleState->focused);

    std::array<WidgetPath, 4> order{
        button->root,
        toggle->root,
        slider->root,
        list->root,
    };

    auto moveToggle = WidgetFocus::Move(fx.space,
                                        config,
                                        std::span<const WidgetPath>(order.data(), order.size()),
                                        WidgetFocus::Direction::Forward);
    REQUIRE(moveToggle);
    REQUIRE(moveToggle->has_value());
    CHECK(moveToggle->value().widget.getPath() == toggle->root.getPath());

    toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK(toggleState->hovered);
    CHECK(toggleState->focused);

    buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK_FALSE(buttonState->hovered);
    CHECK_FALSE(buttonState->focused);

    // Advance to slider, then list
    (void)WidgetFocus::Move(fx.space,
                            config,
                            std::span<const WidgetPath>(order.data(), order.size()),
                            WidgetFocus::Direction::Forward);
    auto moveList = WidgetFocus::Move(fx.space,
                                      config,
                                      std::span<const WidgetPath>(order.data(), order.size()),
                                      WidgetFocus::Direction::Forward);
    REQUIRE(moveList);
    REQUIRE(moveList->has_value());
    CHECK(moveList->value().widget.getPath() == list->root.getPath());

    auto focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == list->root.getPath());

    auto listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->hovered_index >= 0);
    CHECK(listState->focused);

    auto cleared = WidgetFocus::Clear(fx.space, config);
    REQUIRE(cleared);
    CHECK(*cleared);

    listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->hovered_index == -1);
    CHECK_FALSE(listState->focused);
}

TEST_CASE("Widgets::Focus::ApplyHit focuses widget from hit test") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "focus_hit_button";
    params.label = "FocusHit";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    auto config = WidgetFocus::MakeConfig(fx.root_view());

    Scene::HitTestRequest request{};
    request.x = 8.0f;
    request.y = 8.0f;

    auto hit = Scene::HitTest(fx.space, button->scene, request);
    REQUIRE(hit);
    REQUIRE(hit->hit);

    auto result = WidgetFocus::ApplyHit(fx.space, config, *hit);
    REQUIRE(result);
    REQUIRE(result->has_value());
    CHECK(result->value().widget.getPath() == button->root.getPath());

    auto state = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(state);
    CHECK(state->hovered);
    CHECK(state->focused);
}

TEST_CASE("Widgets::Focus::Set schedules auto render events") {
    BuildersFixture fx;

    Widgets::ButtonParams params{};
    params.name = "focus_auto_button";
    params.label = "Auto";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    RendererParams rendererParams{ .name = "focus_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 192};

    SurfaceParams surfaceParams{ .name = "focus_surface", .desc = desc, .renderer = "renderers/focus_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, button->scene));

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);

    auto config = WidgetFocus::MakeConfig(fx.root_view(),
                                          SP::UI::Builders::ConcretePath{targetAbs->getPath()});

    auto setFocus = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(setFocus);
    CHECK(setFocus->changed);

    auto queuePath = targetAbs->getPath() + "/events/renderRequested/queue";
    auto event = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
    REQUIRE(event);
    CHECK(event->reason == "focus-navigation");

    auto noExtra = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(noExtra);
    CHECK_FALSE(noExtra->changed);

    auto noEvent = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
    CHECK_FALSE(noEvent);
    if (!noEvent) {
        CHECK((noEvent.error().code == Error::Code::NoObjectFound
               || noEvent.error().code == Error::Code::NoSuchPath));
    }
}

TEST_CASE("Widget focus state publishes highlight drawable") {
    BuildersFixture fx;

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "focus_highlight_button";
    buttonParams.label = "FocusHighlight";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto config = WidgetFocus::MakeConfig(fx.root_view());
    auto setFocus = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(setFocus);
    CHECK(setFocus->changed);

    SceneSnapshotBuilder builder(fx.space, fx.root_view(), button->scene);
    auto records = builder.snapshot_records();
    REQUIRE(records);
    REQUIRE_FALSE(records->empty());

    auto latest = records->back().revision;
    std::ostringstream revision_base;
    revision_base << button->scene.getPath() << "/builds/"
                  << std::setw(16) << std::setfill('0') << latest;
    auto bucket = SceneSnapshotBuilder::decode_bucket(fx.space, revision_base.str());
    REQUIRE(bucket);

    bool found_highlight = false;
    for (auto const& entry : bucket->authoring_map) {
        if (entry.authoring_node_id.find("/focus/highlight") != std::string::npos) {
            found_highlight = true;
            break;
        }
    }
    CHECK(found_highlight);
}

TEST_CASE("Widgets::Focus keyboard navigation cycles focus order and schedules renders") {
    BuildersFixture fx;

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "keyboard_focus_button";
    buttonParams.label = "KeyboardButton";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "keyboard_focus_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "keyboard_focus_slider";
    sliderParams.minimum = 0.0f;
    sliderParams.maximum = 1.0f;
    sliderParams.value = 0.42f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    Widgets::ListParams listParams{};
    listParams.name = "keyboard_focus_list";
    listParams.items = {
        Widgets::ListItem{.id = "one", .label = "One"},
        Widgets::ListItem{.id = "two", .label = "Two"},
        Widgets::ListItem{.id = "three", .label = "Three"},
    };
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    RendererParams rendererParams{ .name = "keyboard_focus_renderer",
                                   .kind = RendererKind::Software2D,
                                   .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 192};

    SurfaceParams surfaceParams{
        .name = "keyboard_focus_surface",
        .desc = desc,
        .renderer = "renderers/keyboard_focus_renderer",
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, button->scene));

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);

    auto config = WidgetFocus::MakeConfig(
        fx.root_view(),
        SP::UI::Builders::ConcretePath{targetAbs->getPath()});

    std::array<WidgetPath, 4> order{
        button->root,
        toggle->root,
        slider->root,
        list->root,
    };

    auto queuePath = targetAbs->getPath() + "/events/renderRequested/queue";
    auto ensure_event = [&](std::uint64_t last_seq) -> std::uint64_t {
        auto event = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
        REQUIRE(event);
        CHECK(event->reason == "focus-navigation");
        CHECK(event->sequence > last_seq);
        return event->sequence;
    };

    std::uint64_t lastSequence = 0;

    // Simulate Tab key: focus advances to the first widget.
    auto moveButton = WidgetFocus::Move(fx.space,
                                        config,
                                        std::span<const WidgetPath>(order.data(), order.size()),
                                        WidgetFocus::Direction::Forward);
    REQUIRE(moveButton);
    REQUIRE(moveButton->has_value());
    CHECK(moveButton->value().widget.getPath() == button->root.getPath());
    CHECK(moveButton->value().changed);
    lastSequence = ensure_event(lastSequence);

    auto focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == button->root.getPath());

    auto buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK(buttonState->hovered);

    auto toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK_FALSE(toggleState->hovered);

    // Another Tab: advance focus to the toggle.
    auto moveToggle = WidgetFocus::Move(fx.space,
                                        config,
                                        std::span<const WidgetPath>(order.data(), order.size()),
                                        WidgetFocus::Direction::Forward);
    REQUIRE(moveToggle);
    REQUIRE(moveToggle->has_value());
    CHECK(moveToggle->value().widget.getPath() == toggle->root.getPath());
    CHECK(moveToggle->value().changed);
    lastSequence = ensure_event(lastSequence);

    toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK(toggleState->hovered);

    buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK_FALSE(buttonState->hovered);

    focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == toggle->root.getPath());

    // Shift+Tab: move focus back to the button.
    auto moveBack = WidgetFocus::Move(fx.space,
                                      config,
                                      std::span<const WidgetPath>(order.data(), order.size()),
                                      WidgetFocus::Direction::Backward);
    REQUIRE(moveBack);
    REQUIRE(moveBack->has_value());
    CHECK(moveBack->value().widget.getPath() == button->root.getPath());
    CHECK(moveBack->value().changed);
    lastSequence = ensure_event(lastSequence);

    buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK(buttonState->hovered);

    toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK_FALSE(toggleState->hovered);

    focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == button->root.getPath());

    auto noEvent = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
    CHECK_FALSE(noEvent);
    if (!noEvent) {
        CHECK((noEvent.error().code == Error::Code::NoObjectFound
               || noEvent.error().code == Error::Code::NoSuchPath));
    }
}

TEST_CASE("Widgets::Focus gamepad navigation hops focus order and schedules renders") {
    BuildersFixture fx;

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "gamepad_focus_button";
    buttonParams.label = "GamepadButton";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "gamepad_focus_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    Widgets::SliderParams sliderParams{};
    sliderParams.name = "gamepad_focus_slider";
    sliderParams.minimum = 0.0f;
    sliderParams.maximum = 1.0f;
    sliderParams.value = 0.7f;
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    Widgets::ListParams listParams{};
    listParams.name = "gamepad_focus_list";
    listParams.items = {
        Widgets::ListItem{.id = "north", .label = "North"},
        Widgets::ListItem{.id = "south", .label = "South"},
    };
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    RendererParams rendererParams{ .name = "gamepad_focus_renderer",
                                   .kind = RendererKind::Software2D,
                                   .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 200};

    SurfaceParams surfaceParams{
        .name = "gamepad_focus_surface",
        .desc = desc,
        .renderer = "renderers/gamepad_focus_renderer",
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, slider->scene));

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);

    auto config = WidgetFocus::MakeConfig(
        fx.root_view(),
        SP::UI::Builders::ConcretePath{targetAbs->getPath()});

    std::array<WidgetPath, 4> order{
        button->root,
        slider->root,
        list->root,
        toggle->root,
    };

    auto queuePath = targetAbs->getPath() + "/events/renderRequested/queue";
    auto take_event = [&]() -> AutoRenderRequestEvent {
        auto event = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
        REQUIRE(event);
        CHECK(event->reason == "focus-navigation");
        return *event;
    };

    // Simulate selecting the slider via a focused gamepad interaction.
    auto setSlider = WidgetFocus::Set(fx.space, config, slider->root);
    REQUIRE(setSlider);
    CHECK(setSlider->changed);
    auto sliderEvent = take_event();
    auto lastSequence = sliderEvent.sequence;

    auto sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK(sliderState->hovered);

    auto listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->hovered_index == -1);

    auto focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == slider->root.getPath());

    // Hop forward (e.g., D-pad right/down): moves focus to the list.
    auto moveList = WidgetFocus::Move(fx.space,
                                      config,
                                      std::span<const WidgetPath>(order.data(), order.size()),
                                      WidgetFocus::Direction::Forward);
    REQUIRE(moveList);
    REQUIRE(moveList->has_value());
    CHECK(moveList->value().widget.getPath() == list->root.getPath());
    CHECK(moveList->value().changed);
    auto listEvent = take_event();
    CHECK(listEvent.sequence > lastSequence);
    lastSequence = listEvent.sequence;

    listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->hovered_index >= 0);

    sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK_FALSE(sliderState->hovered);

    focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == list->root.getPath());

    // Hop backward (e.g., D-pad left/up): returns focus to the slider.
    auto moveSlider = WidgetFocus::Move(fx.space,
                                        config,
                                        std::span<const WidgetPath>(order.data(), order.size()),
                                        WidgetFocus::Direction::Backward);
    REQUIRE(moveSlider);
    REQUIRE(moveSlider->has_value());
    CHECK(moveSlider->value().widget.getPath() == slider->root.getPath());
    CHECK(moveSlider->value().changed);
    auto backEvent = take_event();
    CHECK(backEvent.sequence > lastSequence);
    lastSequence = backEvent.sequence;

    sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK(sliderState->hovered);

    focusPath = fx.space.read<std::string, std::string>(config.focus_state.getPath());
    REQUIRE(focusPath);
    CHECK(*focusPath == slider->root.getPath());

    // Repeat Set on the same widget should not schedule an additional render.
    auto repeatSet = WidgetFocus::Set(fx.space, config, slider->root);
    REQUIRE(repeatSet);
    CHECK_FALSE(repeatSet->changed);
    auto noEvent = fx.space.take<AutoRenderRequestEvent, std::string>(queuePath);
    CHECK_FALSE(noEvent);
    if (!noEvent) {
        CHECK((noEvent.error().code == Error::Code::NoObjectFound
               || noEvent.error().code == Error::Code::NoSuchPath));
    }
}

TEST_CASE("Widgets::Bindings::DispatchList enqueues ops and schedules renders") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_list_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 240};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_list_surface", .desc = desc, .renderer = "renderers/bindings_list_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_list_surface");
    REQUIRE(target);

    Widgets::ListParams listParams{};
    listParams.name = "inventory_bindings";
    listParams.items = {
        Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
        Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
    };
    auto listWidget = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(listWidget);

    auto binding = WidgetBindings::CreateListBinding(fx.space,
                                                     fx.root_view(),
                                                     *listWidget,
                                                     SP::ConcretePathStringView{target->getPath()});
    REQUIRE(binding);

    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = 10.0f;
    pointer.scene_y = 18.0f;
    pointer.inside = true;

    Widgets::ListState selectState{};
    selectState.selected_index = 1;

    auto selectResult = WidgetBindings::DispatchList(fx.space,
                                                     *binding,
                                                     selectState,
                                                     WidgetBindings::WidgetOpKind::ListSelect,
                                                     pointer,
                                                     1);
    REQUIRE(selectResult);
    CHECK(*selectResult);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(renderEvent);
    CHECK(renderEvent->reason == "widget/list");

    auto opQueuePath = binding->options.ops_queue.getPath();
    auto selectOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(selectOp);
    CHECK(selectOp->kind == WidgetBindings::WidgetOpKind::ListSelect);
    CHECK(selectOp->value == doctest::Approx(1.0f));

    Widgets::ListState hoverState{};
    auto hoverResult = WidgetBindings::DispatchList(fx.space,
                                                    *binding,
                                                    hoverState,
                                                    WidgetBindings::WidgetOpKind::ListHover,
                                                    pointer,
                                                    0);
    REQUIRE(hoverResult);
    CHECK(*hoverResult);

    auto hoverEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(hoverEvent);
    CHECK(hoverEvent->reason == "widget/list");

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::ListHover);
    CHECK(hoverOp->value == doctest::Approx(0.0f));

    Widgets::ListState scrollState{};
    scrollState.scroll_offset = 40.0f;
    auto scrollResult = WidgetBindings::DispatchList(fx.space,
                                                     *binding,
                                                     scrollState,
                                                     WidgetBindings::WidgetOpKind::ListScroll,
                                                     pointer,
                                                     -1,
                                                     12.0f);
    REQUIRE(scrollResult);
    CHECK(*scrollResult);

    auto scrollEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(scrollEvent);
    CHECK(scrollEvent->reason == "widget/list");

    auto scrollOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(scrollOp);
    CHECK(scrollOp->kind == WidgetBindings::WidgetOpKind::ListScroll);
    CHECK(scrollOp->value >= 0.0f);

    Widgets::ListState disabled{};
    disabled.enabled = false;
    disabled.selected_index = 1;

    auto disableResult = Widgets::UpdateListState(fx.space, *listWidget, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    CHECK_FALSE(disableEvent);
    if (!disableEvent) {
        CHECK((disableEvent.error().code == Error::Code::NoObjectFound || disableEvent.error().code == Error::Code::NoSuchPath));
    }

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto storedState = fx.space.read<Widgets::ListState, std::string>(listWidget->state.getPath());
    REQUIRE(storedState);
    CHECK_FALSE(storedState->enabled);
}

TEST_CASE("Widgets::Reducers::ReducePending routes widget ops to action queues") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "reducers_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 200};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "reducers_surface", .desc = desc, .renderer = "renderers/reducers_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/reducers_surface");
    REQUIRE(target);

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "reducers_button";
    buttonParams.label = "Reducers";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto buttonBinding = WidgetBindings::CreateButtonBinding(fx.space,
                                                             fx.root_view(),
                                                             *button,
                                                             SP::ConcretePathStringView{target->getPath()});
    REQUIRE(buttonBinding);

    WidgetBindings::PointerInfo pointer{};
    pointer.inside = true;
    pointer.scene_x = 4.0f;
    pointer.scene_y = 5.0f;

    Widgets::ButtonState pressed{};
    pressed.pressed = true;
    pressed.hovered = true;

    auto dispatched = WidgetBindings::DispatchButton(fx.space,
                                                     *buttonBinding,
                                                     pressed,
                                                     WidgetBindings::WidgetOpKind::Press,
                                                     pointer);
    REQUIRE(dispatched);
    CHECK(*dispatched);

    auto buttonOpsQueue = WidgetReducers::WidgetOpsQueue(button->root);
    auto reduceResult = WidgetReducers::ReducePending(fx.space,
                                                      SP::ConcretePathStringView{buttonOpsQueue.getPath()});
    REQUIRE(reduceResult);
    REQUIRE(reduceResult->size() == 1);

    auto const& action = reduceResult->front();
    CHECK(action.kind == WidgetBindings::WidgetOpKind::Press);
    CHECK(action.widget_path == button->root.getPath());
    CHECK(action.pointer.inside);
    CHECK(action.analog_value == doctest::Approx(1.0f));
    CHECK(action.discrete_index == -1);

    auto buttonActionsQueue = WidgetReducers::DefaultActionsQueue(button->root);
    auto spanActions = std::span<const WidgetReducers::WidgetAction>(reduceResult->data(), reduceResult->size());
    auto publish = WidgetReducers::PublishActions(fx.space,
                                                  SP::ConcretePathStringView{buttonActionsQueue.getPath()},
                                                  spanActions);
    REQUIRE(publish);

    auto storedAction = fx.space.take<WidgetReducers::WidgetAction, std::string>(buttonActionsQueue.getPath());
    REQUIRE(storedAction);
    CHECK(storedAction->widget_path == button->root.getPath());
    CHECK(storedAction->analog_value == doctest::Approx(1.0f));

    Widgets::ListParams listParams{};
    listParams.name = "reducers_list";
    listParams.items = {
        Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
        Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
    };
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    auto listBinding = WidgetBindings::CreateListBinding(fx.space,
                                                         fx.root_view(),
                                                         *list,
                                                         SP::ConcretePathStringView{target->getPath()});
    REQUIRE(listBinding);

    Widgets::ListState listState{};
    listState.selected_index = 1;
    auto listDispatch = WidgetBindings::DispatchList(fx.space,
                                                     *listBinding,
                                                     listState,
                                                     WidgetBindings::WidgetOpKind::ListSelect,
                                                     pointer,
                                                     1);
    REQUIRE(listDispatch);
    CHECK(*listDispatch);

    auto listOpsQueue = WidgetReducers::WidgetOpsQueue(list->root);
    auto listReduce = WidgetReducers::ReducePending(fx.space,
                                                    SP::ConcretePathStringView{listOpsQueue.getPath()});
    REQUIRE(listReduce);
    REQUIRE(listReduce->size() == 1);
    auto const& listAction = listReduce->front();
    CHECK(listAction.kind == WidgetBindings::WidgetOpKind::ListSelect);
    CHECK(listAction.discrete_index == 1);
    CHECK(listAction.analog_value == doctest::Approx(1.0f));

    auto listActionsQueue = WidgetReducers::DefaultActionsQueue(list->root);
    auto listSpan = std::span<const WidgetReducers::WidgetAction>(listReduce->data(), listReduce->size());
    auto listPublish = WidgetReducers::PublishActions(fx.space,
                                                      SP::ConcretePathStringView{listActionsQueue.getPath()},
                                                      listSpan);
    REQUIRE(listPublish);

    auto storedListAction = fx.space.take<WidgetReducers::WidgetAction, std::string>(listActionsQueue.getPath());
    REQUIRE(storedListAction);
    CHECK(storedListAction->discrete_index == 1);
    CHECK(storedListAction->widget_path == list->root.getPath());
}

TEST_CASE("Html::Asset vectors survive PathSpace round-trip") {
    BuildersFixture fx;

    auto const base = std::string(fx.app_root.getPath()) + "/html/test/assets";

    std::vector<Asset> assets;
    Asset image{};
    image.logical_path = "images/example.png";
    image.mime_type = "image/png";
    image.bytes = {0u, 17u, 34u, 0u, 255u, 128u};
    assets.emplace_back(image);

    Asset font{};
    font.logical_path = "fonts/display.woff2";
    font.mime_type = "font/woff2";
    font.bytes = {1u, 3u, 3u, 7u};
    assets.emplace_back(font);

    auto inserted = fx.space.insert(base, assets);
    REQUIRE(inserted.errors.empty());

    auto read_back = fx.space.read<std::vector<Asset>>(base);
    REQUIRE(read_back);
    REQUIRE(read_back->size() == assets.size());
    for (std::size_t index = 0; index < assets.size(); ++index) {
        CHECK((*read_back)[index].logical_path == assets[index].logical_path);
        CHECK((*read_back)[index].mime_type == assets[index].mime_type);
        CHECK((*read_back)[index].bytes == assets[index].bytes);
    }

    auto taken = fx.space.take<std::vector<Asset>>(base);
    REQUIRE(taken);
    REQUIRE(taken->size() == assets.size());
    CHECK((*taken)[0].bytes == assets[0].bytes);
    CHECK((*taken)[1].logical_path == assets[1].logical_path);

    auto missing = fx.space.read<std::vector<Asset>>(base);
    REQUIRE_FALSE(missing);
    auto const missingCode = missing.error().code;
    bool const missingOk = missingCode == SP::Error::Code::NoObjectFound
                           || missingCode == SP::Error::Code::NoSuchPath;
    CHECK(missingOk);
}

TEST_CASE("Renderer::RenderHtml hydrates image assets into output") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "html_renderer_assets", .kind = RendererKind::Software2D, .description = "HTML" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SceneParams sceneParams{ .name = "scene_html_assets", .description = "html assets" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    constexpr std::uint64_t kImageFingerprint = 0xABCDEF0102030405ull;
    auto bucket = make_image_bucket(kImageFingerprint);

    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene};
    SnapshotPublishOptions opts{};
    opts.metadata.author = "tests";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);

    auto ready = Scene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
    REQUIRE(ready);

    auto revision_base = std::string(scene->getPath()) + "/builds/" + format_revision(*revision);
    auto logical_path = std::string("images/") + fingerprint_hex(kImageFingerprint) + ".png";
    auto image_path = revision_base + "/assets/" + logical_path;
    std::vector<std::uint8_t> png_bytes(kTestPngRgba.begin(), kTestPngRgba.end());
    auto insert_result = fx.space.insert(image_path, png_bytes);
    REQUIRE(insert_result.errors.empty());

    auto font_manifest_path = revision_base + "/assets/font-manifest";
    std::vector<Html::Asset> font_manifest;
    Html::Asset font_asset{};
    font_asset.logical_path = "fonts/display.woff2";
    font_asset.mime_type = "font/woff2";
    font_manifest.push_back(font_asset);
    auto font_manifest_insert = fx.space.insert(font_manifest_path, font_manifest);
    REQUIRE(font_manifest_insert.errors.empty());

    auto manifest_check = fx.space.read<std::vector<Html::Asset>>(font_manifest_path);
    REQUIRE(manifest_check);

    auto font_bytes_path = revision_base + "/assets/fonts/display.woff2";
    std::vector<std::uint8_t> font_bytes{0xF0u, 0x0Du, 0xC0u, 0xDEu};
    auto font_bytes_insert = fx.space.insert(font_bytes_path, font_bytes);
    REQUIRE(font_bytes_insert.errors.empty());

    HtmlTargetParams targetParams{};
    targetParams.name = "preview_assets";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    if (!render_html.has_value()) {
        auto const& err = render_html.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("RenderHtml failed"));
        auto diagValue = fx.space.read<PathSpaceError>(htmlBase + "/diagnostics/errors/live");
        if (diagValue) {
            CAPTURE(diagValue->message);
            CAPTURE(diagValue->detail);
        } else {
            CAPTURE(static_cast<int>(diagValue.error().code));
            CAPTURE(diagValue.error().message.value_or("diagnostics read failed"));
        }
    }
    REQUIRE(render_html.has_value());
    auto assets = read_value<std::vector<Asset>>(fx.space, htmlBase + "/assets");
    REQUIRE(assets);
    REQUIRE(assets->size() == 2);
    bool found_image = false;
    bool found_font = false;
    for (auto const& asset : *assets) {
        if (asset.logical_path == logical_path) {
            found_image = true;
            CHECK(asset.mime_type == "image/png");
            CHECK(asset.bytes == std::vector<std::uint8_t>(kTestPngRgba.begin(), kTestPngRgba.end()));
        } else if (asset.logical_path == "fonts/display.woff2") {
            found_font = true;
            CHECK(asset.mime_type == "font/woff2");
            CHECK(asset.bytes == font_bytes);
        }
    }
    CHECK(found_image);
    CHECK(found_font);

    auto manifest = read_value<std::vector<std::string>>(fx.space, htmlBase + "/assets/manifest");
    REQUIRE(manifest);
    REQUIRE(manifest->size() == 2);
    CHECK(std::find(manifest->begin(), manifest->end(), logical_path) != manifest->end());
    CHECK(std::find(manifest->begin(), manifest->end(), std::string{"fonts/display.woff2"}) != manifest->end());

    auto dataPath = htmlBase + "/assets/data/" + logical_path;
    auto storedBytes = read_value<std::vector<std::uint8_t>>(fx.space, dataPath);
    REQUIRE(storedBytes);
    CHECK(*storedBytes == std::vector<std::uint8_t>(kTestPngRgba.begin(), kTestPngRgba.end()));

    auto mimePath = htmlBase + "/assets/meta/" + logical_path;
    auto storedMime = read_value<std::string>(fx.space, mimePath);
    REQUIRE(storedMime);
    CHECK(*storedMime == std::string{"image/png"});

    auto fontDataPath = htmlBase + "/assets/data/fonts/display.woff2";
    auto storedFontBytes = read_value<std::vector<std::uint8_t>>(fx.space, fontDataPath);
    REQUIRE(storedFontBytes);
    CHECK(*storedFontBytes == font_bytes);

    auto fontMimePath = htmlBase + "/assets/meta/fonts/display.woff2";
    auto storedFontMime = read_value<std::string>(fx.space, fontMimePath);
    REQUIRE(storedFontMime);
    CHECK(*storedFontMime == std::string{"font/woff2"});

    auto cssValue = read_value<std::string>(fx.space, htmlBase + "/css");
    REQUIRE(cssValue);
    CHECK(cssValue->find("@font-face") != std::string::npos);
    CHECK(cssValue->find("assets/fonts/display.woff2") != std::string::npos);
}

TEST_CASE("Renderer::RenderHtml clears stale asset payloads") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "html_renderer_stale", .kind = RendererKind::Software2D, .description = "HTML" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SceneParams sceneParams{ .name = "scene_html_stale", .description = "html stale assets" };
    auto scene = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    constexpr std::uint64_t kImageFingerprint = 0xABCDEF0102030405ull;
    auto bucket_with_image = make_image_bucket(kImageFingerprint);

    SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene};
    SnapshotPublishOptions opts{};
    opts.metadata.author = "tests";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket_with_image.drawable_ids.size();
    opts.metadata.command_count = bucket_with_image.command_kinds.size();
    auto revision = builder.publish(opts, bucket_with_image);
    REQUIRE(revision);

    auto ready = Scene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
    REQUIRE(ready);

    auto revision_base = std::string(scene->getPath()) + "/builds/" + format_revision(*revision);
    auto logical_path = std::string("images/") + fingerprint_hex(kImageFingerprint) + ".png";
    auto image_path = revision_base + "/assets/" + logical_path;
    std::vector<std::uint8_t> png_bytes(kTestPngRgba.begin(), kTestPngRgba.end());
    auto insert_result = fx.space.insert(image_path, png_bytes);
    REQUIRE(insert_result.errors.empty());

    auto font_manifest_path = revision_base + "/assets/font-manifest";
    std::vector<Html::Asset> font_manifest_initial;
    Html::Asset font_asset_initial{};
    font_asset_initial.logical_path = "fonts/display.woff2";
    font_asset_initial.mime_type = "font/woff2";
    font_manifest_initial.push_back(font_asset_initial);
    auto font_manifest_insert = fx.space.insert(font_manifest_path, font_manifest_initial);
    REQUIRE(font_manifest_insert.errors.empty());

    auto manifest_check = fx.space.read<std::vector<Html::Asset>>(font_manifest_path);
    REQUIRE(manifest_check);

    auto font_bytes_path = revision_base + "/assets/fonts/display.woff2";
    std::vector<std::uint8_t> font_bytes{0xF0u, 0x0Du, 0xC0u, 0xDEu};
    auto font_bytes_insert = fx.space.insert(font_bytes_path, font_bytes);
    REQUIRE(font_bytes_insert.errors.empty());

    HtmlTargetParams targetParams{};
    targetParams.name = "preview_stale";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    REQUIRE(render_html);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto manifest = read_value<std::vector<std::string>>(fx.space, htmlBase + "/assets/manifest");
    REQUIRE(manifest);
    REQUIRE(manifest->size() == 2);
    CHECK(std::find(manifest->begin(), manifest->end(), logical_path) != manifest->end());
    CHECK(std::find(manifest->begin(), manifest->end(), std::string{"fonts/display.woff2"}) != manifest->end());

    // Publish a new revision with no assets and render again.
    auto bucket_no_assets = make_rect_bucket();
    SnapshotPublishOptions opts2 = opts;
    opts2.metadata.drawable_count = bucket_no_assets.drawable_ids.size();
    opts2.metadata.command_count = bucket_no_assets.command_kinds.size();
    auto revision2 = builder.publish(opts2, bucket_no_assets);
    REQUIRE(revision2);

    auto ready2 = Scene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
    REQUIRE(ready2);

    auto render_html2 = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    if (!render_html2.has_value()) {
        auto const& err = render_html2.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("RenderHtml failed"));
        auto diagValue = fx.space.read<PathSpaceError>(htmlBase + "/diagnostics/errors/live");
        if (diagValue) {
            CAPTURE(diagValue->message);
            CAPTURE(diagValue->detail);
        } else {
            CAPTURE(static_cast<int>(diagValue.error().code));
            CAPTURE(diagValue.error().message.value_or("diagnostics read failed"));
        }
    }
    REQUIRE(render_html2.has_value());

    auto manifest_after = fx.space.read<std::vector<std::string>, std::string>(htmlBase + "/assets/manifest");
    CHECK_FALSE(manifest_after.has_value());
    if (!manifest_after.has_value()) {
        CHECK((manifest_after.error().code == Error::Code::NoSuchPath
               || manifest_after.error().code == Error::Code::NoObjectFound));
    }

    auto dataPath = htmlBase + "/assets/data/" + logical_path;
    auto dataResult = fx.space.read<std::vector<std::uint8_t>, std::string>(dataPath);
    CHECK_FALSE(dataResult.has_value());
    if (!dataResult.has_value()) {
        CHECK((dataResult.error().code == Error::Code::NoObjectFound || dataResult.error().code == Error::Code::NoSuchPath));
    }

    auto mimePath = htmlBase + "/assets/meta/" + logical_path;
    auto mimeResult = fx.space.read<std::string, std::string>(mimePath);
    CHECK_FALSE(mimeResult.has_value());
    if (!mimeResult.has_value()) {
        CHECK((mimeResult.error().code == Error::Code::NoObjectFound || mimeResult.error().code == Error::Code::NoSuchPath));
    }

    auto fontDataPath = htmlBase + "/assets/data/fonts/display.woff2";
    auto fontDataResult = fx.space.read<std::vector<std::uint8_t>, std::string>(fontDataPath);
    CHECK_FALSE(fontDataResult.has_value());
    if (!fontDataResult.has_value()) {
        CHECK((fontDataResult.error().code == Error::Code::NoObjectFound || fontDataResult.error().code == Error::Code::NoSuchPath));
    }

    auto fontMimePath = htmlBase + "/assets/meta/fonts/display.woff2";
    auto fontMimeResult = fx.space.read<std::string, std::string>(fontMimePath);
    CHECK_FALSE(fontMimeResult.has_value());
    if (!fontMimeResult.has_value()) {
        CHECK((fontMimeResult.error().code == Error::Code::NoObjectFound || fontMimeResult.error().code == Error::Code::NoSuchPath));
    }
}

TEST_CASE("SubmitDirtyRects coalesces tile-aligned hints") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 128};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "dirty_rects", .desc = desc, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/dirty_rects");
    REQUIRE(target);

    std::vector<DirtyRectHint> hints{
        DirtyRectHint{0.0f, 0.0f, 32.0f, 32.0f},
        DirtyRectHint{32.0f, 0.0f, 64.0f, 32.0f},
        DirtyRectHint{0.0f, 32.0f, 32.0f, 64.0f},
        DirtyRectHint{32.0f, 32.0f, 64.0f, 64.0f},
    };

    auto submit = Renderer::SubmitDirtyRects(fx.space,
                                             SP::ConcretePathStringView{target->getPath()},
                                             std::span<const DirtyRectHint>(hints.data(), hints.size()));
    REQUIRE(submit);

    auto stored = read_value<std::vector<DirtyRectHint>>(fx.space,
                                                         std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(stored);
    REQUIRE(stored->size() == 1);
    auto const& rect = stored->front();
    CHECK(rect.min_x == doctest::Approx(0.0f));
    CHECK(rect.min_y == doctest::Approx(0.0f));
    CHECK(rect.max_x == doctest::Approx(64.0f));
    CHECK(rect.max_y == doctest::Approx(64.0f));
}

TEST_CASE("SubmitDirtyRects collapses excessive hints to full surface") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 192};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "many_dirty_rects", .desc = desc, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/many_dirty_rects");
    REQUIRE(target);

    std::vector<DirtyRectHint> hints;
    hints.reserve(256);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 20; ++x) {
            DirtyRectHint hint{
                .min_x = static_cast<float>(x * 16),
                .min_y = static_cast<float>(y * 16),
                .max_x = static_cast<float>((x + 1) * 16),
                .max_y = static_cast<float>((y + 1) * 16),
            };
            hints.push_back(hint);
        }
    }

    auto submit = Renderer::SubmitDirtyRects(fx.space,
                                             SP::ConcretePathStringView{target->getPath()},
                                             std::span<const DirtyRectHint>(hints.data(), hints.size()));
    REQUIRE(submit);

    auto stored = read_value<std::vector<DirtyRectHint>>(fx.space,
                                                         std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(stored);
    REQUIRE(stored->size() == 1);
    auto const& rect = stored->front();
    CHECK(rect.min_x == doctest::Approx(0.0f));
    CHECK(rect.min_y == doctest::Approx(0.0f));
    CHECK(rect.max_x == doctest::Approx(static_cast<float>(desc.size_px.width)));
    CHECK(rect.max_y == doctest::Approx(static_cast<float>(desc.size_px.height)));
}

TEST_CASE("Widgets::Bindings::UpdateStack emits dirty hints and auto render events") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_stack_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {512, 512};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_stack_surface", .desc = desc, .renderer = "renderers/bindings_stack_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_stack_surface");
    REQUIRE(target);

    Widgets::ButtonParams buttonParams{};
    buttonParams.name = "stack_binding_button";
    buttonParams.label = "Primary";
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    Widgets::ToggleParams toggleParams{};
    toggleParams.name = "stack_binding_toggle";
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    Widgets::StackLayoutParams stackParams{};
    stackParams.name = "binding_stack";
    stackParams.style.axis = Widgets::StackAxis::Vertical;
    stackParams.style.spacing = 12.0f;
    stackParams.children = {
        Widgets::StackChildSpec{
            .id = "button",
            .widget_path = button->root.getPath(),
            .scene_path = button->scene.getPath(),
        },
        Widgets::StackChildSpec{
            .id = "toggle",
            .widget_path = toggle->root.getPath(),
            .scene_path = toggle->scene.getPath(),
        },
    };

    auto stack = Widgets::CreateStack(fx.space, fx.root_view(), stackParams);
    REQUIRE(stack);

    auto binding = WidgetBindings::CreateStackBinding(fx.space,
                                                      fx.root_view(),
                                                      *stack,
                                                      SP::ConcretePathStringView{target->getPath()});
    REQUIRE(binding);

    auto describe = Widgets::DescribeStack(fx.space, *stack);
    REQUIRE(describe);
    describe->style.spacing = 36.0f;

    auto updated = WidgetBindings::UpdateStack(fx.space, *binding, *describe);
    REQUIRE(updated);
    CHECK(*updated);

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto renderEvent = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
    REQUIRE(renderEvent);
    CHECK(renderEvent->reason == "widget/stack");
}

TEST_CASE("App bootstrap helper wires renderer, surface, and window defaults") {
    BuildersFixture fx;

    SceneParams sceneParams{
        .name = "gallery",
        .description = "bootstrap scene",
    };
    auto scenePath = Scene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    AppBootstrap::BootstrapParams params;
    params.renderer.name = "bootstrap_renderer";
    params.renderer.kind = RendererKind::Software2D;
    params.renderer.description = "bootstrap renderer";
    params.surface.name = "bootstrap_surface";
    params.surface.desc.size_px.width = 640;
    params.surface.desc.size_px.height = 360;
    params.window.name = "bootstrap_window";
    params.window.title = "Bootstrap Window";
    params.window.background = "#151820";
    params.window.width = 640;
    params.window.height = 360;
    params.view_name = "main";
    params.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    params.present_policy.vsync_align = false;
    params.present_policy.auto_render_on_present = true;
    params.present_policy.capture_framebuffer = false;
    params.present_policy.staleness_budget = std::chrono::milliseconds{0};
    params.present_policy.frame_timeout = std::chrono::milliseconds{0};
    params.configure_present_policy = true;
    params.configure_renderer_settings = true;
    params.submit_initial_dirty_rect = true;

    auto bootstrap = AppBootstrap::Bootstrap(fx.space, fx.root_view(), *scenePath, params);
    REQUIRE(bootstrap);
    auto const& result = *bootstrap;

    CHECK(result.renderer.getPath()
          == "/system/applications/test_app/renderers/bootstrap_renderer");
    CHECK(result.surface.getPath()
          == "/system/applications/test_app/surfaces/bootstrap_surface");
    CHECK(result.window.getPath()
          == "/system/applications/test_app/windows/bootstrap_window");
    CHECK(result.target.getPath()
          == "/system/applications/test_app/renderers/bootstrap_renderer/targets/surfaces/bootstrap_surface");
    CHECK(result.view_name == "main");
    CHECK(result.surface_desc.size_px.width == 640);
    CHECK(result.surface_desc.size_px.height == 360);
    CHECK(result.present_policy.mode == PathWindowView::PresentMode::AlwaysLatestComplete);
    CHECK(result.applied_settings.surface.size_px.width == 640);
    CHECK(result.applied_settings.surface.size_px.height == 360);
    CHECK(result.applied_settings.renderer.backend_kind == RendererKind::Software2D);

    auto surfaceScene = read_value<std::string>(fx.space,
                                                std::string(result.surface.getPath()) + "/scene");
    REQUIRE(surfaceScene);
    CHECK(*surfaceScene == "scenes/gallery");

    auto targetScene = read_value<std::string>(fx.space,
                                               std::string(result.target.getPath()) + "/scene");
    REQUIRE(targetScene);
    CHECK(*targetScene == "scenes/gallery");

    auto windowViewBase = std::string(result.window.getPath()) + "/views/" + result.view_name;
    auto attachedSurface = read_value<std::string>(fx.space, windowViewBase + "/surface");
    REQUIRE(attachedSurface);
    CHECK(*attachedSurface == "surfaces/bootstrap_surface");

    auto policyText = read_value<std::string>(fx.space, windowViewBase + "/present/policy");
    REQUIRE(policyText);
    CHECK(*policyText == "AlwaysLatestComplete");

    auto stalenessMs = read_value<double>(fx.space, windowViewBase + "/present/params/staleness_budget_ms");
    REQUIRE(stalenessMs);
    CHECK(*stalenessMs == doctest::Approx(0.0));

    auto frameTimeoutMs = read_value<double>(fx.space, windowViewBase + "/present/params/frame_timeout_ms");
    REQUIRE(frameTimeoutMs);
    CHECK(*frameTimeoutMs == doctest::Approx(0.0));

    auto maxAgeFrames = read_value<std::uint64_t>(fx.space, windowViewBase + "/present/params/max_age_frames");
    REQUIRE(maxAgeFrames);
    CHECK(*maxAgeFrames == 0);

    auto vsyncAlign = read_value<bool>(fx.space, windowViewBase + "/present/params/vsync_align");
    REQUIRE(vsyncAlign);
    CHECK_FALSE(*vsyncAlign);

    auto autoRender = read_value<bool>(fx.space, windowViewBase + "/present/params/auto_render_on_present");
    REQUIRE(autoRender);
    CHECK(*autoRender);

    auto captureFramebuffer = read_value<bool>(fx.space, windowViewBase + "/present/params/capture_framebuffer");
    REQUIRE(captureFramebuffer);
    CHECK_FALSE(*captureFramebuffer);

    auto storedSettings = Renderer::ReadSettings(fx.space,
                                                 ConcretePathView{result.target.getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->surface.size_px.width == 640);
    CHECK(storedSettings->surface.size_px.height == 360);
    CHECK(storedSettings->renderer.backend_kind == RendererKind::Software2D);

    auto dirtyRects = read_value<std::vector<DirtyRectHint>>(fx.space,
                                                             std::string(result.target.getPath())
                                                             + "/hints/dirtyRects");
    REQUIRE(dirtyRects);
    REQUIRE(dirtyRects->size() == 1);
    auto const& hint = dirtyRects->front();
    CHECK(hint.min_x == doctest::Approx(0.0f));
    CHECK(hint.min_y == doctest::Approx(0.0f));
    CHECK(hint.max_x == doctest::Approx(640.0f));
    CHECK(hint.max_y == doctest::Approx(360.0f));
}

} // TEST_SUITE
