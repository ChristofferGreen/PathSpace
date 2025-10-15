#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

using namespace SP;
using namespace SP::UI::Builders;

struct BuildersFixture {
    PathSpace     space;
    AppRootPath   app_root{"/system/applications/test_app"};
    auto root_view() const -> SP::App::AppRootPathView { return SP::App::AppRootPathView{app_root.getPath()}; }
};

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
    settings.microtri_rt.microtri_edge_px = 0.75f;
    settings.microtri_rt.max_microtris_per_frame = 150000;
    settings.microtri_rt.rays_per_vertex = 2;
    settings.microtri_rt.max_bounces = 2;
    settings.microtri_rt.rr_start_bounce = 1;
    settings.microtri_rt.use_hardware_rt = RenderSettings::MicrotriRT::HardwareMode::ForceOn;
    settings.microtri_rt.environment.hdr_path = "/assets/hdr/sunrise.hdr";
    settings.microtri_rt.environment.intensity = 1.5f;
    settings.microtri_rt.environment.rotation = 0.25f;
    settings.microtri_rt.allow_caustics = true;
    settings.microtri_rt.clamp_direct = 5.0f;
    settings.microtri_rt.clamp_indirect = 10.0f;
    settings.microtri_rt.progressive_accumulation = true;
    settings.microtri_rt.vertex_accum_half_life = 0.4f;
    settings.microtri_rt.seed = 12345u;
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

    RendererParams rendererParams{ .name = "2d", .description = "Software renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
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
    CHECK(stored->microtri_rt.microtri_edge_px == doctest::Approx(settings.microtri_rt.microtri_edge_px));
    CHECK(stored->microtri_rt.max_microtris_per_frame == settings.microtri_rt.max_microtris_per_frame);
    CHECK(stored->microtri_rt.rays_per_vertex == settings.microtri_rt.rays_per_vertex);
    CHECK(stored->microtri_rt.max_bounces == settings.microtri_rt.max_bounces);
    CHECK(stored->microtri_rt.rr_start_bounce == settings.microtri_rt.rr_start_bounce);
    CHECK(stored->microtri_rt.environment.hdr_path == settings.microtri_rt.environment.hdr_path);
    CHECK(stored->microtri_rt.environment.intensity == doctest::Approx(settings.microtri_rt.environment.intensity));
    CHECK(stored->microtri_rt.environment.rotation == doctest::Approx(settings.microtri_rt.environment.rotation));
    CHECK(stored->microtri_rt.allow_caustics == settings.microtri_rt.allow_caustics);
    CHECK(stored->microtri_rt.clamp_direct == settings.microtri_rt.clamp_direct);
    CHECK(stored->microtri_rt.clamp_indirect == settings.microtri_rt.clamp_indirect);
    CHECK(stored->microtri_rt.progressive_accumulation == settings.microtri_rt.progressive_accumulation);
    CHECK(stored->microtri_rt.vertex_accum_half_life == doctest::Approx(settings.microtri_rt.vertex_accum_half_life));
    CHECK(stored->microtri_rt.seed == settings.microtri_rt.seed);
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

    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
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

    auto empty = fx.space.take<RenderSettings>(settingsPath);
    CHECK_FALSE(empty);
    auto code = empty.error().code;
    bool is_expected = (code == Error::Code::NoObjectFound) || (code == Error::Code::NoSuchPath);
    CHECK(is_expected);
}

TEST_CASE("Surface creation binds renderer and scene") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
    REQUIRE(renderer);

    SurfaceDesc desc;
    desc.size_px = {1280, 720};
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.color_space = ColorSpace::DisplayP3;
    desc.premultiplied_alpha = false;

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

TEST_CASE("Window attach surface records binding") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
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
    CHECK(present.error().code == Error::Code::UnknownError);
}

TEST_CASE("Renderer::ResolveTargetBase rejects empty specifications") {
    BuildersFixture fx;
    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
    REQUIRE(renderer);

    auto target = Renderer::ResolveTargetBase(fx.space, fx.root_view(), *renderer, "");
    CHECK_FALSE(target);
    CHECK(target.error().code == Error::Code::InvalidPath);
}

TEST_CASE("Window::AttachSurface enforces shared app roots") {
    BuildersFixture fx;

    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
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

    RendererParams rendererParams{ .name = "2d", .description = "Renderer" };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), rendererParams, RendererKind::Software2D);
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
    CHECK(metrics->last_present_skipped == false);
    CHECK(metrics->last_error.empty());

    auto common = std::string(targetBase->getPath()) + "/output/v1/common";
    fx.space.insert(common + "/frameIndex", uint64_t{7});
    fx.space.insert(common + "/revision", uint64_t{13});
    fx.space.insert(common + "/renderMs", 8.5);
    fx.space.insert(common + "/presentMs", 4.25);
    fx.space.insert(common + "/lastPresentSkipped", true);
    fx.space.insert(common + "/lastError", std::string{"failure"});

    auto updated = Diagnostics::ReadTargetMetrics(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(updated);
    CHECK(updated->frame_index == 7);
    CHECK(updated->revision == 13);
    CHECK(updated->render_ms == doctest::Approx(8.5));
    CHECK(updated->present_ms == doctest::Approx(4.25));
    CHECK(updated->last_present_skipped == true);
    CHECK(updated->last_error == "failure");

    auto cleared = Diagnostics::ClearTargetError(fx.space, ConcretePathView{targetBase->getPath()});
    REQUIRE(cleared);

    auto clearedValue = read_value<std::string>(fx.space, common + "/lastError");
    REQUIRE(clearedValue);
    CHECK(clearedValue->empty());
}

} // TEST_SUITE
