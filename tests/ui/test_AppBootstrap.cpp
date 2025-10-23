#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace UIScene = SP::UI::Scene;
namespace Builders = SP::UI::Builders;

namespace {

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto make_scene_bucket() -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xB0057F00u};
    bucket.world_transforms = {identity_transform()};

    UIScene::BoundingSphere sphere{};
    sphere.center = {16.0f, 12.0f, 0.0f};
    sphere.radius = 20.0f;
    bucket.bounds_spheres = {sphere};

    UIScene::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {32.0f, 24.0f, 0.0f};
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
    bucket.alpha_indices = {};
    bucket.layer_indices = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {UIScene::DrawableAuthoringMapEntry{
        bucket.drawable_ids.front(), "bootstrap/rect", 0, 0}};
    bucket.drawable_fingerprints = {0xFACEB00Cu};

    UIScene::RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = 32.0f;
    rect.max_y = 24.0f;
    rect.color = {0.1f, 0.6f, 0.9f, 1.0f};

    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(rect));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

    return bucket;
}

struct PublishedScene {
    Builders::ScenePath path;
    std::uint64_t revision = 0;
};

struct BootstrapFixture {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/bootstrap_app"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_scene(UIScene::DrawableBucketSnapshot bucket) -> PublishedScene {
        Builders::SceneParams params{
            .name = "main_scene",
            .description = "Bootstrap scene",
        };
        auto scene = Builders::Scene::Create(space, root_view(), params);
        REQUIRE(scene);

        UIScene::SceneSnapshotBuilder builder{space, root_view(), *scene};
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "tests";
        opts.metadata.tool_version = "tests";
        opts.metadata.created_at = std::chrono::system_clock::time_point{};
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();

        auto revision = builder.publish(opts, bucket);
        REQUIRE(revision);

        return PublishedScene{*scene, *revision};
    }
};

} // namespace

TEST_CASE("App::Bootstrap renders and presents scene end-to-end") {
    BootstrapFixture fx;
    auto const published = fx.publish_scene(make_scene_bucket());

    Builders::App::BootstrapParams params{};
    params.view_name = "main";
    params.renderer.name = "bootstrap_renderer";
    params.renderer.kind = Builders::RendererKind::Software2D;
    params.surface.name = "bootstrap_surface";
    params.surface.desc.size_px.width = 64;
    params.surface.desc.size_px.height = 48;
    params.surface.desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    params.surface.desc.color_space = Builders::ColorSpace::sRGB;
    params.surface.desc.premultiplied_alpha = true;
    params.window.name = "bootstrap_window";
    params.window.title = "Bootstrap Test";
    params.window.width = params.surface.desc.size_px.width;
    params.window.height = params.surface.desc.size_px.height;
    params.window.scale = 1.0f;
    params.window.background = "#101010";
    params.present_policy.capture_framebuffer = true;
    params.present_policy.auto_render_on_present = false;

    auto bootstrap = Builders::App::Bootstrap(fx.space,
                                              fx.root_view(),
                                              published.path,
                                              params);
    REQUIRE(bootstrap);

    auto render_future = Builders::Surface::RenderOnce(fx.space,
                                                       bootstrap->surface,
                                                       std::nullopt);
    REQUIRE(render_future);
    CHECK(render_future->ready());

    auto present = Builders::Window::Present(fx.space,
                                             bootstrap->window,
                                             bootstrap->view_name);
    REQUIRE(present);

    auto const& stats = present->stats;
    CHECK(stats.presented);
    CHECK_FALSE(stats.skipped);
    CHECK(stats.backend_kind == "Software2D");
    CHECK(stats.frame.revision == published.revision);
    CHECK(stats.frame.frame_index >= 1);

    auto const width = bootstrap->surface_desc.size_px.width;
    auto const height = bootstrap->surface_desc.size_px.height;
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    auto const expected_bytes = static_cast<std::size_t>(width)
                                * static_cast<std::size_t>(height) * 4;
    CHECK(present->framebuffer.size() == expected_bytes);
    CHECK(std::any_of(present->framebuffer.begin(),
                      present->framebuffer.end(),
                      [](std::uint8_t value) { return value != 0; }));

    auto const target_base = std::string(bootstrap->target.getPath());
    auto const common_metrics = target_base + "/output/v1/common";
    auto frame_index = fx.space.read<uint64_t>(common_metrics + "/frameIndex");
    REQUIRE(frame_index);
    CHECK(*frame_index == stats.frame.frame_index);
    auto backend_kind = fx.space.read<std::string, std::string>(common_metrics + "/backendKind");
    REQUIRE(backend_kind);
    CHECK(*backend_kind == "Software2D");
    auto presented = fx.space.read<bool>(common_metrics + "/presented");
    REQUIRE(presented);
    CHECK(*presented);

    auto const window_metrics = std::string(bootstrap->window.getPath())
                                + "/diagnostics/metrics/live/views/"
                                + bootstrap->view_name + "/present";
    auto central_frame_index = fx.space.read<uint64_t>(window_metrics + "/frameIndex");
    REQUIRE(central_frame_index);
    CHECK(*central_frame_index == stats.frame.frame_index);
    auto central_backend = fx.space.read<std::string, std::string>(window_metrics + "/backendKind");
    REQUIRE(central_backend);
    CHECK(*central_backend == "Software2D");
    auto progressive_tiles = fx.space.read<uint64_t>(window_metrics + "/progressiveTilesCopied");
    REQUIRE(progressive_tiles);
    CHECK(*progressive_tiles == static_cast<uint64_t>(stats.progressive_tiles_copied));
    auto mirrored_revision = fx.space.read<uint64_t>(window_metrics + "/revision");
    REQUIRE(mirrored_revision);
    CHECK(*mirrored_revision == stats.frame.revision);

    auto stored_settings = Builders::Renderer::ReadSettings(fx.space,
                                                           SP::ConcretePathStringView{bootstrap->target.getPath()});
    REQUIRE(stored_settings);
    CHECK(stored_settings->surface.size_px.width == bootstrap->surface_desc.size_px.width);
    CHECK(stored_settings->surface.size_px.height == bootstrap->surface_desc.size_px.height);
    CHECK(stored_settings->renderer.backend_kind == params.renderer.kind);
}

TEST_CASE("App::Bootstrap configures present policy and renderer overrides when sizes are omitted") {
    BootstrapFixture fx;
    auto const published = fx.publish_scene(make_scene_bucket());

    Builders::App::BootstrapParams params{};
    params.view_name = "configured";
    params.renderer.name = "configured_renderer";
    params.surface.name = "configured_surface";
    params.surface.desc.size_px.width = 0;
    params.surface.desc.size_px.height = 0;
    params.window.name = "configured_window";
    params.window.title = "Configured Window";
    params.window.width = 0;
    params.window.height = 0;
    params.window.scale = 0.0f;
    params.present_policy.mode = SP::UI::PathWindowView::PresentMode::AlwaysFresh;
    params.present_policy.staleness_budget = std::chrono::milliseconds{24};
    params.present_policy.max_age_frames = 4;
    params.present_policy.frame_timeout = std::chrono::milliseconds{52};
    params.present_policy.vsync_align = false;
    params.present_policy.auto_render_on_present = false;
    params.present_policy.capture_framebuffer = true;

    Builders::RenderSettings override_settings{};
    override_settings.surface.size_px.width = 1920;
    override_settings.surface.size_px.height = 1080;
    override_settings.surface.dpi_scale = 1.5f;
    override_settings.renderer.backend_kind = Builders::RendererKind::Software2D;
    params.renderer_settings_override = override_settings;

    auto bootstrap = Builders::App::Bootstrap(fx.space,
                                              fx.root_view(),
                                              published.path,
                                              params);
    REQUIRE(bootstrap);

    CHECK(bootstrap->surface_desc.size_px.width == 1920);
    CHECK(bootstrap->surface_desc.size_px.height == 1080);
    CHECK(bootstrap->present_policy.mode == params.present_policy.mode);
    CHECK(bootstrap->present_policy.max_age_frames == params.present_policy.max_age_frames);
    CHECK(bootstrap->present_policy.auto_render_on_present == params.present_policy.auto_render_on_present);
    CHECK(bootstrap->present_policy.capture_framebuffer == params.present_policy.capture_framebuffer);
    CHECK(bootstrap->present_policy.staleness_budget_ms_value == doctest::Approx(24.0));
    CHECK(bootstrap->present_policy.frame_timeout_ms_value == doctest::Approx(52.0));

    auto const view_base = std::string(bootstrap->window.getPath())
                           + "/views/" + bootstrap->view_name;
    auto policy = fx.space.read<std::string, std::string>(view_base + "/present/policy");
    REQUIRE(policy);
    CHECK(*policy == "AlwaysFresh");
    auto staleness_budget = fx.space.read<double>(view_base + "/present/params/staleness_budget_ms");
    REQUIRE(staleness_budget);
    CHECK(*staleness_budget == doctest::Approx(24.0));
    auto frame_timeout = fx.space.read<double>(view_base + "/present/params/frame_timeout_ms");
    REQUIRE(frame_timeout);
    CHECK(*frame_timeout == doctest::Approx(52.0));
    auto max_age_frames = fx.space.read<uint64_t>(view_base + "/present/params/max_age_frames");
    REQUIRE(max_age_frames);
    CHECK(*max_age_frames == 4);
    auto vsync_align = fx.space.read<bool>(view_base + "/present/params/vsync_align");
    REQUIRE(vsync_align);
    CHECK_FALSE(*vsync_align);
    auto auto_render = fx.space.read<bool>(view_base + "/present/params/auto_render_on_present");
    REQUIRE(auto_render);
    CHECK_FALSE(*auto_render);
    auto capture_framebuffer = fx.space.read<bool>(view_base + "/present/params/capture_framebuffer");
    REQUIRE(capture_framebuffer);
    CHECK(*capture_framebuffer);

    auto const window_meta = std::string(bootstrap->window.getPath()) + "/meta";
    auto stored_width = fx.space.read<int>(window_meta + "/width");
    REQUIRE(stored_width);
    CHECK(*stored_width == 1920);
    auto stored_height = fx.space.read<int>(window_meta + "/height");
    REQUIRE(stored_height);
    CHECK(*stored_height == 1080);
    auto stored_scale = fx.space.read<float>(window_meta + "/scale");
    REQUIRE(stored_scale);
    CHECK(*stored_scale == doctest::Approx(1.0f));

    auto stored_settings = Builders::Renderer::ReadSettings(fx.space,
                                                           SP::ConcretePathStringView{bootstrap->target.getPath()});
    REQUIRE(stored_settings);
    CHECK(stored_settings->surface.size_px.width == 1920);
    CHECK(stored_settings->surface.size_px.height == 1080);
    CHECK(stored_settings->surface.dpi_scale == doctest::Approx(1.5f));
    CHECK(stored_settings->renderer.backend_kind == Builders::RendererKind::Software2D);
    CHECK(stored_settings->surface.visibility);

    auto dirty_hints = fx.space.read<std::vector<Builders::DirtyRectHint>>(std::string(bootstrap->target.getPath())
                                                                           + "/hints/dirtyRects");
    REQUIRE(dirty_hints);
    REQUIRE_FALSE(dirty_hints->empty());
    CHECK(std::all_of(dirty_hints->begin(),
                      dirty_hints->end(),
                      [](Builders::DirtyRectHint const& hint) {
                          return hint.max_x > hint.min_x && hint.max_y > hint.min_y;
                      }));
}

TEST_CASE("App::UpdateSurfaceSize refreshes surface and renderer settings") {
    BootstrapFixture fx;
    auto const published = fx.publish_scene(make_scene_bucket());

    Builders::App::BootstrapParams params{};
    params.renderer.name = "resize_renderer";
    params.renderer.kind = Builders::RendererKind::Software2D;
    params.surface.name = "resize_surface";
    params.surface.desc.size_px.width = 48;
    params.surface.desc.size_px.height = 36;
    params.surface.desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    params.surface.desc.color_space = Builders::ColorSpace::sRGB;
    params.surface.desc.premultiplied_alpha = true;
    params.window.name = "resize_window";
    params.window.title = "Resize Test";
    params.window.width = params.surface.desc.size_px.width;
    params.window.height = params.surface.desc.size_px.height;
    params.window.scale = 1.0f;
    params.present_policy.capture_framebuffer = true;

    auto bootstrap = Builders::App::Bootstrap(fx.space,
                                              fx.root_view(),
                                              published.path,
                                              params);
    REQUIRE(bootstrap);

    constexpr int kNewWidth = 96;
    constexpr int kNewHeight = 72;

    auto resize_status = Builders::App::UpdateSurfaceSize(fx.space,
                                                         *bootstrap,
                                                         kNewWidth,
                                                         kNewHeight);
    REQUIRE(resize_status);

    CHECK(bootstrap->surface_desc.size_px.width == kNewWidth);
    CHECK(bootstrap->surface_desc.size_px.height == kNewHeight);

    auto renderer_settings = Builders::Renderer::ReadSettings(fx.space,
                                                              SP::ConcretePathStringView{bootstrap->target.getPath()});
    REQUIRE(renderer_settings);
    CHECK(renderer_settings->surface.size_px.width == kNewWidth);
    CHECK(renderer_settings->surface.size_px.height == kNewHeight);

    auto surface_desc = fx.space.read<Builders::SurfaceDesc, std::string>(
        std::string(bootstrap->surface.getPath()) + "/desc");
    REQUIRE(surface_desc);
    CHECK(surface_desc->size_px.width == kNewWidth);
    CHECK(surface_desc->size_px.height == kNewHeight);

    auto target_desc = fx.space.read<Builders::SurfaceDesc, std::string>(
        std::string(bootstrap->target.getPath()) + "/desc");
    REQUIRE(target_desc);
    CHECK(target_desc->size_px.width == kNewWidth);
    CHECK(target_desc->size_px.height == kNewHeight);
}

TEST_CASE("App::Bootstrap rejects invalid view identifiers") {
    BootstrapFixture fx;
    auto const published = fx.publish_scene(make_scene_bucket());

    Builders::App::BootstrapParams params{};
    params.view_name = "invalid/view";

    auto bootstrap = Builders::App::Bootstrap(fx.space,
                                              fx.root_view(),
                                              published.path,
                                              params);
    REQUIRE_FALSE(bootstrap);
    CHECK(bootstrap.error().code == SP::Error::Code::InvalidPathSubcomponent);
}
