#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlRunner.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
namespace UIScene = SP::UI::Scene;

namespace {

struct RendererFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/html_replay"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_snapshot(ScenePath const& scenePath,
                          UIScene::DrawableBucketSnapshot bucket) -> std::uint64_t {
        UIScene::SceneSnapshotBuilder builder{space, root_view(), scenePath};
        UIScene::SnapshotPublishOptions opts{};
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
                  UIScene::DrawableBucketSnapshot bucket) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "HTML replay scene",
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
        .description = "HTML replay renderer",
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

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

template <typename Command>
void append_command(UIScene::DrawableBucketSnapshot& bucket,
                    UIScene::DrawCommandKind kind,
                    Command const& command) {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(Command));
    std::memcpy(bucket.command_payload.data() + offset, &command, sizeof(Command));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(kind));
}

auto make_sample_bucket() -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {1, 2};
    bucket.world_transforms = {identity_transform(), identity_transform()};

    UIScene::BoundingSphere sphereA{};
    sphereA.center = {24.0f, 18.0f, 0.0f};
    sphereA.radius = std::sqrt(24.0f * 24.0f + 18.0f * 18.0f);
    UIScene::BoundingSphere sphereB{};
    sphereB.center = {70.0f, 48.0f, 0.0f};
    sphereB.radius = std::sqrt(18.0f * 18.0f + 18.0f * 18.0f);
    bucket.bounds_spheres = {sphereA, sphereB};

    UIScene::BoundingBox boxA{};
    boxA.min = {12.0f, 9.0f, 0.0f};
    boxA.max = {36.0f, 27.0f, 0.0f};
    UIScene::BoundingBox boxB{};
    boxB.min = {61.0f, 39.0f, 0.0f};
    boxB.max = {79.0f, 57.0f, 0.0f};
    bucket.bounds_boxes = {boxA, boxB};
    bucket.bounds_box_valid = {1, 1};

    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 1.0f};
    bucket.material_ids = {0, 0};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.clip_head_indices = {-1, -1};
    bucket.drawable_fingerprints = {0x101u, 0x202u};

    UIScene::RectCommand rect{};
    rect.min_x = 12.0f;
    rect.min_y = 9.0f;
    rect.max_x = 36.0f;
    rect.max_y = 27.0f;
    rect.color = {0.2f, 0.4f, 0.6f, 1.0f};
    append_command(bucket, UIScene::DrawCommandKind::Rect, rect);

    UIScene::RoundedRectCommand rounded{};
    rounded.min_x = 61.0f;
    rounded.min_y = 39.0f;
    rounded.max_x = 79.0f;
    rounded.max_y = 57.0f;
    rounded.radius_top_left = 3.0f;
    rounded.radius_top_right = 3.5f;
    rounded.radius_bottom_right = 2.5f;
    rounded.radius_bottom_left = 4.0f;
    rounded.color = {0.7f, 0.2f, 0.1f, 0.5f};
    append_command(bucket, UIScene::DrawCommandKind::RoundedRect, rounded);

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    return bucket;
}

auto render_to_buffer(RendererFixture& fx,
                      PathRenderer2D& renderer,
                      SP::ConcretePathString const& targetPath,
                      Builders::SurfaceDesc const& desc,
                      Builders::RenderSettings const& settings,
                      UIScene::DrawableBucketSnapshot const& bucket,
                      UIScene::ScenePath const& scenePath) -> std::vector<std::uint8_t> {
    fx.publish_snapshot(scenePath, bucket);
    PathSurfaceSoftware surface{
        desc,
        PathSurfaceSoftware::Options{
            .enable_progressive = false,
            .enable_buffered = true,
            .progressive_tile_size_px = 32,
        },
    };

    auto render_result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    REQUIRE(render_result);

    std::vector<std::uint8_t> buffer(surface.frame_bytes(), 0);
    auto copied = surface.copy_buffered_frame(buffer);
    REQUIRE(copied.has_value());
    return buffer;
}

} // namespace

TEST_CASE("HTML canvas replay matches PathRenderer2D output") {
    RendererFixture fx;
    PathRenderer2D renderer{fx.space};

    auto bucket = make_sample_bucket();
    auto scenePath = create_scene(fx, "html_replay_scene", bucket);
    auto rendererPath = create_renderer(fx, "html_replay_renderer");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 96;
    surfaceDesc.size_px.height = 72;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "html_replay_surface", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.surface.dpi_scale = 1.0f;
    settings.renderer.backend_kind = RendererKind::Software2D;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};

    auto primary_buffer = render_to_buffer(fx,
                                           renderer,
                                           targetPath,
                                           surfaceDesc,
                                           settings,
                                           bucket,
                                           scenePath);

    Html::Adapter adapter;
    Html::EmitOptions options{};
    options.prefer_dom = false;
    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);
    REQUIRE(emitted->used_canvas_fallback);
    REQUIRE_FALSE(emitted->canvas_replay_commands.empty());

    auto replay_bucket = Html::commands_to_bucket(emitted->canvas_replay_commands);
    REQUIRE(replay_bucket);

    auto replay_buffer = render_to_buffer(fx,
                                          renderer,
                                          targetPath,
                                          surfaceDesc,
                                          settings,
                                          *replay_bucket,
                                          scenePath);

    REQUIRE(primary_buffer.size() == replay_buffer.size());
    CHECK(primary_buffer == replay_buffer);
}
