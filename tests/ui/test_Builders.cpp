#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/declarative/StackReadiness.hpp>

#include "DeclarativeTestUtils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
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
namespace BuilderScene = SP::UI::Builders::Scene;
namespace BuilderWindow = SP::UI::Builders::Window;
using SP::UI::Builders::Diagnostics::PathSpaceError;
namespace Widgets = SP::UI::Builders::Widgets;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;
namespace WidgetFocus = SP::UI::Builders::Widgets::Focus;
namespace WidgetInput = SP::UI::Builders::Widgets::Input;

auto is_not_found(auto code) -> bool {
    return code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath;
}

auto drain_auto_render_queue(PathSpace& space,
                             std::string const& queue_path) -> std::vector<std::string> {
    std::vector<std::string> reasons;
    while (true) {
        auto event = space.take<AutoRenderRequestEvent, std::string>(queue_path);
        if (!event) {
            CHECK(is_not_found(event.error().code));
            break;
        }
        reasons.push_back(event->reason);
        if (reasons.size() > 4) {
            break;
        }
    }
    return reasons;
}

auto expect_auto_render_reason(std::vector<std::string> const& reasons,
                               std::string_view expected_reason) -> void {
    REQUIRE_FALSE(reasons.empty());
    bool seen_expected = false;
    for (auto const& reason : reasons) {
        if (reason == expected_reason) {
            seen_expected = true;
            continue;
        }
        CHECK(reason == "focus-navigation");
    }
    CHECK(seen_expected);
}

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

struct ScopedEnvVar {
    explicit ScopedEnvVar(std::string name, std::string value)
        : name_(std::move(name)) {
        if (auto* current = std::getenv(name_.c_str())) {
            had_previous_ = true;
            previous_value_ = current;
        }
        set(value.c_str());
    }

    ScopedEnvVar(ScopedEnvVar const&) = delete;
    ScopedEnvVar& operator=(ScopedEnvVar const&) = delete;

    ~ScopedEnvVar() {
        if (had_previous_) {
            set(previous_value_.c_str());
        } else {
            unset();
        }
    }

private:
    void set(char const* value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool had_previous_{false};
    std::string previous_value_;
};

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
    auto revision = BuilderScene::ReadCurrentRevision(fx.space, scene);
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
        auto windowResult = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
        REQUIRE(windowResult);
        window = *windowResult;

        auto attached = BuilderWindow::AttachSurface(fx.space, window, view_name, surface);
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

        auto present = BuilderWindow::Present(fx.space, window, view_name);
        if (!present) {
            INFO("BuilderWindow::Present error code = " << static_cast<int>(present.error().code));
            INFO("BuilderWindow::Present error message = " << present.error().message.value_or("<none>"));
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
    auto ready = BuilderScene::WaitUntilReady(fx.space, scenePath, std::chrono::milliseconds{10});
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
    auto scenePath = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    SceneRevisionDesc revision{};
    revision.revision     = 42;
    revision.published_at = std::chrono::system_clock::now();
    revision.author       = "tester";

    std::vector<std::byte> bucket(8, std::byte{0x1F});
    std::vector<std::byte> metadata(4, std::byte{0x2A});

    auto publish = BuilderScene::PublishRevision(fx.space,
                                          *scenePath,
                                          revision,
                                          std::span<const std::byte>(bucket.data(), bucket.size()),
                                          std::span<const std::byte>(metadata.data(), metadata.size()));
    REQUIRE(publish);

    auto wait = BuilderScene::WaitUntilReady(fx.space, *scenePath, std::chrono::milliseconds{10});
    REQUIRE(wait);

    auto current = BuilderScene::ReadCurrentRevision(fx.space, *scenePath);
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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

TEST_CASE("BuilderWindow::Present handles metal renderer targets") {
    BuildersFixture fx;

    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") != nullptr) {
        INFO("BuilderWindow::Present metal path exercised by dedicated PATHSPACE_ENABLE_METAL_UPLOADS UITest; skipping builders coverage");
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scene);

    publish_minimal_scene(fx, *scene);

    auto linked = Surface::SetScene(fx.space, *surface, *scene);
    REQUIRE(linked);

    WindowParams windowParams{ .name = "Main", .title = "Window", .width = 1024, .height = 768, .scale = 1.0f, .background = "#000" };
    auto window = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    auto attached = BuilderWindow::AttachSurface(fx.space, *window, "view", *surface);
    REQUIRE(attached);

    auto present = BuilderWindow::Present(fx.space, *window, "view");
    if (!present) {
        INFO("BuilderWindow::Present error code = " << static_cast<int>(present.error().code));
        INFO("BuilderWindow::Present error message = " << present.error().message.value_or("<none>"));
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

TEST_CASE("BuilderScene::Create is idempotent and preserves metadata") {
    BuildersFixture fx;

    SceneParams firstParams{ .name = "main", .description = "First description" };
    auto first = BuilderScene::Create(fx.space, fx.root_view(), firstParams);
    REQUIRE(first);

    SceneParams secondParams{ .name = "main", .description = "Second description" };
    auto second = BuilderScene::Create(fx.space, fx.root_view(), secondParams);
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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
    auto scenePath = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    auto initialState = BuilderScene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(initialState);
    CHECK(initialState->sequence == 0);
    CHECK(initialState->pending == BuilderScene::DirtyKind::None);

    auto seq1 = BuilderScene::MarkDirty(fx.space, *scenePath, BuilderScene::DirtyKind::Structure);
    REQUIRE(seq1);
    CHECK(*seq1 > 0);

    auto stateAfterFirst = BuilderScene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterFirst);
    CHECK(stateAfterFirst->sequence == *seq1);
    CHECK((stateAfterFirst->pending & BuilderScene::DirtyKind::Structure) == BuilderScene::DirtyKind::Structure);

    auto event1 = BuilderScene::TakeDirtyEvent(fx.space, *scenePath, std::chrono::milliseconds{20});
    REQUIRE(event1);
    CHECK(event1->sequence == *seq1);
    CHECK(event1->kinds == BuilderScene::DirtyKind::Structure);

    auto seq2 = BuilderScene::MarkDirty(fx.space, *scenePath, BuilderScene::DirtyKind::Visual | BuilderScene::DirtyKind::Text);
    REQUIRE(seq2);
    CHECK(*seq2 > *seq1);

    auto event2 = BuilderScene::TakeDirtyEvent(fx.space, *scenePath, std::chrono::milliseconds{20});
    REQUIRE(event2);
    CHECK(event2->sequence == *seq2);
    CHECK((event2->kinds & BuilderScene::DirtyKind::Visual) == BuilderScene::DirtyKind::Visual);
    CHECK((event2->kinds & BuilderScene::DirtyKind::Text) == BuilderScene::DirtyKind::Text);

    auto stateAfterSecond = BuilderScene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterSecond);
    CHECK(stateAfterSecond->sequence == *seq2);
    CHECK((stateAfterSecond->pending & BuilderScene::DirtyKind::Structure) == BuilderScene::DirtyKind::Structure);
    CHECK((stateAfterSecond->pending & BuilderScene::DirtyKind::Visual) == BuilderScene::DirtyKind::Visual);
    CHECK((stateAfterSecond->pending & BuilderScene::DirtyKind::Text) == BuilderScene::DirtyKind::Text);

    auto cleared = BuilderScene::ClearDirty(fx.space, *scenePath, BuilderScene::DirtyKind::Visual);
    REQUIRE(cleared);

    auto stateAfterClear = BuilderScene::ReadDirtyState(fx.space, *scenePath);
    REQUIRE(stateAfterClear);
    CHECK((stateAfterClear->pending & BuilderScene::DirtyKind::Visual) == BuilderScene::DirtyKind::None);
    CHECK((stateAfterClear->pending & BuilderScene::DirtyKind::Structure) == BuilderScene::DirtyKind::Structure);
    CHECK((stateAfterClear->pending & BuilderScene::DirtyKind::Text) == BuilderScene::DirtyKind::Text);
}

TEST_CASE("Scene dirty event wait-notify latency stays within budget") {
    using namespace std::chrono_literals;

    BuildersFixture fx;

    SceneParams sceneParams{ .name = "dirty_notify_scene", .description = "Dirty notifications" };
    auto scenePath = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
    REQUIRE(scenePath);

    std::atomic<bool> waiterReady{false};
    bool              eventSucceeded = false;
    BuilderScene::DirtyEvent event{};
    std::chrono::milliseconds observedLatency{0};

    auto wait_timeout = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{1000}, 1.0);
    if (wait_timeout < std::chrono::milliseconds{500}) {
        wait_timeout = std::chrono::milliseconds{500};
    }

    std::thread waiter([&]() {
        waiterReady.store(true, std::memory_order_release);
        auto start = std::chrono::steady_clock::now();
        auto taken = BuilderScene::TakeDirtyEvent(fx.space, *scenePath, wait_timeout);
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

    auto seq = BuilderScene::MarkDirty(fx.space, *scenePath, BuilderScene::DirtyKind::Structure);
    REQUIRE(seq);

    waiter.join();

    REQUIRE(eventSucceeded);
    CHECK(event.sequence == *seq);
    CHECK(event.kinds == BuilderScene::DirtyKind::Structure);
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
    auto window = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    auto attached = BuilderWindow::AttachSurface(fx.space, *window, "view", *surface);
    REQUIRE(attached);

    auto surfaceBinding = read_value<std::string>(fx.space, std::string(window->getPath()) + "/views/view/surface");
    REQUIRE(surfaceBinding);
    CHECK(*surfaceBinding == "surfaces/pane");

    auto present = BuilderWindow::Present(fx.space, *window, "view");
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

TEST_CASE("BuilderWindow::AttachSurface enforces shared app roots") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceParams surfaceParams{ .name = "pane", .desc = {}, .renderer = "renderers/2d" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    WindowParams windowParams{ .name = "Main", .title = "app", .width = 800, .height = 600, .scale = 1.0f, .background = "#000" };
    auto window = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);

    SurfacePath foreignSurface{ "/system/applications/other_app/surfaces/pane" };
    auto attached = BuilderWindow::AttachSurface(fx.space, *window, "view", foreignSurface);
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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

    auto params = Widgets::MakeButtonParams("primary", "Primary")
                      .ModifyStyle([](Widgets::ButtonStyle& style) {
                          style.width = 180.0f;
                          style.height = 44.0f;
                      })
                      .Build();

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

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision > 0);

    auto read_scene_bucket = [&](SP::UI::Builders::ScenePath const& scene) {
        auto stateRevision = BuilderScene::ReadCurrentRevision(fx.space, scene);
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

    auto updatedRevision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
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

    auto defaultTheme = Widgets::MakeDefaultWidgetTheme();
    auto params = Widgets::MakeButtonParams("button_hot_swap", "Theme Swap")
                      .WithTheme(defaultTheme)
                      .Build();

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
        auto revision = BuilderScene::ReadCurrentRevision(fx.space, scene);
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
            auto event = BuilderScene::TakeDirtyEvent(fx.space, scene, std::chrono::milliseconds{1});
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

    auto params = Widgets::MakeToggleParams("toggle_primary")
                      .ModifyStyle([](Widgets::ToggleStyle& style) {
                          style.width = 60.0f;
                          style.height = 32.0f;
                          style.track_on_color = {0.2f, 0.6f, 0.3f, 1.0f};
                      })
                      .Build();

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
        auto rev = BuilderScene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
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

    auto updatedRevision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto toggle_unchanged = Widgets::UpdateToggleState(fx.space, *created, toggled);
    REQUIRE(toggle_unchanged);
    CHECK_FALSE(*toggle_unchanged);
}

TEST_CASE("Widgets::CreateSlider publishes snapshot and state") {
    BuildersFixture fx;

    auto params = Widgets::MakeSliderParams("slider_primary")
                      .WithRange(-1.0f, 1.0f)
                      .WithValue(0.25f)
                      .WithStep(0.25f)
                      .ModifyStyle([](Widgets::SliderStyle& style) {
                          style.width = 320.0f;
                          style.height = 36.0f;
                          style.track_height = 8.0f;
                          style.thumb_radius = 14.0f;
                      })
                      .Build();

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
        auto rev = BuilderScene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
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

    auto updatedRevision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
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

    auto buttonParams = Widgets::MakeButtonParams("primary_button", "Primary").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    CAPTURE(buttonStyle->width);
    CAPTURE(buttonStyle->height);
    auto buttonFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                               0.0f,
                                                               buttonStyle->width,
                                                               buttonStyle->height);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       buttonFootprint);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(12.0f, 6.0f)
                       .WithInside(true);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    auto hovered = Widgets::MakeButtonState()
                        .WithHovered(true)
                        .Build();

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

    auto pressed = Widgets::MakeButtonState()
                        .WithHovered(true)
                        .WithPressed(true)
                        .Build();

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
    auto tile = static_cast<float>(desc.progressive_tile_size_px);
    auto expected_width = std::ceil(buttonStyle->width / tile) * tile;
    auto expected_height = std::ceil(buttonStyle->height / tile) * tile;
    CHECK(hint.min_x == doctest::Approx(0.0f));
    CHECK(hint.min_y == doctest::Approx(0.0f));
    CHECK(hint.max_x == doctest::Approx(expected_width));
    CHECK(hint.max_y == doctest::Approx(expected_height));

    auto pressReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(pressReasons, "widget/button");

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

    auto releaseReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(releaseReasons, "widget/button");

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

    auto hoverReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(hoverReasons.empty());

    Widgets::ButtonState disabled = released;
    disabled.enabled = false;

    auto disableResult = Widgets::UpdateButtonState(fx.space, *button, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(disableReasons.empty());

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto disabledState = read_value<Widgets::ButtonState>(fx.space, std::string(button->state.getPath()));
    REQUIRE(disabledState);
    CHECK_FALSE(disabledState->enabled);
}

TEST_CASE("Widgets::Bindings::DispatchButton invokes action callbacks") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "bindings_button_callback_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 128};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "bindings_button_callback_surface", .desc = desc, .renderer = "renderers/bindings_button_callback_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/bindings_button_callback_surface");
    REQUIRE(target);

    auto buttonParams = Widgets::MakeButtonParams("callback_button", "Callback").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    auto buttonFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                               0.0f,
                                                               buttonStyle->width,
                                                               buttonStyle->height);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       buttonFootprint);
    REQUIRE(binding);

    std::vector<WidgetReducers::WidgetAction> observed;
    int secondary_invocations = 0;

    WidgetBindings::AddActionCallback(*binding, [&](WidgetReducers::WidgetAction const& action) {
        observed.push_back(action);
    });
    WidgetBindings::AddActionCallback(*binding, [&](WidgetReducers::WidgetAction const&) {
        ++secondary_invocations;
    });

    auto pointer = WidgetBindings::PointerInfo::Make(12.0f, 6.0f)
                       .WithInside(true);

    auto opQueuePath = binding->options.ops_queue.getPath();

    auto pressed = Widgets::MakeButtonState()
                        .WithHovered(true)
                        .WithPressed(true)
                        .Build();

    auto pressResult = WidgetBindings::DispatchButton(fx.space,
                                                      *binding,
                                                      pressed,
                                                      WidgetBindings::WidgetOpKind::Press,
                                                      pointer);
    REQUIRE(pressResult);
    CHECK(*pressResult);

    CHECK(observed.size() == 1);
    CHECK(observed.front().kind == WidgetBindings::WidgetOpKind::Press);
    CHECK(observed.front().analog_value == doctest::Approx(1.0f));
    CHECK(observed.front().widget_path == binding->widget.root.getPath());
    CHECK(secondary_invocations == 1);

    auto pressOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(pressOp);
    CHECK(pressOp->kind == WidgetBindings::WidgetOpKind::Press);

    WidgetBindings::ClearActionCallbacks(*binding);

    auto released = pressed;
    released.pressed = false;

    auto releaseResult = WidgetBindings::DispatchButton(fx.space,
                                                        *binding,
                                                        released,
                                                        WidgetBindings::WidgetOpKind::Release,
                                                        pointer);
    REQUIRE(releaseResult);
    CHECK(*releaseResult);

    CHECK(observed.size() == 1);
    CHECK(secondary_invocations == 1);

    auto releaseOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(releaseOp);
    CHECK(releaseOp->kind == WidgetBindings::WidgetOpKind::Release);
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

    auto buttonParams = Widgets::MakeButtonParams("manual_button", "Manual").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    auto buttonFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                               0.0f,
                                                               buttonStyle->width,
                                                               buttonStyle->height);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       buttonFootprint,
                                                       std::nullopt,
                                                       /*auto_render=*/false);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(4.0f, 3.0f)
                       .WithInside(true);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    auto hover = Widgets::MakeButtonState()
                      .WithHovered(true)
                      .Build();

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

    auto toggleParams = Widgets::MakeToggleParams("primary_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto toggleStyle = fx.space.read<Widgets::ToggleStyle, std::string>(std::string(toggle->root.getPath()) + "/meta/style");
    REQUIRE(toggleStyle);
    auto toggleFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                               0.0f,
                                                               toggleStyle->width,
                                                               toggleStyle->height);

    auto binding = WidgetBindings::CreateToggleBinding(fx.space,
                                                       fx.root_view(),
                                                       *toggle,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       toggleFootprint);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(18.0f, 12.0f)
                       .WithInside(true);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    auto hoverState = Widgets::MakeToggleState()
                           .WithHovered(true)
                           .Build();

    auto hoverEnter = WidgetBindings::DispatchToggle(fx.space,
                                                     *binding,
                                                     hoverState,
                                                     WidgetBindings::WidgetOpKind::HoverEnter,
                                                     pointer);
    REQUIRE(hoverEnter);
    CHECK(*hoverEnter);

    auto hoverReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(hoverReasons, "widget/toggle");

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::HoverEnter);
    CHECK(hoverOp->value == doctest::Approx(0.0f));

    auto toggledState = Widgets::MakeToggleState()
                            .WithHovered(true)
                            .WithChecked(true)
                            .Build();

    auto toggleResult = WidgetBindings::DispatchToggle(fx.space,
                                                       *binding,
                                                       toggledState,
                                                       WidgetBindings::WidgetOpKind::Toggle,
                                                       pointer);
    REQUIRE(toggleResult);
    CHECK(*toggleResult);

    auto toggleReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(toggleReasons, "widget/toggle");

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

    auto hoverExitReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(hoverExitReasons, "widget/toggle");

    auto hoverExitOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverExitOp);
    CHECK(hoverExitOp->kind == WidgetBindings::WidgetOpKind::HoverExit);
    CHECK(hoverExitOp->value == doctest::Approx(1.0f));

    Widgets::ToggleState disabledState = hoverExitState;
    disabledState.enabled = false;

    auto disableResult = Widgets::UpdateToggleState(fx.space, *toggle, disabledState);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(disableReasons.empty());

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

    auto leftParams = Widgets::MakeButtonParams("left_button", "Left")
                          .ModifyStyle([](Widgets::ButtonStyle& style) {
                              style.width = 96.0f;
                              style.height = 64.0f;
                          })
                          .Build();
    auto left = Widgets::CreateButton(fx.space, fx.root_view(), leftParams);
    REQUIRE(left);

    auto rightParams = Widgets::MakeButtonParams("right_button", "Right")
                           .ModifyStyle([](Widgets::ButtonStyle& style) {
                               style.width = 96.0f;
                               style.height = 64.0f;
                           })
                           .Build();
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

    auto pointer = WidgetBindings::PointerInfo::Make(12.0f, 8.0f)
                       .WithInside(true);

    auto hover = Widgets::MakeButtonState()
                      .WithHovered(true)
                      .Build();

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

    auto sliderParams = Widgets::MakeSliderParams("volume")
                             .WithMaximum(1.0f)
                             .WithValue(0.25f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto sliderStyle = fx.space.read<Widgets::SliderStyle, std::string>(std::string(slider->root.getPath()) + "/meta/style");
    REQUIRE(sliderStyle);
    auto sliderFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                               0.0f,
                                                               sliderStyle->width,
                                                               sliderStyle->height);

    auto binding = WidgetBindings::CreateSliderBinding(fx.space,
                                                       fx.root_view(),
                                                       *slider,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       sliderFootprint);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(120.0f, 12.0f)
                       .WithPrimary(true);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto opQueuePath = binding->options.ops_queue.getPath();

    auto beginState = Widgets::MakeSliderState()
                          .WithEnabled(true)
                          .WithDragging(true)
                          .WithValue(0.15f)
                          .Build();

    auto beginResult = WidgetBindings::DispatchSlider(fx.space,
                                                      *binding,
                                                      beginState,
                                                      WidgetBindings::WidgetOpKind::SliderBegin,
                                                      pointer);
    REQUIRE(beginResult);
    CHECK(*beginResult);

    auto beginReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(beginReasons, "widget/slider");

    auto beginOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(beginOp);
    CHECK(beginOp->kind == WidgetBindings::WidgetOpKind::SliderBegin);
    CHECK(beginOp->value == doctest::Approx(0.15f));

    auto dragState = Widgets::MakeSliderState()
                         .WithEnabled(true)
                         .WithDragging(true)
                         .WithValue(2.0f)
                         .Build();

    auto updateResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *binding,
                                                       dragState,
                                                       WidgetBindings::WidgetOpKind::SliderUpdate,
                                                       pointer);
    REQUIRE(updateResult);
    CHECK(*updateResult);

    auto updateReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(updateReasons, "widget/slider");

    auto updateOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(updateOp);
    CHECK(updateOp->kind == WidgetBindings::WidgetOpKind::SliderUpdate);
    CHECK(updateOp->value == doctest::Approx(1.0f));

    auto commitState = Widgets::MakeSliderState()
                           .WithEnabled(true)
                           .WithDragging(false)
                           .WithValue(0.6f)
                           .Build();

    auto commitResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *binding,
                                                       commitState,
                                                       WidgetBindings::WidgetOpKind::SliderCommit,
                                                       pointer);
    REQUIRE(commitResult);
    CHECK(*commitResult);

    auto commitReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(commitReasons, "widget/slider");

    auto commitOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(commitOp);
    CHECK(commitOp->kind == WidgetBindings::WidgetOpKind::SliderCommit);
    CHECK(commitOp->value == doctest::Approx(0.6f));

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(std::string(target->getPath()) + "/hints/dirtyRects");
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());

    auto noExtraReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(noExtraReasons.empty());

    auto disabled = Widgets::MakeSliderState()
                         .WithEnabled(false)
                         .WithValue(0.6f)
                         .Build();

    auto disableResult = Widgets::UpdateSliderState(fx.space, *slider, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(disableReasons.empty());

    auto disableOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    CHECK_FALSE(disableOp);
    if (!disableOp) {
        CHECK((disableOp.error().code == Error::Code::NoObjectFound || disableOp.error().code == Error::Code::NoSuchPath));
    }

    auto storedState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(storedState);
    CHECK_FALSE(storedState->enabled);
}

TEST_CASE("WidgetInput slider helpers dispatch slider ops and respect deadzone") {
    BuildersFixture fx;

    RendererParams rendererParams{
        .name = "slider_helper_renderer",
        .kind = RendererKind::Software2D,
        .description = "Renderer for slider helpers",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 192};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{
        .name = "slider_helper_surface",
        .desc = desc,
        .renderer = "renderers/slider_helper_renderer",
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/slider_helper_surface");
    REQUIRE(target);

    auto sliderParams = Widgets::MakeSliderParams("volume_slider_helper")
                             .WithMaximum(1.0f)
                             .WithValue(0.25f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto sliderStyleStored = fx.space.read<Widgets::SliderStyle, std::string>(
        std::string(slider->root.getPath()) + "/meta/style");
    REQUIRE(sliderStyleStored);
    Widgets::SliderStyle slider_style = *sliderStyleStored;

    auto sliderRangeStored = fx.space.read<Widgets::SliderRange, std::string>(
        std::string(slider->range.getPath()));
    REQUIRE(sliderRangeStored);
    Widgets::SliderRange slider_range = *sliderRangeStored;

    auto sliderStateStored = fx.space.read<Widgets::SliderState, std::string>(
        std::string(slider->state.getPath()));
    REQUIRE(sliderStateStored);
    Widgets::SliderState slider_state = *sliderStateStored;

    auto sliderFootprint = SP::UI::Builders::MakeDirtyRectHint(
        0.0f, 0.0f, slider_style.width, slider_style.height);

    auto bindingExpected = WidgetBindings::CreateSliderBinding(fx.space,
                                                               fx.root_view(),
                                                               *slider,
                                                               SP::ConcretePathStringView{target->getPath()},
                                                               sliderFootprint);
    REQUIRE(bindingExpected);
    auto slider_binding = std::move(*bindingExpected);
    auto slider_paths = *slider;

    auto opQueuePath = slider_binding.options.ops_queue.getPath();

    WidgetInput::WidgetInputContext input{};
    input.space = &fx.space;

    WidgetInput::LayoutSnapshot layout{};
    WidgetInput::SliderLayout slider_layout{};
    slider_layout.bounds = WidgetInput::WidgetBounds{0.0f, 0.0f, slider_style.width, slider_style.height};
    float track_min_y = (slider_style.height - slider_style.track_height) * 0.5f;
    float track_max_y = track_min_y + slider_style.track_height;
    slider_layout.track = WidgetInput::WidgetBounds{0.0f, track_min_y, slider_style.width, track_max_y};
    layout.slider = slider_layout;
    layout.slider_footprint = WidgetInput::WidgetBounds{
        sliderFootprint.min_x,
        sliderFootprint.min_y,
        sliderFootprint.max_x,
        sliderFootprint.max_y,
    };
    input.layout = layout;

    auto focus_config = WidgetFocus::MakeConfig(fx.root_view());
    WidgetInput::FocusTarget focus_target = WidgetInput::FocusTarget::Slider;
    std::array<WidgetInput::FocusTarget, 1> focus_order{WidgetInput::FocusTarget::Slider};
    int focus_list_index = 0;
    int focus_tree_index = 0;

    input.focus.config = &focus_config;
    input.focus.current = &focus_target;
    input.focus.order = std::span<const WidgetInput::FocusTarget>{focus_order.data(), focus_order.size()};
    input.focus.slider = slider_paths.root;
    input.focus.focus_list_index = &focus_list_index;
    input.focus.focus_tree_index = &focus_tree_index;

    input.slider_binding = &slider_binding;
    input.slider_paths = &slider_paths;
    input.slider_state = &slider_state;
    input.slider_style = &slider_style;
    input.slider_range = &slider_range;

    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_down = false;
    bool slider_dragging = false;
    std::string tree_pointer_down_id;
    bool tree_pointer_toggle = false;

    input.pointer_x = &pointer_x;
    input.pointer_y = &pointer_y;
    input.pointer_down = &pointer_down;
    input.slider_dragging = &slider_dragging;
    input.tree_pointer_down_id = &tree_pointer_down_id;
    input.tree_pointer_toggle = &tree_pointer_toggle;

    auto start_pointer = WidgetInput::SliderPointerForValue(input, slider_state.value);
    pointer_x = start_pointer.first;
    pointer_y = start_pointer.second;

    auto drain_ops = [&]() {
        while (true) {
            auto op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
            if (!op) {
                auto code = op.error().code;
                CHECK((code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath));
                break;
            }
        }
    };

    drain_ops();

    float base_value = slider_state.value;
    float step = WidgetInput::SliderStep(input);
    REQUIRE(step > 0.0f);

    auto keyboard_update = WidgetInput::AdjustSliderByStep(input, 1);
    CHECK(keyboard_update.state_changed);

    auto update_op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(update_op);
    CHECK(update_op->kind == WidgetBindings::WidgetOpKind::SliderUpdate);

    auto commit_op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(commit_op);
    CHECK(commit_op->kind == WidgetBindings::WidgetOpKind::SliderCommit);
    CHECK(commit_op->value == doctest::Approx(base_value + step));

    CHECK(slider_state.value == doctest::Approx(base_value + step));

    drain_ops();

    auto reset_state = Widgets::MakeSliderState()
                           .WithEnabled(true)
                           .WithValue(base_value)
                           .Build();
    auto reset_result = Widgets::UpdateSliderState(fx.space, slider_paths, reset_state);
    REQUIRE(reset_result);
    slider_state = reset_state;

    auto pointer_reset = WidgetInput::SliderPointerForValue(input, slider_state.value);
    pointer_x = pointer_reset.first;
    pointer_y = pointer_reset.second;

    drain_ops();

    float axis = 0.5f;
    auto analog_update = WidgetInput::AdjustSliderAnalog(input, axis);
    CHECK(analog_update.state_changed);

    auto analog_update_op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(analog_update_op);
    CHECK(analog_update_op->kind == WidgetBindings::WidgetOpKind::SliderUpdate);

    auto analog_commit_op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(analog_commit_op);
    CHECK(analog_commit_op->kind == WidgetBindings::WidgetOpKind::SliderCommit);

    float normalized = (std::abs(axis) - 0.1f) / (1.0f - 0.1f);
    float expected_delta = step * normalized;
    CHECK(analog_commit_op->value == doctest::Approx(base_value + expected_delta));
    CHECK(slider_state.value == doctest::Approx(base_value + expected_delta));

    drain_ops();

    auto deadzone_update = WidgetInput::AdjustSliderAnalog(input, 0.05f);
    CHECK_FALSE(deadzone_update.state_changed);

    auto deadzone_op = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
   CHECK_FALSE(deadzone_op);
   if (!deadzone_op) {
       CHECK((deadzone_op.error().code == Error::Code::NoObjectFound || deadzone_op.error().code == Error::Code::NoSuchPath));
   }
}

TEST_CASE("WidgetInput tree pointer events select rows at translated origin") {
    BuildersFixture fx;

    auto theme = Widgets::MakeDefaultWidgetTheme();
    std::vector<Widgets::TreeNode> nodes{
        Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "child", .parent_id = "root", .label = "Child", .enabled = true, .expandable = false, .loaded = true},
    };

    auto treeParams = Widgets::MakeTreeParams("input_tree")
                          .WithTheme(theme)
                          .WithNodes(nodes)
                          .Build();
    auto treePathsExpected = Widgets::CreateTree(fx.space, fx.root_view(), treeParams);
    REQUIRE(treePathsExpected);
    auto treePaths = *treePathsExpected;

    auto expandedState = Widgets::MakeTreeState()
                             .WithExpandedIds({"root"})
                             .Build();
    REQUIRE(Widgets::UpdateTreeState(fx.space, treePaths, expandedState));

    auto treeStyleStored = fx.space.read<Widgets::TreeStyle, std::string>(
        std::string(treePaths.root.getPath()) + "/meta/style");
    REQUIRE(treeStyleStored);
    auto treeStyle = *treeStyleStored;

    auto treeStateStored = fx.space.read<Widgets::TreeState, std::string>(treePaths.state.getPath());
    REQUIRE(treeStateStored);
    auto treeState = *treeStateStored;

    auto treeNodesStored = fx.space.read<std::vector<Widgets::TreeNode>, std::string>(treePaths.nodes.getPath());
    REQUIRE(treeNodesStored);
    auto treeNodes = *treeNodesStored;

    auto preview = Widgets::BuildTreePreview(treeStyle,
                                             treeNodes,
                                             treeState,
                                             Widgets::TreePreviewOptions{.authoring_root = "test/tree"});

    auto treeLayoutOpt = WidgetInput::MakeTreeLayout(preview.layout);
    REQUIRE(treeLayoutOpt);
    auto treeLayout = *treeLayoutOpt;

    float tree_left = 80.0f;
    float tree_top = 120.0f;
    WidgetInput::TranslateTreeLayout(treeLayout, tree_left, tree_top);

    WidgetInput::LayoutSnapshot layout{};
    layout.tree = treeLayout;
    layout.tree_footprint = treeLayout.bounds;

    WidgetBindings::ButtonBinding dummy_button{};
    WidgetBindings::ToggleBinding dummy_toggle{};
    WidgetBindings::SliderBinding dummy_slider{};
    WidgetBindings::ListBinding dummy_list{};

    Widgets::ButtonPaths button_paths{};
    Widgets::TogglePaths toggle_paths{};
    Widgets::SliderPaths slider_paths{};
    Widgets::ListPaths list_paths{};

    Widgets::ButtonState button_state{};
    Widgets::ToggleState toggle_state{};
    Widgets::SliderState slider_state{};
    Widgets::ListState list_state{};

    WidgetInput::WidgetBounds zero_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    auto zero_hint = WidgetInput::MakeDirtyHint(zero_bounds);
    SP::ConcretePathString target_path{std::string(fx.app_root.getPath()) + "/renderers/test_target"};
    auto treeBindingExpected = WidgetBindings::CreateTreeBinding(fx.space,
                                                                 fx.root_view(),
                                                                 treePaths,
                                                                 SP::ConcretePathStringView{target_path.getPath()},
                                                                 zero_hint,
                                                                 zero_hint,
                                                                 false);
    REQUIRE(treeBindingExpected);
    auto treeBinding = *treeBindingExpected;

    auto focus_config = WidgetFocus::MakeConfig(fx.root_view());
    WidgetInput::FocusTarget focus_target = WidgetInput::FocusTarget::Tree;
    std::array focus_order{WidgetInput::FocusTarget::Tree};
    int focus_list_index = 0;
    int focus_tree_index = 0;

    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_down = false;
    bool slider_dragging = false;
    std::string tree_pointer_down_id;
    bool tree_pointer_toggle = false;

    WidgetInput::WidgetInputContext input{};
    input.space = &fx.space;
    input.layout = layout;
    input.focus.config = &focus_config;
    input.focus.current = &focus_target;
    input.focus.order = std::span<const WidgetInput::FocusTarget>(focus_order.data(), focus_order.size());
    input.focus.button = button_paths.root;
    input.focus.toggle = toggle_paths.root;
    input.focus.slider = slider_paths.root;
    input.focus.list = list_paths.root;
    input.focus.tree = treePaths.root;
    input.focus.focus_list_index = &focus_list_index;
    input.focus.focus_tree_index = &focus_tree_index;
    input.button_binding = &dummy_button;
    input.button_paths = &button_paths;
    input.button_state = &button_state;
    input.toggle_binding = &dummy_toggle;
    input.toggle_paths = &toggle_paths;
    input.toggle_state = &toggle_state;
    input.slider_binding = &dummy_slider;
    input.slider_paths = &slider_paths;
    input.slider_state = &slider_state;
    input.list_binding = &dummy_list;
    input.list_paths = &list_paths;
    input.list_state = &list_state;
    input.tree_binding = &treeBinding;
    input.tree_paths = &treePaths;
    input.tree_state = &treeState;
    input.tree_style = &treeStyle;
    input.tree_nodes = &treeNodes;
    input.pointer_x = &pointer_x;
    input.pointer_y = &pointer_y;
    input.pointer_down = &pointer_down;
    input.slider_dragging = &slider_dragging;
    input.tree_pointer_down_id = &tree_pointer_down_id;
    input.tree_pointer_toggle = &tree_pointer_toggle;

    REQUIRE(treeLayout.rows.size() >= 2);
    auto const& target_row = treeLayout.rows[1];
    auto const pointer_x_target = target_row.toggle.max_x + 16.0f;
    auto const pointer_y_target = target_row.bounds.min_y + treeLayout.row_height * 0.5f;

    auto move_update = WidgetInput::HandlePointerMove(input, pointer_x_target, pointer_y_target);
    (void)move_update;
    auto down_update = WidgetInput::HandlePointerDown(input);
    (void)down_update;
    auto up_update = WidgetInput::HandlePointerUp(input);
    (void)up_update;

    auto updated_state = fx.space.read<Widgets::TreeState, std::string>(treePaths.state.getPath());
    REQUIRE(updated_state);
    CHECK_EQ(updated_state->selected_id, "child");
    CHECK_EQ(updated_state->hovered_id, "child");
}

TEST_CASE("Widgets::CreateList publishes snapshot and metadata") {
    BuildersFixture fx;

    auto listParams = Widgets::MakeListParams("inventory")
                          .WithItems({
                              Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
                              Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
                              Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = false},
                          })
                          .ModifyStyle([](Widgets::ListStyle& style) {
                              style.width = 220.0f;
                              style.item_height = 40.0f;
                          })
                          .Build();

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
        auto rev = BuilderScene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::CreateTextField publishes snapshot and metadata") {
    BuildersFixture fx;

    Widgets::TextFieldParams params{};
    params.name = "username";
    params.style.width = 280.0f;
    params.style.height = 52.0f;
    params.style.padding_x = 16.0f;
    params.style.padding_y = 14.0f;
    params.state.text = "guest";
    params.state.cursor = 5;
    params.state.selection_start = 1;
    params.state.selection_end = 3;

    auto created = Widgets::CreateTextField(fx.space, fx.root_view(), params);
    REQUIRE(created);

    auto state = read_value<Widgets::TextFieldState>(fx.space, created->state.getPath());
    REQUIRE(state);
    CHECK(state->text == "guest");
    CHECK(state->cursor == 5);
    CHECK(state->selection_start == 1);
    CHECK(state->selection_end == 3);
    CHECK(state->focused == params.state.focused);

    auto stylePath = std::string(created->root.getPath()) + "/meta/style";
    auto storedStyle = read_value<Widgets::TextFieldStyle>(fx.space, stylePath);
    REQUIRE(storedStyle);
    CHECK(storedStyle->width >= 280.0f);
    CHECK(storedStyle->height >= params.style.height);
    CHECK(storedStyle->padding_x == doctest::Approx(16.0f));

    auto footprint = read_value<DirtyRectHint>(fx.space,
                                               std::string(created->root.getPath()) + "/meta/footprint");
    REQUIRE(footprint);
    CHECK(footprint->max_x > footprint->min_x);
    CHECK(footprint->max_y > footprint->min_y);

    auto ensure_state_scene = [&](SP::UI::Builders::ScenePath const& scene) {
        auto rev = BuilderScene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::CreateTextArea publishes snapshot and metadata") {
    BuildersFixture fx;

    Widgets::TextAreaParams params{};
    params.name = "notes";
    params.style.width = 360.0f;
    params.style.height = 240.0f;
    params.style.line_spacing = 4.0f;
    params.state.text = "Line 1\nLine 2";
    params.state.cursor = 7;
    params.state.selection_start = 0;
    params.state.selection_end = 5;
    params.state.scroll_y = 12.0f;

    auto created = Widgets::CreateTextArea(fx.space, fx.root_view(), params);
    REQUIRE(created);

    auto state = read_value<Widgets::TextAreaState>(fx.space, created->state.getPath());
    REQUIRE(state);
    CHECK(state->text == "Line 1\nLine 2");
    CHECK(state->cursor == 7);
    CHECK(state->selection_end == 5);
    CHECK(state->scroll_y == doctest::Approx(12.0f));

    auto stylePath = std::string(created->root.getPath()) + "/meta/style";
    auto storedStyle = read_value<Widgets::TextAreaStyle>(fx.space, stylePath);
    REQUIRE(storedStyle);
    CHECK(storedStyle->width >= 360.0f);
    CHECK(storedStyle->height >= 240.0f);
    CHECK(storedStyle->line_spacing == doctest::Approx(4.0f));

    auto footprint = read_value<DirtyRectHint>(fx.space,
                                               std::string(created->root.getPath()) + "/meta/footprint");
    REQUIRE(footprint);
    CHECK(footprint->max_x > footprint->min_x);
    CHECK(footprint->max_y > footprint->min_y);

    auto ensure_state_scene = [&](SP::UI::Builders::ScenePath const& scene) {
        auto rev = BuilderScene::ReadCurrentRevision(fx.space, scene);
        REQUIRE(rev);
        CHECK(rev->revision > 0);
    };
    ensure_state_scene(created->states.idle);
    ensure_state_scene(created->states.hover);
    ensure_state_scene(created->states.pressed);
    ensure_state_scene(created->states.disabled);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::BuildButtonPreview provides canonical authoring ids and highlight control") {
    Widgets::ButtonStyle style{};
    style.width = 180.0f;
    style.height = 48.0f;
    style.corner_radius = 9.0f;

    auto focused = Widgets::MakeButtonState()
                        .WithFocused(true)
                        .Build();

    auto preview = Widgets::BuildButtonPreview(
        style,
        focused,
        Widgets::ButtonPreviewOptions{
            .authoring_root = "widgets/test/button",
            .label = "Preview Button",
            .pulsing_highlight = true,
        });

    REQUIRE(preview.drawable_ids.size() >= 2);
    REQUIRE(preview.authoring_map.size() >= 2);
    CHECK(preview.authoring_map.front().authoring_node_id
          == "widgets/test/button/authoring/button/background");
    auto highlight_it = std::find_if(preview.authoring_map.begin(),
                                     preview.authoring_map.end(),
                                     [](auto const& entry) {
                                         return entry.authoring_node_id
                                             == "widgets/test/button/authoring/focus/highlight";
                                     });
    REQUIRE(highlight_it != preview.authoring_map.end());
    auto highlight_index = static_cast<std::size_t>(std::distance(preview.authoring_map.begin(),
                                                                  highlight_it));
    REQUIRE(highlight_index < preview.pipeline_flags.size());
    CHECK(preview.pipeline_flags[highlight_index] == SP::UI::PipelineFlags::HighlightPulse);
    auto label_it = std::find_if(preview.authoring_map.begin(),
                                 preview.authoring_map.end(),
                                 [](auto const& entry) {
                                     return entry.authoring_node_id
                                         == "widgets/test/button/authoring/button/label";
                                 });
    REQUIRE(label_it != preview.authoring_map.end());
    CHECK(preview.bounds_boxes.front().max[0] == doctest::Approx(style.width));
    CHECK(preview.bounds_boxes.front().max[1] == doctest::Approx(style.height));

    auto no_pulse = Widgets::BuildButtonPreview(
        style,
        focused,
        Widgets::ButtonPreviewOptions{
            .authoring_root = "widgets/test/button",
            .label = "Preview Button",
            .pulsing_highlight = false,
        });
    REQUIRE_FALSE(no_pulse.pipeline_flags.empty());
    REQUIRE(highlight_index < no_pulse.pipeline_flags.size());
    CHECK(no_pulse.pipeline_flags[highlight_index] == 0u);
}

TEST_CASE("Widgets::BuildLabel produces text bucket and bounds") {
    Widgets::TypographyStyle typography{};
    typography.font_size = 20.0f;
    typography.line_height = 24.0f;
    auto params = Widgets::LabelBuildParams::Make("Label", typography)
                      .WithOrigin(12.0f, 34.0f)
                      .WithColor({0.9f, 0.1f, 0.2f, 1.0f})
                      .WithDrawable(0xDEADBEEF, std::string("widgets/test/label"), 0.25f);

    auto label = Widgets::BuildLabel(params);
    REQUIRE(label);
    CHECK_FALSE(label->bucket.drawable_ids.empty());
    CHECK(label->bucket.drawable_ids.front() == params.drawable_id);
    CHECK_FALSE(label->bucket.command_kinds.empty());

    auto bounds = Widgets::LabelBounds(*label);
    REQUIRE(bounds);
    CHECK(bounds->width() > 0.0f);
    CHECK(bounds->height() > 0.0f);
    CHECK(bounds->min_x <= params.origin_x);
    CHECK(bounds->max_x >= params.origin_x);
    CHECK(bounds->max_y >= params.origin_y);
}

TEST_CASE("Widgets::BuildTogglePreview emits drawable ordering and highlight metadata") {
    Widgets::ToggleStyle style{};
    style.width = 72.0f;
    style.height = 36.0f;

    auto state = Widgets::MakeToggleState()
                     .WithChecked(true)
                     .WithFocused(true)
                     .WithHovered(true)
                     .Build();

    auto preview = Widgets::BuildTogglePreview(
        style,
        state,
        Widgets::TogglePreviewOptions{
            .authoring_root = "widgets/test/toggle",
            .pulsing_highlight = false,
        });

    REQUIRE(preview.drawable_ids.size() == 3);
    CHECK(preview.bounds_boxes.front().min[0] == doctest::Approx(0.0f));
    CHECK(preview.bounds_boxes.front().max[0] == doctest::Approx(style.width));
    REQUIRE(preview.authoring_map.size() == 3);
    CHECK(preview.authoring_map[0].authoring_node_id
          == "widgets/test/toggle/authoring/toggle/track");
    CHECK(preview.authoring_map[1].authoring_node_id
          == "widgets/test/toggle/authoring/toggle/thumb");
    CHECK(preview.authoring_map[2].authoring_node_id
          == "widgets/test/toggle/authoring/focus/highlight");
    REQUIRE_FALSE(preview.pipeline_flags.empty());
    CHECK(preview.pipeline_flags.back() == 0u);
}

TEST_CASE("Widgets::BuildSliderPreview clamps range and records fill geometry") {
    Widgets::SliderStyle style{};
    style.width = 200.0f;
    style.height = 32.0f;
    style.track_height = 8.0f;
    style.thumb_radius = 10.0f;

    Widgets::SliderRange range{};
    range.minimum = -50.0f;
    range.maximum = 50.0f;
    range.step = 5.0f;

    auto state = Widgets::MakeSliderState()
                     .WithValue(17.0f)
                     .WithFocused(true)
                     .Build();

    auto preview = Widgets::BuildSliderPreview(
        style,
        range,
        state,
        Widgets::SliderPreviewOptions{
            .authoring_root = "widgets/test/slider",
            .pulsing_highlight = false,
        });

    REQUIRE(preview.drawable_ids.size() == 4);
    REQUIRE(preview.bounds_boxes.size() >= 3);
    float clamped_value = 15.0f; // step should clamp to nearest 5
    float progress = (clamped_value - range.minimum) / (range.maximum - range.minimum);
    CHECK(preview.bounds_boxes[1].max[0]
          == doctest::Approx(progress * style.width).epsilon(1e-3f));
    REQUIRE(preview.authoring_map.size() == 4);
    CHECK(preview.authoring_map[0].authoring_node_id
          == "widgets/test/slider/authoring/slider/track");
    CHECK(preview.authoring_map[1].authoring_node_id
          == "widgets/test/slider/authoring/slider/fill");
    CHECK(preview.authoring_map[2].authoring_node_id
          == "widgets/test/slider/authoring/slider/thumb");
    CHECK(preview.authoring_map[3].authoring_node_id
          == "widgets/test/slider/authoring/focus/highlight");
    REQUIRE_FALSE(preview.pipeline_flags.empty());
    CHECK(preview.pipeline_flags.back() == 0u);
}

TEST_CASE("Widgets::BuildListPreview provides layout geometry") {
    Widgets::ListStyle style{};
    style.width = 120.0f;
    style.item_height = 30.0f;
    style.border_thickness = 4.0f;
    style.item_typography.font_size = 16.0f;
    style.item_typography.line_height = 20.0f;
    style.item_typography.baseline_shift = 3.0f;

    std::vector<Widgets::ListItem> items{
        Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
        Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = false},
        Widgets::ListItem{.id = "gamma", .label = "Gamma", .enabled = true},
    };

    auto state = Widgets::MakeListState()
                     .WithEnabled(true)
                     .WithFocused(true)
                     .WithHoveredIndex(2)
                     .WithSelectedIndex(1)
                     .WithScrollOffset(12.0f)
                     .Build();

    auto preview = Widgets::BuildListPreview(style, items, state);
    CHECK(preview.layout.bounds.max_x == doctest::Approx(120.0f));
    CHECK(preview.layout.bounds.height()
          == doctest::Approx(preview.layout.border_thickness * 2.0f
                             + preview.layout.item_height * 3.0f));
    CHECK(preview.layout.label_inset == doctest::Approx(16.0f));
    CHECK(preview.layout.state.selected_index == 2);
    CHECK(preview.layout.state.hovered_index == 2);

    REQUIRE(preview.layout.rows.size() == 3);
    auto const& row0 = preview.layout.rows[0];
    CHECK(row0.row_bounds.min_x == doctest::Approx(preview.layout.border_thickness));
    CHECK(row0.row_bounds.max_x == doctest::Approx(120.0f - preview.layout.border_thickness));
    CHECK(row0.label_bounds.min_x
          == doctest::Approx(preview.layout.border_thickness + preview.layout.label_inset));
    CHECK(row0.label_bounds.height()
          == doctest::Approx(preview.layout.style.item_typography.line_height));
    CHECK_FALSE(row0.selected);
    CHECK_FALSE(row0.hovered);

    auto const& row1 = preview.layout.rows[1];
    CHECK_FALSE(row1.enabled);
    CHECK_FALSE(row1.selected);

    auto const& row2 = preview.layout.rows[2];
    CHECK(row2.hovered);
    CHECK(row2.selected);
    CHECK(row2.label_baseline
          == doctest::Approx(row2.label_bounds.min_y + preview.layout.style.item_typography.baseline_shift));

    REQUIRE_FALSE(preview.bucket.pipeline_flags.empty());
    CHECK(preview.bucket.pipeline_flags.back() == SP::UI::PipelineFlags::HighlightPulse);

    auto preview_no_pulse = Widgets::BuildListPreview(
        style,
        items,
        state,
        Widgets::ListPreviewOptions{
            .authoring_root = "widgets/test/list",
            .label_inset = 8.0f,
            .pulsing_highlight = false,
        });
    CHECK(preview_no_pulse.layout.label_inset == doctest::Approx(8.0f));
    REQUIRE_FALSE(preview_no_pulse.bucket.authoring_map.empty());
    CHECK(preview_no_pulse.bucket.authoring_map.front().authoring_node_id
          == "widgets/test/list/authoring/list/background");
    REQUIRE_FALSE(preview_no_pulse.bucket.pipeline_flags.empty());
    CHECK(preview_no_pulse.bucket.pipeline_flags.back() == 0u);
}

TEST_CASE("Widgets::BuildStackPreview reports layout metrics and bucket metadata") {
    Widgets::StackLayoutStyle style{};
    style.axis = Widgets::StackAxis::Vertical;
    style.spacing = 12.0f;
    style.padding_main_start = 8.0f;
    style.padding_main_end = 10.0f;
    style.padding_cross_start = 6.0f;
    style.padding_cross_end = 4.0f;
    style.width = 200.0f;

    Widgets::StackLayoutState state{};
    state.width = 180.0f;
    state.height = 100.0f;
    state.children = {
        Widgets::StackLayoutComputedChild{
            .id = "alpha",
            .x = 8.0f,
            .y = 6.0f,
            .width = 90.0f,
            .height = 28.0f,
        },
        Widgets::StackLayoutComputedChild{
            .id = "beta",
            .x = 8.0f,
            .y = 54.0f,
            .width = 140.0f,
            .height = 48.0f,
        },
    };

    auto preview = Widgets::BuildStackPreview(
        style,
        state,
        Widgets::StackPreviewOptions{
            .authoring_root = "widgets/test/stack",
            .background_color = {0.10f, 0.11f, 0.14f, 1.0f},
            .child_start_color = {0.70f, 0.72f, 0.98f, 1.0f},
            .child_end_color = {0.92f, 0.94f, 0.99f, 1.0f},
            .child_opacity = 0.5f,
            .mix_scale = 0.5f,
        });

    CHECK(preview.layout.bounds.max_x == doctest::Approx(200.0f));
    CHECK(preview.layout.bounds.max_y == doctest::Approx(102.0f));
    REQUIRE(preview.layout.child_bounds.size() == 2);
    CHECK(preview.layout.child_bounds[0].min_x == doctest::Approx(8.0f));
    CHECK(preview.layout.child_bounds[1].max_x == doctest::Approx(148.0f));
    CHECK(preview.layout.child_bounds[1].max_y == doctest::Approx(102.0f));
    CHECK(preview.layout.state.width == doctest::Approx(200.0f));
    CHECK(preview.layout.state.height == doctest::Approx(102.0f));

    REQUIRE(preview.bucket.drawable_ids.size() == 3);
    REQUIRE_FALSE(preview.bucket.authoring_map.empty());
    CHECK(preview.bucket.authoring_map.front().authoring_node_id
          == "widgets/test/stack/authoring/stack/background");
    CHECK(preview.bucket.authoring_map.back().authoring_node_id
          == "widgets/test/stack/authoring/stack/child/beta");
}

TEST_CASE("Widgets::CreateTree publishes snapshot and metadata") {
    BuildersFixture fx;

    auto treeParams = Widgets::MakeTreeParams("filesystem")
                          .WithNodes({
                              Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
                              Widgets::TreeNode{.id = "docs", .parent_id = "root", .label = "Docs", .enabled = true, .expandable = false, .loaded = false},
                              Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
                              Widgets::TreeNode{.id = "tests", .parent_id = "src", .label = "Tests", .enabled = true, .expandable = false, .loaded = false},
                          })
                          .Build();

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

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision != 0);
}

TEST_CASE("Widgets::UpdateTreeState toggles expansion and clamps state") {
    BuildersFixture fx;

    auto treeParams = Widgets::MakeTreeParams("project")
                          .WithNodes({
                              Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
                              Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
                              Widgets::TreeNode{.id = "include", .parent_id = "root", .label = "Include", .enabled = false, .expandable = false, .loaded = false},
                          })
                          .Build();

    auto tree = Widgets::CreateTree(fx.space, fx.root_view(), treeParams);
    REQUIRE(tree);

    auto desired = Widgets::MakeTreeState()
                        .WithEnabled(true)
                        .WithHoveredId("include")
                        .WithSelectedId("src")
                        .WithExpandedIds({"root"})
                        .WithLoadingIds({"src"})
                        .WithScrollOffset(100.0f)
                        .Build();

    auto changed = Widgets::UpdateTreeState(fx.space, *tree, desired);
    REQUIRE(changed);
    CHECK(*changed);

    auto updated = read_value<Widgets::TreeState>(fx.space, tree->state.getPath());
    REQUIRE(updated);
    CHECK(updated->selected_id == "src");
    CHECK(updated->hovered_id.empty());
    CHECK(std::find(updated->expanded_ids.begin(), updated->expanded_ids.end(), "root")
          != updated->expanded_ids.end());

    auto collapse = Widgets::MakeTreeState()
                        .WithEnabled(true)
                        .WithSelectedId("src")
                        .WithExpandedIds({})
                        .Build();
    auto collapsed = Widgets::UpdateTreeState(fx.space, *tree, collapse);
    REQUIRE(collapsed);
    CHECK(*collapsed);

    auto collapsedState = read_value<Widgets::TreeState>(fx.space, tree->state.getPath());
    REQUIRE(collapsedState);
    CHECK(collapsedState->expanded_ids.empty());
}

TEST_CASE("Widgets::Bindings::DispatchTree enqueues ops and schedules renders") {
    BuildersFixture fx;

    auto treeParams = Widgets::MakeTreeParams("bindings_tree")
                          .WithNodes({
                              Widgets::TreeNode{.id = "root", .parent_id = "", .label = "Root", .enabled = true, .expandable = true, .loaded = true},
                              Widgets::TreeNode{.id = "src", .parent_id = "root", .label = "Src", .enabled = true, .expandable = true, .loaded = false},
                              Widgets::TreeNode{.id = "docs", .parent_id = "root", .label = "Docs", .enabled = true, .expandable = false, .loaded = false},
                          })
                          .Build();

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

    auto treeStyle = fx.space.read<Widgets::TreeStyle, std::string>(std::string(tree->root.getPath()) + "/meta/style");
    REQUIRE(treeStyle);
    auto treeNodes = fx.space.read<std::vector<Widgets::TreeNode>, std::string>(tree->nodes.getPath());
    REQUIRE(treeNodes);
    std::size_t nodeCount = std::max<std::size_t>(treeNodes->size(), 1u);
    DirtyRectHint treeFootprint{};
    treeFootprint.min_x = 0.0f;
    treeFootprint.min_y = 0.0f;
    treeFootprint.max_x = treeStyle->width;
    treeFootprint.max_y = treeStyle->border_thickness * 2.0f
                         + treeStyle->row_height * static_cast<float>(nodeCount);

    auto binding = WidgetBindings::CreateTreeBinding(fx.space,
                                                     fx.root_view(),
                                                     *tree,
                                                     SP::ConcretePathStringView{target->getPath()},
                                                     treeFootprint,
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
                                               WidgetBindings::PointerInfo::Make(0.0f, 0.0f),
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

    auto buttonParams = Widgets::MakeButtonParams("stack_button", "Stack Button").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("stack_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto sliderParams = Widgets::MakeSliderParams("stack_slider")
                             .WithRange(0.0f, 1.0f)
                             .WithValue(0.5f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto stackParams = Widgets::MakeStackLayoutParams("column")
                            .ModifyStyle([](Widgets::StackLayoutStyle& style) {
                                style.axis = Widgets::StackAxis::Vertical;
                                style.spacing = 24.0f;
                                style.padding_main_start = 16.0f;
                                style.padding_cross_start = 20.0f;
                            })
                            .WithChildren({
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
                            })
                            .Build();

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

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, stack->scene);
    REQUIRE(revision);
    auto base = std::string(stack->scene.getPath()) + "/builds/" + format_revision(revision->revision);
    auto bucket = SceneSnapshotBuilder::decode_bucket(fx.space, base);
    REQUIRE(bucket);
    CHECK(bucket->drawable_ids.size() >= 3);
}

TEST_CASE("Widgets::UpdateListState clamps indices and marks dirty") {
    BuildersFixture fx;

    auto listParams = Widgets::MakeListParams("inventory_updates")
                          .WithItems({
                              Widgets::ListItem{.id = "sword", .label = "Sword", .enabled = false},
                              Widgets::ListItem{.id = "shield", .label = "Shield", .enabled = true},
                              Widgets::ListItem{.id = "bow", .label = "Bow", .enabled = true},
                          })
                          .ModifyStyle([](Widgets::ListStyle& style) {
                              style.item_height = 32.0f;
                          })
                          .Build();

    auto created = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(created);

    auto revision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);

    auto desired = Widgets::MakeListState()
                        .WithEnabled(true)
                        .WithSelectedIndex(0)
                        .WithHoveredIndex(5)
                        .WithScrollOffset(120.0f)
                        .Build();

    auto changed = Widgets::UpdateListState(fx.space, *created, desired);
    REQUIRE(changed);
    CHECK(*changed);

    auto updated = read_value<Widgets::ListState>(fx.space, created->state.getPath());
    REQUIRE(updated);
    CHECK(updated->selected_index == 1);
    CHECK(updated->hovered_index == 2);
    CHECK(updated->scroll_offset == doctest::Approx(64.0f)); // two rows * 32 - 32

    auto updatedRevision = BuilderScene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(updatedRevision);
    CHECK(updatedRevision->revision > revision->revision);

    auto unchanged = Widgets::UpdateListState(fx.space, *created, *updated);
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
}

TEST_CASE("Widgets::ResolveHitTarget extracts canonical widget path from hit test") {
    BuildersFixture fx;

    auto params = Widgets::MakeButtonParams("resolve_hit_button", "Resolve").Build();

    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    BuilderScene::HitTestRequest request{};
    request.x = 12.0f;
    request.y = 16.0f;

    auto hit = BuilderScene::HitTest(fx.space, button->scene, request);
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

    auto params = Widgets::MakeButtonParams("golden_button", "Golden").Build();
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

    auto params = Widgets::MakeToggleParams("golden_toggle").Build();
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

    auto params = Widgets::MakeSliderParams("golden_slider")
                      .WithRange(0.0f, 1.0f)
                      .WithValue(0.35f)
                      .Build();
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

    auto params = Widgets::MakeListParams("golden_list")
                      .WithItems({
                          Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
                          Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
                          Widgets::ListItem{.id = "gamma", .label = "Gamma", .enabled = false},
                      })
                      .ModifyStyle([](Widgets::ListStyle& style) {
                          style.width = 260.0f;
                          style.item_height = 38.0f;
                      })
                      .Build();
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

    auto buttonParams = Widgets::MakeButtonParams("focus_01_button", "Focus").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("focus_02_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto sliderParams = Widgets::MakeSliderParams("focus_03_slider")
                             .WithRange(0.0f, 1.0f)
                             .WithValue(0.25f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto listParams = Widgets::MakeListParams("focus_04_list")
                          .WithItems({
                              Widgets::ListItem{.id = "alpha", .label = "Alpha"},
                              Widgets::ListItem{.id = "beta", .label = "Beta"},
                              Widgets::ListItem{.id = "gamma", .label = "Gamma"},
                          })
                          .Build();
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
    auto buttonFocusFlag = fx.space.read<bool, std::string>(button->root.getPath() + "/focus/current");
    REQUIRE(buttonFocusFlag);
    CHECK(*buttonFocusFlag);

    auto toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK_FALSE(toggleState->hovered);
    CHECK_FALSE(toggleState->focused);

    auto moveToggle = WidgetFocus::Move(fx.space,
                                        config,
                                        WidgetFocus::Direction::Forward);
    REQUIRE(moveToggle);
    REQUIRE(moveToggle->has_value());
    CHECK(moveToggle->value().widget.getPath() == toggle->root.getPath());

    toggleState = fx.space.read<Widgets::ToggleState, std::string>(toggle->state.getPath());
    REQUIRE(toggleState);
    CHECK(toggleState->hovered);
    CHECK(toggleState->focused);
    auto toggleFocusFlag = fx.space.read<bool, std::string>(toggle->root.getPath() + "/focus/current");
    REQUIRE(toggleFocusFlag);
    CHECK(*toggleFocusFlag);

    buttonState = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(buttonState);
    CHECK_FALSE(buttonState->hovered);
    CHECK_FALSE(buttonState->focused);
    buttonFocusFlag = fx.space.read<bool, std::string>(button->root.getPath() + "/focus/current");
    REQUIRE(buttonFocusFlag);
    CHECK_FALSE(*buttonFocusFlag);

    // Advance to slider, then list
    (void)WidgetFocus::Move(fx.space,
                            config,
                            WidgetFocus::Direction::Forward);
    auto moveList = WidgetFocus::Move(fx.space,
                                      config,
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
    auto listFocusFlag = fx.space.read<bool, std::string>(list->root.getPath() + "/focus/current");
    REQUIRE(listFocusFlag);
    CHECK(*listFocusFlag);

    auto cleared = WidgetFocus::Clear(fx.space, config);
    REQUIRE(cleared);
    CHECK(*cleared);

    listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->hovered_index == -1);
    CHECK_FALSE(listState->focused);
    listFocusFlag = fx.space.read<bool, std::string>(list->root.getPath() + "/focus/current");
    REQUIRE(listFocusFlag);
    CHECK_FALSE(*listFocusFlag);
}

TEST_CASE("Widgets::Focus::ApplyHit focuses widget from hit test") {
    BuildersFixture fx;

    auto params = Widgets::MakeButtonParams("focus_hit_button", "FocusHit").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), params);
    REQUIRE(button);

    auto config = WidgetFocus::MakeConfig(fx.root_view());

    BuilderScene::HitTestRequest request{};
    request.x = 8.0f;
    request.y = 8.0f;

    auto hit = BuilderScene::HitTest(fx.space, button->scene, request);
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

    auto params = Widgets::MakeButtonParams("focus_auto_button", "Auto").Build();
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

    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    DirtyRectHint buttonFootprint{};
    buttonFootprint.min_x = 0.0f;
    buttonFootprint.min_y = 0.0f;
    buttonFootprint.max_x = buttonStyle->width;
    buttonFootprint.max_y = buttonStyle->height;

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{targetAbs->getPath()},
                                                       buttonFootprint);
    REQUIRE(binding);

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

TEST_CASE("Widget focus shift marks previous footprint dirty") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("focus_dirty_button", "DirtyButton").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("focus_dirty_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    RendererParams rendererParams{ .name = "focus_dirty_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {256, 192};

    SurfaceParams surfaceParams{ .name = "focus_dirty_surface", .desc = desc, .renderer = "renderers/focus_dirty_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, button->scene));

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);

    auto buttonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(buttonStyle);
    constexpr float kFocusPadding = 6.0f;
    DirtyRectHint buttonFootprint{-kFocusPadding,
                                  -kFocusPadding,
                                  buttonStyle->width + kFocusPadding,
                                  buttonStyle->height + kFocusPadding};
    auto buttonBinding = WidgetBindings::CreateButtonBinding(fx.space,
                                                             fx.root_view(),
                                                             *button,
                                                             SP::ConcretePathStringView{targetAbs->getPath()},
                                                             buttonFootprint);
    REQUIRE(buttonBinding);

    auto toggleStyle = fx.space.read<Widgets::ToggleStyle, std::string>(std::string(toggle->root.getPath()) + "/meta/style");
    REQUIRE(toggleStyle);
    DirtyRectHint toggleFootprint{200.0f, 0.0f, 200.0f + toggleStyle->width, toggleStyle->height};
    auto toggleBinding = WidgetBindings::CreateToggleBinding(fx.space,
                                                             fx.root_view(),
                                                             *toggle,
                                                             SP::ConcretePathStringView{targetAbs->getPath()},
                                                             toggleFootprint);
    REQUIRE(toggleBinding);

    auto config = WidgetFocus::MakeConfig(fx.root_view(),
                                          SP::UI::Builders::ConcretePath{targetAbs->getPath()});

    auto setButton = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(setButton);
    CHECK(setButton->changed);

    auto hintsPath = targetAbs->getPath() + "/hints/dirtyRects";
    (void)fx.space.read<std::vector<DirtyRectHint>, std::string>(hintsPath);

    auto moveToggle = WidgetFocus::Move(fx.space,
                                        config,
                                        WidgetFocus::Direction::Forward);
    REQUIRE(moveToggle);
    REQUIRE(moveToggle->has_value());
    CHECK(moveToggle->value().widget.getPath() == toggle->root.getPath());

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(hintsPath);
    REQUIRE(hints);
    REQUIRE_FALSE(hints->empty());

    auto button_center_x = (buttonFootprint.min_x + buttonFootprint.max_x) * 0.5f;
    auto button_center_y = (buttonFootprint.min_y + buttonFootprint.max_y) * 0.5f;
    auto toggle_center_x = (toggleFootprint.min_x + toggleFootprint.max_x) * 0.5f;
    auto toggle_center_y = (toggleFootprint.min_y + toggleFootprint.max_y) * 0.5f;

    auto covers_point = [](DirtyRectHint const& hint, float x, float y) {
        return x >= hint.min_x && x <= hint.max_x && y >= hint.min_y && y <= hint.max_y;
    };

    bool button_covered = std::any_of(hints->begin(), hints->end(), [&](DirtyRectHint const& hint) {
        return covers_point(hint, button_center_x, button_center_y);
    });
    bool toggle_covered = std::any_of(hints->begin(), hints->end(), [&](DirtyRectHint const& hint) {
        return covers_point(hint, toggle_center_x, toggle_center_y);
    });

    CHECK(button_covered);
    CHECK(toggle_covered);

    float focus_padding = WidgetInput::FocusHighlightPadding();
    float surface_width = static_cast<float>(desc.size_px.width);
    float surface_height = static_cast<float>(desc.size_px.height);
    auto expanded_rect = [&](DirtyRectHint const& base) {
        DirtyRectHint expanded{};
        expanded.min_x = std::max(0.0f, base.min_x - focus_padding);
        expanded.min_y = std::max(0.0f, base.min_y - focus_padding);
        expanded.max_x = std::min(surface_width, base.max_x + focus_padding);
        expanded.max_y = std::min(surface_height, base.max_y + focus_padding);
        return expanded;
    };
    DirtyRectHint expected_button = expanded_rect(buttonFootprint);
    DirtyRectHint expected_toggle = expanded_rect(toggleFootprint);

    std::ostringstream hints_stream;
    hints_stream << "[";
    for (std::size_t i = 0; i < hints->size(); ++i) {
        auto const& hint = (*hints)[i];
        hints_stream << "[" << hint.min_x << ", " << hint.min_y << ", " << hint.max_x << ", " << hint.max_y << "]";
        if (i + 1 < hints->size()) {
            hints_stream << ", ";
        }
    }
    hints_stream << "]";
    auto hints_str = hints_stream.str();

    auto covers_any = [&](float x, float y) {
        return std::any_of(hints->begin(), hints->end(), [&](DirtyRectHint const& hint) {
            return covers_point(hint, x, y);
        });
    };

    auto highlight_edges_covered = [&](DirtyRectHint const& expected) {
        constexpr float kEdgeEpsilon = 0.25f;
        float x_center = (expected.min_x + expected.max_x) * 0.5f;
        float y_center = (expected.min_y + expected.max_y) * 0.5f;
        auto sample_x = [&](float edge) {
            if (expected.max_x - expected.min_x <= 2.0f * kEdgeEpsilon) {
                return x_center;
            }
            return std::clamp(edge, expected.min_x + kEdgeEpsilon, expected.max_x - kEdgeEpsilon);
        };
        auto sample_y = [&](float edge) {
            if (expected.max_y - expected.min_y <= 2.0f * kEdgeEpsilon) {
                return y_center;
            }
            return std::clamp(edge, expected.min_y + kEdgeEpsilon, expected.max_y - kEdgeEpsilon);
        };
        float x_left = sample_x(expected.min_x + kEdgeEpsilon);
        float x_right = sample_x(expected.max_x - kEdgeEpsilon);
        float y_top = sample_y(expected.min_y + kEdgeEpsilon);
        float y_bottom = sample_y(expected.max_y - kEdgeEpsilon);

        bool horizontal = covers_any(x_left, y_center) && covers_any(x_right, y_center);
        bool vertical = covers_any(x_center, y_top) && covers_any(x_center, y_bottom);
        return horizontal && vertical;
    };

    CHECK_MESSAGE(highlight_edges_covered(expected_button),
                  "dirty hints " << hints_str << " expected button highlight coverage ["
                                  << expected_button.min_x << ", " << expected_button.min_y << ", "
                                  << expected_button.max_x << ", " << expected_button.max_y << "]");
    CHECK_MESSAGE(highlight_edges_covered(expected_toggle),
                  "dirty hints " << hints_str << " expected toggle highlight coverage ["
                                  << expected_toggle.min_x << ", " << expected_toggle.min_y << ", "
                                  << expected_toggle.max_x << ", " << expected_toggle.max_y << "]");
}

TEST_CASE("Widget focus slider-to-list transition covers highlight footprint") {
    BuildersFixture fx;

    auto sliderParams = Widgets::MakeSliderParams("focus_slider_widget")
                            .WithRange(0.0f, 1.0f)
                            .WithValue(0.4f)
                            .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto listParams = Widgets::MakeListParams("focus_list_widget")
                          .WithItems({
                              Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
                              Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
                              Widgets::ListItem{.id = "gamma", .label = "Gamma", .enabled = true},
                          })
                          .Build();
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    RendererParams rendererParams{
        .name = "focus_slider_list_renderer",
        .kind = RendererKind::Software2D,
        .description = "Renderer"
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 256};
    SurfaceParams surfaceParams{
        .name = "focus_slider_list_surface",
        .desc = desc,
        .renderer = "renderers/focus_slider_list_renderer"
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    REQUIRE(Surface::SetScene(fx.space, *surface, slider->scene));

    WindowParams windowParams{
        .name = "focus_slider_list_window",
        .title = "Slider List Focus Window",
        .width = desc.size_px.width,
        .height = desc.size_px.height,
    };
    auto window = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);
    REQUIRE(BuilderWindow::AttachSurface(fx.space, *window, "main", *surface));
    enable_framebuffer_capture(fx.space, *window, "main");

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);
    auto targetConcrete = SP::UI::Builders::ConcretePath{targetAbs->getPath()};
    auto targetView = SP::UI::Builders::ConcretePathView{targetConcrete.getPath()};

    auto capture_frame = [&](std::string const& step_label, RenderSettings const& settings) -> SoftwareFramebuffer {
        auto render = Surface::RenderOnce(fx.space, *surface, settings);
        if (!render) {
            INFO(step_label << ": Surface::RenderOnce code=" << static_cast<int>(render.error().code));
            INFO(step_label << ": Surface::RenderOnce message="
                            << render.error().message.value_or("<none>"));
        }
        REQUIRE(render);
        auto present = BuilderWindow::Present(fx.space, *window, "main");
        if (!present) {
            INFO(step_label << ": BuilderWindow::Present code=" << static_cast<int>(present.error().code));
            INFO(step_label << ": BuilderWindow::Present message="
                            << present.error().message.value_or("<none>"));
        }
        REQUIRE(present);
        auto framebuffer = SP::UI::Builders::Diagnostics::ReadSoftwareFramebuffer(fx.space, targetView);
        if (!framebuffer) {
            INFO(step_label << ": ReadSoftwareFramebuffer code="
                            << static_cast<int>(framebuffer.error().code));
            INFO(step_label << ": ReadSoftwareFramebuffer message="
                            << framebuffer.error().message.value_or("<none>"));
        }
        REQUIRE(framebuffer);
        return *framebuffer;
    };

    auto sliderStyle = fx.space.read<Widgets::SliderStyle, std::string>(std::string(slider->root.getPath()) + "/meta/style");
    REQUIRE(sliderStyle);
    DirtyRectHint sliderFootprint{
        0.0f,
        0.0f,
        sliderStyle->width,
        sliderStyle->height
    };
    auto sliderBinding = WidgetBindings::CreateSliderBinding(fx.space,
                                                             fx.root_view(),
                                                             *slider,
                                                             SP::ConcretePathStringView{targetAbs->getPath()},
                                                             sliderFootprint);
    REQUIRE(sliderBinding);

    auto listStyle = fx.space.read<Widgets::ListStyle, std::string>(std::string(list->root.getPath()) + "/meta/style");
    REQUIRE(listStyle);
    auto listItems = fx.space.read<std::vector<Widgets::ListItem>, std::string>(list->items.getPath());
    REQUIRE(listItems);
    auto listCount = static_cast<float>(std::max<std::size_t>(listItems->size(), 1));
    float listHeight = listStyle->border_thickness * 2.0f + listStyle->item_height * listCount;
    float listOffsetY = sliderStyle->height + 48.0f;
    DirtyRectHint listFootprint{
        0.0f,
        listOffsetY,
        listStyle->width,
        listOffsetY + listHeight
    };
    auto listBinding = WidgetBindings::CreateListBinding(fx.space,
                                                         fx.root_view(),
                                                         *list,
                                                         SP::ConcretePathStringView{targetAbs->getPath()},
                                                         listFootprint);
    REQUIRE(listBinding);

    float focus_padding = Widgets::Input::FocusHighlightPadding();
    auto expand_for_focus = [&](DirtyRectHint const& base) -> DirtyRectHint {
        DirtyRectHint expanded{
            std::max(0.0f, base.min_x - focus_padding),
            std::max(0.0f, base.min_y - focus_padding),
            base.max_x + focus_padding,
            base.max_y + focus_padding,
        };
        expanded.max_x = std::min(expanded.max_x, static_cast<float>(desc.size_px.width));
        expanded.max_y = std::min(expanded.max_y, static_cast<float>(desc.size_px.height));
        return expanded;
    };

    DirtyRectHint sliderHighlightRegion = expand_for_focus(sliderFootprint);
    DirtyRectHint listHighlightRegion = expand_for_focus(listFootprint);

    RenderSettings base_settings{};
    base_settings.surface.size_px.width = desc.size_px.width;
    base_settings.surface.size_px.height = desc.size_px.height;
    base_settings.surface.visibility = true;
    base_settings.clear_color = {0.05f, 0.05f, 0.05f, 1.0f};
    base_settings.time.time_ms = 1000.0;
    base_settings.time.delta_ms = 16.0;

    uint64_t frame_index = 1;
    auto next_settings = [&]() {
        RenderSettings settings = base_settings;
        settings.time.frame_index = frame_index++;
        return settings;
    };

    auto renderQueuePath = targetAbs->getPath() + "/events/renderRequested/queue";

    auto drain_auto_render = [&](std::vector<SoftwareFramebuffer>& frames) {
        frames.clear();
        while (true) {
            auto event = fx.space.take<AutoRenderRequestEvent, std::string>(renderQueuePath);
            if (!event) {
                CHECK((event.error().code == Error::Code::NoObjectFound
                       || event.error().code == Error::Code::NoSuchPath));
                break;
            }
            frames.push_back(capture_frame("auto-render", next_settings()));
        }
    };

    auto baseline_fb = capture_frame("baseline", next_settings());

    auto pointer = WidgetBindings::PointerInfo::Make(sliderStyle->width * 0.75f,
                                                     sliderStyle->height * 0.5f)
                       .WithInside(true)
                       .WithPrimary(true);

    auto beginState = Widgets::MakeSliderState()
                          .WithEnabled(true)
                          .WithHovered(true)
                          .WithDragging(true)
                          .WithFocused(true)
                          .WithValue(0.45f)
                          .Build();
    auto beginResult = WidgetBindings::DispatchSlider(fx.space,
                                                      *sliderBinding,
                                                      beginState,
                                                      WidgetBindings::WidgetOpKind::SliderBegin,
                                                      pointer);
    REQUIRE(beginResult);
    CHECK(*beginResult);
    std::vector<SoftwareFramebuffer> slider_frames;
    drain_auto_render(slider_frames);

    auto updateState = Widgets::MakeSliderState()
                           .WithEnabled(true)
                           .WithHovered(true)
                           .WithDragging(true)
                           .WithFocused(true)
                           .WithValue(0.65f)
                           .Build();
    auto updateResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *sliderBinding,
                                                       updateState,
                                                       WidgetBindings::WidgetOpKind::SliderUpdate,
                                                       pointer);
    REQUIRE(updateResult);
    CHECK(*updateResult);
    drain_auto_render(slider_frames);

    auto commitState = Widgets::MakeSliderState()
                           .WithEnabled(true)
                           .WithHovered(true)
                           .WithFocused(true)
                           .WithValue(0.65f)
                           .Build();
    auto commitResult = WidgetBindings::DispatchSlider(fx.space,
                                                       *sliderBinding,
                                                       commitState,
                                                       WidgetBindings::WidgetOpKind::SliderCommit,
                                                       pointer);
    REQUIRE(commitResult);
    CHECK(*commitResult);
    SoftwareFramebuffer slider_fb{};
    drain_auto_render(slider_frames);
    if (slider_frames.empty()) {
        slider_fb = capture_frame("focus-slider", next_settings());
    } else {
        slider_fb = slider_frames.back();
    }

    auto storedFootprint = fx.space.read<DirtyRectHint, std::string>(std::string(slider->root.getPath()) + "/meta/footprint");
    REQUIRE(storedFootprint);
    INFO("stored slider footprint [" << storedFootprint->min_x << ", "
                                     << storedFootprint->min_y << ", "
                                     << storedFootprint->max_x << ", "
                                     << storedFootprint->max_y << "]");

    auto sliderStateBeforeMove = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderStateBeforeMove);
    CHECK(sliderStateBeforeMove->focused);
    CHECK(sliderStateBeforeMove->hovered);

    auto focusBeforeMove = WidgetFocus::Current(fx.space, ConcretePathView{sliderBinding->options.focus_state.getPath()});
    REQUIRE(focusBeforeMove);
    REQUIRE(focusBeforeMove->has_value());
    CHECK(*focusBeforeMove == slider->root.getPath());

    float listPointerX = (listFootprint.min_x + listFootprint.max_x) * 0.5f;
    float listPointerY = listFootprint.min_y + listStyle->border_thickness + listStyle->item_height * 0.5f;
    auto listPointer = WidgetBindings::PointerInfo::Make(listPointerX, listPointerY)
                           .WithInside(true)
                           .WithPrimary(true);

    auto listStateBefore = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listStateBefore);

    auto listHoverState = *listStateBefore;
    listHoverState.hovered_index = 0;
    auto hoverResult = WidgetBindings::DispatchList(fx.space,
                                                    *listBinding,
                                                    listHoverState,
                                                    WidgetBindings::WidgetOpKind::ListHover,
                                                    listPointer,
                                                    0,
                                                    0.0f);
    REQUIRE(hoverResult);
    CHECK(*hoverResult);

    auto listSelectState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listSelectState);
    listSelectState->selected_index = 0;
    auto selectResult = WidgetBindings::DispatchList(fx.space,
                                                     *listBinding,
                                                     *listSelectState,
                                                     WidgetBindings::WidgetOpKind::ListSelect,
                                                     listPointer,
                                                     0,
                                                     0.0f);
    REQUIRE(selectResult);
    CHECK(*selectResult);

    auto listActivateState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listActivateState);
    listActivateState->selected_index = 0;
    auto activateResult = WidgetBindings::DispatchList(fx.space,
                                                       *listBinding,
                                                       *listActivateState,
                                                       WidgetBindings::WidgetOpKind::ListActivate,
                                                       listPointer,
                                                       0,
                                                       0.0f);
    REQUIRE(activateResult);

    auto focusAfterPointer = WidgetFocus::Current(fx.space,
                                                  ConcretePathView{sliderBinding->options.focus_state.getPath()});
    REQUIRE(focusAfterPointer);
    REQUIRE(focusAfterPointer->has_value());
    CHECK(*focusAfterPointer == list->root.getPath());

    auto hintsPath = targetAbs->getPath() + "/hints/dirtyRects";
    auto hintsBefore = fx.space.read<std::vector<DirtyRectHint>, std::string>(hintsPath);
    REQUIRE_MESSAGE(hintsBefore,
                    "dirty hints missing before focus handoff: code="
                        << static_cast<int>(hintsBefore.error().code)
                        << " message=" << hintsBefore.error().message.value_or("<none>"));
    auto format_hint = [](DirtyRectHint const& hint) {
        std::ostringstream oss;
        oss << "[" << hint.min_x << ", " << hint.min_y << ", "
            << hint.max_x << ", " << hint.max_y << "]";
        return oss.str();
    };
    std::ostringstream hints_summary;
    hints_summary << "[";
    for (std::size_t i = 0; i < hintsBefore->size(); ++i) {
        hints_summary << format_hint((*hintsBefore)[i]);
        if (i + 1 < hintsBefore->size()) {
            hints_summary << ", ";
        }
    }
    hints_summary << "]";
    auto hints_summary_str = hints_summary.str();
    INFO("dirty hints summary " << hints_summary_str);

    constexpr float kHintTolerance = 0.75f;
    auto covers_region = [&](DirtyRectHint const& hint, DirtyRectHint const& expected) -> bool {
        return hint.min_x <= expected.min_x + kHintTolerance
            && hint.min_y <= expected.min_y + kHintTolerance
            && hint.max_x + kHintTolerance >= expected.max_x
            && hint.max_y + kHintTolerance >= expected.max_y;
    };

    bool slider_hint_found = std::any_of(hintsBefore->begin(),
                                         hintsBefore->end(),
                                         [&](DirtyRectHint const& hint) {
                                             return covers_region(hint, sliderHighlightRegion);
                                         });
    bool list_hint_found = std::any_of(hintsBefore->begin(),
                                       hintsBefore->end(),
                                       [&](DirtyRectHint const& hint) {
                                           return covers_region(hint, listHighlightRegion);
                                       });

    CHECK_MESSAGE(slider_hint_found,
                  "dirty hints " << hints_summary_str
                                 << " missing slider highlight coverage ["
                                 << sliderHighlightRegion.min_x << ", "
                                 << sliderHighlightRegion.min_y << ", "
                                 << sliderHighlightRegion.max_x << ", "
                                 << sliderHighlightRegion.max_y << "]");
    CHECK_MESSAGE(list_hint_found,
                  "dirty hints " << hints_summary_str
                                 << " missing list highlight coverage ["
                                 << listHighlightRegion.min_x << ", "
                                 << listHighlightRegion.min_y << ", "
                                 << listHighlightRegion.max_x << ", "
                                 << listHighlightRegion.max_y << "]");

    for (auto const& hint : *hintsBefore) {
        INFO("dirty hint [" << hint.min_x << ", " << hint.min_y << ", "
             << hint.max_x << ", " << hint.max_y << "]");
    }

    std::vector<SoftwareFramebuffer> list_frames;
    drain_auto_render(list_frames);
    SoftwareFramebuffer first_list_frame{};
    SoftwareFramebuffer list_fb{};
    if (list_frames.empty()) {
        list_fb = capture_frame("focus-list", next_settings());
        first_list_frame = list_fb;
    } else {
        first_list_frame = list_frames.front();
        list_fb = list_frames.back();
    }

    auto sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK_FALSE(sliderState->focused);
    CHECK_FALSE(sliderState->hovered);
    CHECK_FALSE(sliderState->dragging);

    auto listState = fx.space.read<Widgets::ListState, std::string>(list->state.getPath());
    REQUIRE(listState);
    CHECK(listState->focused);

    auto sliderRevision = BuilderScene::ReadCurrentRevision(fx.space, slider->scene);
    REQUIRE(sliderRevision);
    auto sliderRevisionPath = std::string(slider->scene.getPath())
        + "/builds/"
        + format_revision(sliderRevision->revision);
    auto sliderBucket = SceneSnapshotBuilder::decode_bucket(fx.space, sliderRevisionPath);
    REQUIRE(sliderBucket);
    bool sliderHighlightPresent = std::any_of(sliderBucket->authoring_map.begin(),
                                              sliderBucket->authoring_map.end(),
                                              [](UIScene::DrawableAuthoringMapEntry const& entry) {
                                                  return entry.authoring_node_id.find("focus/highlight")
                                                      != std::string::npos;
                                              });
    CHECK_FALSE(sliderHighlightPresent);

    auto sample_pixel = [](SoftwareFramebuffer const& fb, int x, int y) -> std::array<std::uint8_t, 4> {
        REQUIRE(x >= 0);
        REQUIRE(y >= 0);
        REQUIRE(x < fb.width);
        REQUIRE(y < fb.height);
        auto stride = static_cast<std::size_t>(fb.row_stride_bytes);
        auto offset = stride * static_cast<std::size_t>(y) + static_cast<std::size_t>(x) * 4u;
        REQUIRE(offset + 3 < fb.pixels.size());
        return {
            fb.pixels[offset + 0],
            fb.pixels[offset + 1],
            fb.pixels[offset + 2],
            fb.pixels[offset + 3],
        };
    };

    auto clamp_index = [](float coord, int extent) -> int {
        int value = static_cast<int>(std::lround(coord));
        if (value < 0) {
            return 0;
        }
        if (value >= extent) {
            return extent - 1;
        }
        return value;
    };

    auto compute_region_diff = [&](SoftwareFramebuffer const& before,
                                   SoftwareFramebuffer const& after,
                                   DirtyRectHint const& region) -> std::uint64_t {
        int min_x = clamp_index(region.min_x, before.width);
        int min_y = clamp_index(region.min_y, before.height);
        int max_x = clamp_index(region.max_x - 1.0f, before.width);
        int max_y = clamp_index(region.max_y - 1.0f, before.height);
        std::uint64_t total = 0;
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                auto b = sample_pixel(before, x, y);
                auto a = sample_pixel(after, x, y);
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[0]) - static_cast<int>(a[0])));
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[1]) - static_cast<int>(a[1])));
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[2]) - static_cast<int>(a[2])));
            }
        }
        return total;
    };

    auto compute_ring_diff = [&](SoftwareFramebuffer const& before,
                                 SoftwareFramebuffer const& after,
                                 DirtyRectHint const& outer,
                                 DirtyRectHint const& inner) -> std::uint64_t {
        int outer_min_x = clamp_index(outer.min_x, before.width);
        int outer_min_y = clamp_index(outer.min_y, before.height);
        int outer_max_x = clamp_index(outer.max_x - 1.0f, before.width);
        int outer_max_y = clamp_index(outer.max_y - 1.0f, before.height);
        int inner_min_x = clamp_index(inner.min_x, before.width);
        int inner_min_y = clamp_index(inner.min_y, before.height);
        int inner_max_x = clamp_index(inner.max_x - 1.0f, before.width);
        int inner_max_y = clamp_index(inner.max_y - 1.0f, before.height);

        auto inside_inner = [&](int x, int y) -> bool {
            return x >= inner_min_x && x <= inner_max_x
                && y >= inner_min_y && y <= inner_max_y;
        };
        std::uint64_t total = 0;
        for (int y = outer_min_y; y <= outer_max_y; ++y) {
            for (int x = outer_min_x; x <= outer_max_x; ++x) {
                if (inside_inner(x, y)) {
                    continue;
                }
                auto b = sample_pixel(before, x, y);
                auto a = sample_pixel(after, x, y);
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[0]) - static_cast<int>(a[0])));
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[1]) - static_cast<int>(a[1])));
                total += static_cast<std::uint64_t>(std::abs(static_cast<int>(b[2]) - static_cast<int>(a[2])));
            }
        }
        return total;
    };

    auto slider_on_diff = compute_region_diff(baseline_fb, slider_fb, sliderHighlightRegion);
    CHECK(slider_on_diff > 0);

    auto first_frame_ring_diff = compute_ring_diff(baseline_fb, first_list_frame, sliderHighlightRegion, sliderFootprint);
    CHECK_MESSAGE(first_frame_ring_diff == 0,
                  "first focus transition frame should match baseline in highlight ring (diff="
                      << first_frame_ring_diff << ")");

    auto slider_to_first_ring_diff = compute_ring_diff(slider_fb, first_list_frame, sliderHighlightRegion, sliderFootprint);
    CHECK_MESSAGE(slider_to_first_ring_diff > 0,
                  "highlight ring should change between slider-focused and first blur frame (diff="
                      << slider_to_first_ring_diff << ")");

    auto slider_ring_off_diff = compute_ring_diff(baseline_fb, list_fb, sliderHighlightRegion, sliderFootprint);
    CHECK_MESSAGE(slider_ring_off_diff == 0,
                  "slider highlight ring should match baseline after focus move (diff="
                      << slider_ring_off_diff << ")");

    auto slider_ring_diff = compute_ring_diff(slider_fb, list_fb, sliderHighlightRegion, sliderFootprint);
    CHECK_MESSAGE(slider_ring_diff > 0,
                  "highlight ring diff should be non-zero when focus leaves slider (diff="
                      << slider_ring_diff << ")");
}

TEST_CASE("Widget focus slider-to-list transition marks previous footprint without slider binding") {
    BuildersFixture fx;

    auto sliderParams = Widgets::MakeSliderParams("focus_slider_widget_unbound")
                            .WithRange(0.0f, 1.0f)
                            .WithValue(0.35f)
                            .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto listParams = Widgets::MakeListParams("focus_list_widget_unbound")
                          .WithItems({
                              Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
                              Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
                          })
                          .Build();
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    RendererParams rendererParams{
        .name = "focus_slider_list_renderer_unbound",
        .kind = RendererKind::Software2D,
        .description = "Renderer"
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 240};
    SurfaceParams surfaceParams{
        .name = "focus_slider_list_surface_unbound",
        .desc = desc,
        .renderer = "renderers/focus_slider_list_renderer_unbound"
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);
    REQUIRE(Surface::SetScene(fx.space, *surface, slider->scene));

    WindowParams windowParams{
        .name = "focus_slider_list_window_unbound",
        .title = "Slider List Focus Window",
        .width = desc.size_px.width,
        .height = desc.size_px.height,
    };
    auto window = BuilderWindow::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(window);
    REQUIRE(BuilderWindow::AttachSurface(fx.space, *window, "main", *surface));

    auto targetRel = fx.space.read<std::string, std::string>(std::string(surface->getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);
    auto targetPath = targetAbs->getPath();

    auto sliderStyle = fx.space.read<Widgets::SliderStyle, std::string>(std::string(slider->root.getPath()) + "/meta/style");
    REQUIRE(sliderStyle);

    auto listStyle = fx.space.read<Widgets::ListStyle, std::string>(std::string(list->root.getPath()) + "/meta/style");
    REQUIRE(listStyle);
    auto listItems = fx.space.read<std::vector<Widgets::ListItem>, std::string>(list->items.getPath());
    REQUIRE(listItems);

    float listHeight = listStyle->border_thickness * 2.0f
        + listStyle->item_height * static_cast<float>(std::max<std::size_t>(listItems->size(), 1));
    float listOffsetY = sliderStyle->height + 48.0f;
    DirtyRectHint listFootprint{
        0.0f,
        listOffsetY,
        listStyle->width,
        listOffsetY + listHeight
    };
    auto listBinding = WidgetBindings::CreateListBinding(fx.space,
                                                         fx.root_view(),
                                                         *list,
                                                         SP::ConcretePathStringView{targetAbs->getPath()},
                                                         listFootprint);
    REQUIRE(listBinding);

    auto hintsPath = targetPath + "/hints/dirtyRects";
    auto clearHints = fx.space.take<std::vector<DirtyRectHint>>(hintsPath);
    if (!clearHints) {
        CHECK((clearHints.error().code == Error::Code::NoObjectFound
               || clearHints.error().code == Error::Code::NoSuchPath));
    }

    auto config = Widgets::Focus::MakeConfig(
        fx.root_view(),
        std::optional<SP::UI::Builders::ConcretePath>{SP::UI::Builders::ConcretePath{targetPath}});

    auto sliderFocus = Widgets::Focus::Set(fx.space, config, slider->root);
    REQUIRE(sliderFocus);
    CHECK(sliderFocus->changed);

    auto listFocus = Widgets::Focus::Set(fx.space, config, list->root);
    REQUIRE(listFocus);
    CHECK(listFocus->changed);

    auto hints = fx.space.read<std::vector<DirtyRectHint>, std::string>(hintsPath);
    REQUIRE_MESSAGE(hints,
                    "expected dirty hints at "
                        << hintsPath << " code=" << static_cast<int>(hints.error().code)
                        << " message=" << hints.error().message.value_or("<none>"));
    REQUIRE_FALSE(hints->empty());

    float padding = Widgets::Input::FocusHighlightPadding();
    DirtyRectHint expectedSlider{
        std::max(0.0f, -padding),
        std::max(0.0f, -padding),
        sliderStyle->width + padding,
        sliderStyle->height + padding
    };
    expectedSlider.max_x = std::min(expectedSlider.max_x, static_cast<float>(desc.size_px.width));
    expectedSlider.max_y = std::min(expectedSlider.max_y, static_cast<float>(desc.size_px.height));

    auto covers_expected = [&](DirtyRectHint const& hint) {
        constexpr float kEpsilon = 1e-3f;
        return hint.min_x <= expectedSlider.min_x + kEpsilon
            && hint.min_y <= expectedSlider.min_y + kEpsilon
            && hint.max_x + kEpsilon >= expectedSlider.max_x
            && hint.max_y + kEpsilon >= expectedSlider.max_y;
    };
    bool found = std::any_of(hints->begin(), hints->end(), covers_expected);
    INFO("slider expected dirty hint [" << expectedSlider.min_x << ", " << expectedSlider.min_y << ", "
         << expectedSlider.max_x << ", " << expectedSlider.max_y << "]");
    INFO("dirty hints count " << hints->size());
    for (auto const& hint : *hints) {
        INFO("dirty hint [" << hint.min_x << ", " << hint.min_y << ", "
             << hint.max_x << ", " << hint.max_y << "]");
    }
    CHECK(found);
}

TEST_CASE("Widget focus blur clears highlight footprint pixels") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("focus_blur_button", "FocusBlur").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("focus_blur_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto focusConfig = WidgetFocus::MakeConfig(fx.root_view(), std::nullopt);
    auto setFocus = WidgetFocus::Set(fx.space, focusConfig, button->root);
    REQUIRE(setFocus);
    CHECK(setFocus->changed);

    auto has_focus_highlight = [&](Widgets::ButtonPaths const& paths) {
        auto style = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(paths.root.getPath()) + "/meta/style");
        REQUIRE(style);
        auto state = fx.space.read<Widgets::ButtonState, std::string>(paths.state.getPath());
        REQUIRE(state);
        Widgets::ButtonPreviewOptions preview{};
        preview.authoring_root = paths.root.getPath();
        auto bucket = Widgets::BuildButtonPreview(*style, *state, preview);
        return std::any_of(bucket.authoring_map.begin(),
                           bucket.authoring_map.end(),
                           [](auto const& entry) {
                               return entry.authoring_node_id.find("focus/highlight") != std::string::npos;
                           });
    };
    CHECK(has_focus_highlight(*button));

    auto moveFocus = WidgetFocus::Set(fx.space, focusConfig, toggle->root);
    REQUIRE(moveFocus);
    CHECK(moveFocus->changed);

    CHECK_FALSE(has_focus_highlight(*button));
}

TEST_CASE("Widget focus set clears previous button focus state") {
    BuildersFixture fx;

    auto buttonAParams = Widgets::MakeButtonParams("focus_button_a", "ButtonA").Build();
    auto buttonBParams = Widgets::MakeButtonParams("focus_button_b", "ButtonB").Build();

    auto buttonA = Widgets::CreateButton(fx.space, fx.root_view(), buttonAParams);
    REQUIRE(buttonA);
    auto buttonB = Widgets::CreateButton(fx.space, fx.root_view(), buttonBParams);
    REQUIRE(buttonB);

    auto config = WidgetFocus::MakeConfig(fx.root_view());

    auto setA = WidgetFocus::Set(fx.space, config, buttonA->root);
    REQUIRE(setA);
    CHECK(setA->changed);

    auto stateA = fx.space.read<Widgets::ButtonState, std::string>(buttonA->state.getPath());
    REQUIRE(stateA);
    auto stateB = fx.space.read<Widgets::ButtonState, std::string>(buttonB->state.getPath());
    REQUIRE(stateB);

    CHECK(stateA->focused);
    CHECK_FALSE(stateB->focused);

    auto setB = WidgetFocus::Set(fx.space, config, buttonB->root);
    REQUIRE(setB);
    CHECK(setB->changed);

    stateA = fx.space.read<Widgets::ButtonState, std::string>(buttonA->state.getPath());
    REQUIRE(stateA);
    stateB = fx.space.read<Widgets::ButtonState, std::string>(buttonB->state.getPath());
    REQUIRE(stateB);

    CHECK_FALSE(stateA->focused);
    CHECK(stateB->focused);
}

TEST_CASE("Paint palette updates clear previous button focus") {
    BuildersFixture fx;

    RendererParams rendererParams{
        .name = "paint_focus_renderer",
        .kind = RendererKind::Software2D,
        .description = "Renderer"
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px = {320, 200};
    SurfaceParams surfaceParams{
        .name = "paint_focus_surface",
        .desc = surfaceDesc,
        .renderer = "renderers/paint_focus_renderer"
    };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/paint_focus_surface");
    REQUIRE(target);

    auto make_button = [&](std::string const& name, std::string const& label) {
        auto params = Widgets::MakeButtonParams(name, label)
                           .WithTheme(Widgets::MakeDefaultWidgetTheme())
                           .Build();
        auto buttons = Widgets::CreateButton(fx.space, fx.root_view(), params);
        REQUIRE(buttons);
        return *buttons;
    };

    std::vector<Widgets::ButtonPaths> buttonPaths;
    buttonPaths.push_back(make_button("palette_button_red", "Red"));
    buttonPaths.push_back(make_button("palette_button_blue", "Blue"));

    std::vector<WidgetBindings::ButtonBinding> bindings;
    for (auto const& paths : buttonPaths) {
        auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
        auto style = fx.space.read<Widgets::ButtonStyle, std::string>(stylePath);
        REQUIRE(style);
        WidgetInput::WidgetBounds bounds{0.0f, 0.0f, style->width, style->height};
        auto hint = WidgetInput::MakeDirtyHint(bounds);
        auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                           fx.root_view(),
                                                           paths,
                                                           SP::ConcretePathStringView{target->getPath()},
                                                           hint);
        REQUIRE(binding);
        bindings.push_back(std::move(*binding));
    }

    // Simulate the paint palette updating the focused button entry without clearing the previous one.
    auto ensure_focus = Widgets::SetExclusiveButtonFocus(
        fx.space,
        std::span<const Widgets::ButtonPaths>{buttonPaths.data(), buttonPaths.size()},
        std::optional<std::size_t>{0});
    REQUIRE(ensure_focus);

    auto stateFirstAfterFirst = fx.space.read<Widgets::ButtonState, std::string>(buttonPaths[0].state.getPath());
    REQUIRE(stateFirstAfterFirst);
    auto stateSecondAfterFirst = fx.space.read<Widgets::ButtonState, std::string>(buttonPaths[1].state.getPath());
    REQUIRE(stateSecondAfterFirst);
    CHECK(stateFirstAfterFirst->focused);
    CHECK_FALSE(stateSecondAfterFirst->focused);

    ensure_focus = Widgets::SetExclusiveButtonFocus(
        fx.space,
        std::span<const Widgets::ButtonPaths>{buttonPaths.data(), buttonPaths.size()},
        std::optional<std::size_t>{1});
    REQUIRE(ensure_focus);

    auto stateFirst = fx.space.read<Widgets::ButtonState, std::string>(buttonPaths[0].state.getPath());
    REQUIRE(stateFirst);
    auto stateSecond = fx.space.read<Widgets::ButtonState, std::string>(buttonPaths[1].state.getPath());
    REQUIRE(stateSecond);

    CHECK_FALSE(stateFirst->focused);
    CHECK(stateSecond->focused);
}

TEST_CASE("SetExclusiveButtonFocus clears button focus when no selection") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("palette_button_focus", "ColorButton")
                           .WithTheme(Widgets::MakeDefaultWidgetTheme())
                           .Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    std::array<Widgets::ButtonPaths const, 1> palette_buttons{*button};
    REQUIRE(Widgets::SetExclusiveButtonFocus(fx.space,
                                             palette_buttons,
                                             std::optional<std::size_t>{0}));

    auto focused_state = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(focused_state);
    CHECK(focused_state->focused);

    REQUIRE(Widgets::SetExclusiveButtonFocus(fx.space,
                                             palette_buttons,
                                             std::nullopt));

    auto cleared_state = fx.space.read<Widgets::ButtonState, std::string>(button->state.getPath());
    REQUIRE(cleared_state);
    CHECK_FALSE(cleared_state->focused);
}

TEST_CASE("Widget focus slider to button clears slider focus state") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("focus_toggle_button", "Button").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto sliderParams = Widgets::MakeSliderParams("focus_toggle_slider")
                           .WithTheme(Widgets::MakeDefaultWidgetTheme())
                           .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto config = WidgetFocus::MakeConfig(fx.root_view());

    auto setSlider = WidgetFocus::Set(fx.space, config, slider->root);
    REQUIRE(setSlider);

    auto sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK(sliderState->focused);

    auto setButton = WidgetFocus::Set(fx.space, config, button->root);
    REQUIRE(setButton);

    sliderState = fx.space.read<Widgets::SliderState, std::string>(slider->state.getPath());
    REQUIRE(sliderState);
    CHECK_FALSE(sliderState->focused);
}


TEST_CASE("Widget focus state publishes highlight drawable") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("focus_highlight_button", "FocusHighlight").Build();
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

TEST_CASE("Widget focus pulsing highlight sets pipeline flag") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("focus_pulse_button", "PulseHighlight").Build();
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

    bool found_pulsing = false;
    for (std::size_t index = 0; index < bucket->authoring_map.size(); ++index) {
        auto const& entry = bucket->authoring_map[index];
        if (entry.authoring_node_id.find("/focus/highlight") == std::string::npos) {
            continue;
        }
        REQUIRE(index < bucket->pipeline_flags.size());
        auto flags = bucket->pipeline_flags[index];
        CHECK((flags & SP::UI::PipelineFlags::HighlightPulse) != 0u);
        found_pulsing = true;
        break;
    }
    CHECK(found_pulsing);
}

TEST_CASE("Widgets::Focus keyboard navigation cycles focus order and schedules renders") {
    BuildersFixture fx;

    auto buttonParams = Widgets::MakeButtonParams("keyboard_focus_01_button", "KeyboardButton").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("keyboard_focus_02_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto sliderParams = Widgets::MakeSliderParams("keyboard_focus_03_slider")
                             .WithRange(0.0f, 1.0f)
                             .WithValue(0.42f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto listParams = Widgets::MakeListParams("keyboard_focus_04_list")
                          .WithItems({
                              Widgets::ListItem{.id = "one", .label = "One"},
                              Widgets::ListItem{.id = "two", .label = "Two"},
                              Widgets::ListItem{.id = "three", .label = "Three"},
                          })
                          .Build();
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

    auto buttonParams = Widgets::MakeButtonParams("gamepad_focus_button", "GamepadButton").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("gamepad_focus_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto sliderParams = Widgets::MakeSliderParams("gamepad_focus_slider")
                             .WithRange(0.0f, 1.0f)
                             .WithValue(0.7f)
                             .Build();
    auto slider = Widgets::CreateSlider(fx.space, fx.root_view(), sliderParams);
    REQUIRE(slider);

    auto listParams = Widgets::MakeListParams("gamepad_focus_list")
                          .WithItems({
                              Widgets::ListItem{.id = "north", .label = "North"},
                              Widgets::ListItem{.id = "south", .label = "South"},
                          })
                          .Build();
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

    auto listParams = Widgets::MakeListParams("inventory_bindings")
                          .WithItems({
                              Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
                              Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
                          })
                          .Build();
    auto listWidget = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(listWidget);

    auto listStyle = fx.space.read<Widgets::ListStyle, std::string>(std::string(listWidget->root.getPath()) + "/meta/style");
    REQUIRE(listStyle);
    auto listItems = fx.space.read<std::vector<Widgets::ListItem>, std::string>(listWidget->items.getPath());
    REQUIRE(listItems);
    std::size_t listCount = std::max<std::size_t>(listItems->size(), 1u);
    auto listFootprint = SP::UI::Builders::MakeDirtyRectHint(
        0.0f,
        0.0f,
        listStyle->width,
        listStyle->border_thickness * 2.0f + listStyle->item_height * static_cast<float>(listCount));

    auto binding = WidgetBindings::CreateListBinding(fx.space,
                                                     fx.root_view(),
                                                     *listWidget,
                                                     SP::ConcretePathStringView{target->getPath()},
                                                     listFootprint);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(10.0f, 18.0f)
                       .WithInside(true);

    auto selectState = Widgets::MakeListState()
                            .WithSelectedIndex(1)
                            .Build();

    auto selectResult = WidgetBindings::DispatchList(fx.space,
                                                     *binding,
                                                     selectState,
                                                     WidgetBindings::WidgetOpKind::ListSelect,
                                                     pointer,
                                                     1);
    REQUIRE(selectResult);
    CHECK(*selectResult);

    auto renderQueuePath = std::string(target->getPath()) + "/events/renderRequested/queue";
    auto selectReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(selectReasons, "widget/list");

    auto opQueuePath = binding->options.ops_queue.getPath();
    auto selectOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(selectOp);
    CHECK(selectOp->kind == WidgetBindings::WidgetOpKind::ListSelect);
    CHECK(selectOp->value == doctest::Approx(1.0f));

    auto hoverState = Widgets::MakeListState().Build();
    auto hoverResult = WidgetBindings::DispatchList(fx.space,
                                                    *binding,
                                                    hoverState,
                                                    WidgetBindings::WidgetOpKind::ListHover,
                                                    pointer,
                                                    0);
    REQUIRE(hoverResult);
    CHECK(*hoverResult);

    auto hoverReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(hoverReasons, "widget/list");

    auto hoverOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(hoverOp);
    CHECK(hoverOp->kind == WidgetBindings::WidgetOpKind::ListHover);
    CHECK(hoverOp->value == doctest::Approx(0.0f));

    auto scrollState = Widgets::MakeListState()
                            .WithScrollOffset(40.0f)
                            .Build();
    auto scrollResult = WidgetBindings::DispatchList(fx.space,
                                                     *binding,
                                                     scrollState,
                                                     WidgetBindings::WidgetOpKind::ListScroll,
                                                     pointer,
                                                     -1,
                                                     12.0f);
    REQUIRE(scrollResult);
    CHECK(*scrollResult);

    auto scrollReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    expect_auto_render_reason(scrollReasons, "widget/list");

    auto scrollOp = fx.space.take<WidgetBindings::WidgetOp, std::string>(opQueuePath);
    REQUIRE(scrollOp);
    CHECK(scrollOp->kind == WidgetBindings::WidgetOpKind::ListScroll);
    CHECK(scrollOp->value >= 0.0f);

    auto disabled = Widgets::MakeListState()
                         .WithEnabled(false)
                         .WithSelectedIndex(1)
                         .Build();

    auto disableResult = Widgets::UpdateListState(fx.space, *listWidget, disabled);
    REQUIRE(disableResult);
    CHECK(*disableResult);

    auto disableReasons = drain_auto_render_queue(fx.space, renderQueuePath);
    CHECK(disableReasons.empty());

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

    auto buttonParams = Widgets::MakeButtonParams("reducers_button", "Reducers").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto reducersButtonStyle = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(reducersButtonStyle);
    auto reducersButtonFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                                       0.0f,
                                                                       reducersButtonStyle->width,
                                                                       reducersButtonStyle->height);

    auto buttonBinding = WidgetBindings::CreateButtonBinding(fx.space,
                                                             fx.root_view(),
                                                             *button,
                                                             SP::ConcretePathStringView{target->getPath()},
                                                             reducersButtonFootprint);
    REQUIRE(buttonBinding);

    auto pointer = WidgetBindings::PointerInfo::Make(4.0f, 5.0f)
                       .WithInside(true);

    auto pressed = Widgets::MakeButtonState()
                        .WithPressed(true)
                        .WithHovered(true)
                        .Build();

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

    auto listParams = Widgets::MakeListParams("reducers_list")
                          .WithItems({
                              Widgets::ListItem{.id = "alpha", .label = "Alpha", .enabled = true},
                              Widgets::ListItem{.id = "beta", .label = "Beta", .enabled = true},
                          })
                          .Build();
    auto list = Widgets::CreateList(fx.space, fx.root_view(), listParams);
    REQUIRE(list);

    auto reducersListStyle = fx.space.read<Widgets::ListStyle, std::string>(std::string(list->root.getPath()) + "/meta/style");
    REQUIRE(reducersListStyle);
    auto reducersListItems = fx.space.read<std::vector<Widgets::ListItem>, std::string>(list->items.getPath());
    REQUIRE(reducersListItems);
    std::size_t reducersListCount = std::max<std::size_t>(reducersListItems->size(), 1u);
    auto reducersListFootprint = SP::UI::Builders::MakeDirtyRectHint(
        0.0f,
        0.0f,
        reducersListStyle->width,
        reducersListStyle->border_thickness * 2.0f
            + reducersListStyle->item_height * static_cast<float>(reducersListCount));

    auto listBinding = WidgetBindings::CreateListBinding(fx.space,
                                                         fx.root_view(),
                                                         *list,
                                                         SP::ConcretePathStringView{target->getPath()},
                                                         reducersListFootprint);
    REQUIRE(listBinding);

    auto listState = Widgets::MakeListState()
                          .WithSelectedIndex(1)
                          .Build();
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

TEST_CASE("Widgets::Reducers::ProcessPendingActions drains ops and publishes actions") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "process_renderer", .kind = RendererKind::Software2D, .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(renderer);

    SurfaceDesc desc{};
    desc.size_px = {320, 200};
    desc.progressive_tile_size_px = 32;

    SurfaceParams surfaceParams{ .name = "process_surface", .desc = desc, .renderer = "renderers/process_renderer" };
    auto surface = Surface::Create(fx.space, fx.root_view(), surfaceParams);
    REQUIRE(surface);

    auto target = Renderer::ResolveTargetBase(fx.space,
                                              fx.root_view(),
                                              *renderer,
                                              "targets/surfaces/process_surface");
    REQUIRE(target);

    auto buttonParams = Widgets::MakeButtonParams("process_button", "Process").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto style = fx.space.read<Widgets::ButtonStyle, std::string>(std::string(button->root.getPath()) + "/meta/style");
    REQUIRE(style);
    auto footprint = SP::UI::Builders::MakeDirtyRectHint(0.0f, 0.0f, style->width, style->height);

    auto binding = WidgetBindings::CreateButtonBinding(fx.space,
                                                       fx.root_view(),
                                                       *button,
                                                       SP::ConcretePathStringView{target->getPath()},
                                                       footprint);
    REQUIRE(binding);

    auto pointer = WidgetBindings::PointerInfo::Make(12.0f, 24.0f).WithInside(true);
    auto pressed = Widgets::MakeButtonState()
                       .WithPressed(true)
                       .WithHovered(true)
                       .Build();

    auto dispatched = WidgetBindings::DispatchButton(fx.space,
                                                     *binding,
                                                     pressed,
                                                     WidgetBindings::WidgetOpKind::Press,
                                                     pointer);
    REQUIRE(dispatched);
    CHECK(*dispatched);

    auto processed = WidgetReducers::ProcessPendingActions(fx.space, button->root);
    REQUIRE(processed);
    CHECK(processed->ops_queue.getPath() == WidgetReducers::WidgetOpsQueue(button->root).getPath());
    CHECK(processed->actions_queue.getPath()
          == WidgetReducers::DefaultActionsQueue(button->root).getPath());
    REQUIRE(processed->actions.size() == 1);

    auto const& action = processed->actions.front();
    CHECK(action.kind == WidgetBindings::WidgetOpKind::Press);
    CHECK(action.widget_path == button->root.getPath());
    CHECK(action.pointer.inside);
    CHECK(action.analog_value == doctest::Approx(1.0f));

    auto stored = fx.space.take<WidgetReducers::WidgetAction, std::string>(processed->actions_queue.getPath());
    REQUIRE(stored);
    CHECK(stored->widget_path == button->root.getPath());
    CHECK(stored->analog_value == doctest::Approx(1.0f));
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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

    auto ready = BuilderScene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
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
    auto scene = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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

    auto ready = BuilderScene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
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

    auto ready2 = BuilderScene::WaitUntilReady(fx.space, *scene, std::chrono::milliseconds{10});
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

    auto buttonParams = Widgets::MakeButtonParams("stack_binding_button", "Primary").Build();
    auto button = Widgets::CreateButton(fx.space, fx.root_view(), buttonParams);
    REQUIRE(button);

    auto toggleParams = Widgets::MakeToggleParams("stack_binding_toggle").Build();
    auto toggle = Widgets::CreateToggle(fx.space, fx.root_view(), toggleParams);
    REQUIRE(toggle);

    auto stackParams = Widgets::MakeStackLayoutParams("binding_stack")
                            .ModifyStyle([](Widgets::StackLayoutStyle& style) {
                                style.axis = Widgets::StackAxis::Vertical;
                                style.spacing = 12.0f;
                            })
                            .WithChildren({
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
    })
                            .Build();

    auto stack = Widgets::CreateStack(fx.space, fx.root_view(), stackParams);
    REQUIRE(stack);

    auto stackLayout = Widgets::ReadStackLayout(fx.space, *stack);
    REQUIRE(stackLayout);
    auto stackFootprint = SP::UI::Builders::MakeDirtyRectHint(0.0f,
                                                              0.0f,
                                                              stackLayout->width,
                                                              stackLayout->height);

    auto binding = WidgetBindings::CreateStackBinding(fx.space,
                                                      fx.root_view(),
                                                      *stack,
                                                      SP::ConcretePathStringView{target->getPath()},
                                                      stackFootprint);
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
    auto scenePath = BuilderScene::Create(fx.space, fx.root_view(), sceneParams);
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

TEST_CASE("Stack readiness helper waits for declarative stack children") {
    PathSpace space;
    auto stack_root = std::string{"/system/widgets/runtime/test_stack"};
    auto required = std::to_array<std::string_view>({"panel_a", "panel_b"});

    std::vector<std::string> log_lines;
    SP::UI::Declarative::StackReadinessOptions options{};
    options.timeout = std::chrono::milliseconds{500};
    options.poll_interval = std::chrono::milliseconds{10};
    options.verbose = true;
    options.log = [&](std::string_view line) {
        log_lines.emplace_back(line);
    };

    std::thread publisher([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        REQUIRE(space.insert(stack_root + "/children/panel_a", 1).nbrValuesInserted == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        REQUIRE(space.insert(stack_root + "/children/panel_b", 1).nbrValuesInserted == 1);
    });

    auto ready = SP::UI::Declarative::WaitForStackChildren(space,
                                                           stack_root,
                                                           std::span<const std::string_view>(required),
                                                           options);
    publisher.join();
    REQUIRE(ready);
    CHECK_FALSE(log_lines.empty());
    CHECK(std::any_of(log_lines.begin(), log_lines.end(), [](std::string const& line) {
        return line.find("panel_a") != std::string::npos || line.find("panel_b") != std::string::npos;
    }));
}

TEST_CASE("Stack readiness helper honors PATHSPACE_UI_DEBUG_STACK_LAYOUT env flag") {
    ScopedEnvVar verbose{"PATHSPACE_UI_DEBUG_STACK_LAYOUT", "1"};
    PathSpace space;
    auto stack_root = std::string{"/system/widgets/runtime/env_stack"};
    auto required = std::to_array<std::string_view>({"panel_env"});

    std::vector<std::string> log_lines;
    SP::UI::Declarative::StackReadinessOptions options{};
    options.timeout = std::chrono::milliseconds{250};
    options.poll_interval = std::chrono::milliseconds{20};
    options.log = [&](std::string_view line) {
        log_lines.emplace_back(line);
    };

    std::thread publisher([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        REQUIRE(space.insert(stack_root + "/children/panel_env", 1).nbrValuesInserted == 1);
    });

    auto ready = SP::UI::Declarative::WaitForStackChildren(space,
                                                           stack_root,
                                                           std::span<const std::string_view>(required),
                                                           options);
    publisher.join();
    REQUIRE(ready);
    CHECK_FALSE(log_lines.empty());
    CHECK(std::any_of(log_lines.begin(), log_lines.end(), [](std::string const& line) {
        return line.find("env_stack") != std::string::npos;
    }));
}

TEST_CASE("Stack readiness helper reports missing children on timeout") {
    PathSpace space;
    auto stack_root = std::string{"/system/widgets/runtime/never_ready"};
    auto required = std::to_array<std::string_view>({"missing_panel"});

    SP::UI::Declarative::StackReadinessOptions options{};
    options.timeout = std::chrono::milliseconds{60};
    options.poll_interval = std::chrono::milliseconds{10};

    auto ready = SP::UI::Declarative::WaitForStackChildren(space,
                                                           stack_root,
                                                           std::span<const std::string_view>(required),
                                                           options);
    REQUIRE_FALSE(ready);
    CHECK(ready.error().code == Error::Code::Timeout);
    REQUIRE(ready.error().message);
    CHECK(ready.error().message->find("missing_panel") != std::string::npos);
}

} // TEST_SUITE
