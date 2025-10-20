#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
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

using UIScene::DrawableBucketSnapshot;
using UIScene::SceneSnapshotBuilder;
using UIScene::SnapshotPublishOptions;
using UIScene::RectCommand;
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

    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    REQUIRE(render_html);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
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

    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    REQUIRE(render_html);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
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

    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    REQUIRE(render_html);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
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

    auto revision = Scene::ReadCurrentRevision(fx.space, created->scene);
    REQUIRE(revision);
    CHECK(revision->revision > 0);

    Widgets::ButtonState pressed_state = *state;
    pressed_state.pressed = true;
    auto changed = Widgets::UpdateButtonState(fx.space, *created, pressed_state);
    REQUIRE(changed);
    CHECK(*changed);

    auto updated = read_value<Widgets::ButtonState>(fx.space,
                                                    std::string(created->state.getPath()));
    REQUIRE(updated);
    CHECK(updated->pressed);

    auto unchanged = Widgets::UpdateButtonState(fx.space, *created, pressed_state);
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
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

    auto toggle_unchanged = Widgets::UpdateToggleState(fx.space, *created, toggled);
    REQUIRE(toggle_unchanged);
    CHECK_FALSE(*toggle_unchanged);
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

    HtmlTargetParams targetParams{};
    targetParams.name = "preview_assets";
    targetParams.scene = std::string("scenes/") + sceneParams.name;
    auto target = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *renderer, targetParams);
    REQUIRE(target);

    auto render_html = Renderer::RenderHtml(fx.space, ConcretePathView{target->getPath()});
    REQUIRE(render_html);

    auto htmlBase = std::string(target->getPath()) + "/output/v1/html";
    auto assets = read_value<std::vector<Asset>>(fx.space, htmlBase + "/assets");
    REQUIRE(assets);
    REQUIRE(assets->size() == 1);
    auto const& asset = assets->front();
    CHECK(asset.logical_path == logical_path);
    CHECK(asset.mime_type == "image/png");
    CHECK(asset.bytes == std::vector<std::uint8_t>(kTestPngRgba.begin(), kTestPngRgba.end()));

    auto manifest = read_value<std::vector<std::string>>(fx.space, htmlBase + "/assets/manifest");
    REQUIRE(manifest);
    REQUIRE(manifest->size() == 1);
    CHECK(manifest->front() == logical_path);

    auto dataPath = htmlBase + "/assets/data/" + logical_path;
    auto storedBytes = read_value<std::vector<std::uint8_t>>(fx.space, dataPath);
    REQUIRE(storedBytes);
    CHECK(*storedBytes == std::vector<std::uint8_t>(kTestPngRgba.begin(), kTestPngRgba.end()));

    auto mimePath = htmlBase + "/assets/meta/" + logical_path;
    auto storedMime = read_value<std::string>(fx.space, mimePath);
    REQUIRE(storedMime);
    CHECK(*storedMime == std::string{"image/png"});
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
    REQUIRE(manifest->size() == 1);

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
    REQUIRE(render_html2);

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

} // TEST_SUITE
