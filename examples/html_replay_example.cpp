#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlRunner.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
namespace Runtime = SP::UI::Runtime;
namespace UIScene = SP::UI::Scene;

namespace {

struct RendererFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/html_replay_example"};

    [[nodiscard]] auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_snapshot(ScenePath const& scenePath,
                          UIScene::DrawableBucketSnapshot bucket) -> std::uint64_t {
        UIScene::SceneSnapshotBuilder builder{space, root_view(), scenePath};
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "example";
        opts.metadata.tool_version = "example";
        opts.metadata.created_at = std::chrono::system_clock::time_point{};
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();
        auto revision = builder.publish(opts, bucket);
        assert(revision.has_value());
        return *revision;
    }
};

auto create_scene(RendererFixture& fx,
                  std::string const& name,
                  UIScene::DrawableBucketSnapshot bucket) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "HTML replay example scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    assert(scene.has_value());
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
    assert(renderer.has_value());
    return *renderer;
}

auto create_surface(RendererFixture& fx,
                    std::string const& name,
                    Runtime::SurfaceDesc desc,
                    std::string const& rendererName) -> SurfacePath {
    SurfaceParams params{};
    params.name = name;
    params.desc = desc;
    params.renderer = rendererName;
    auto surface = Surface::Create(fx.space, fx.root_view(), params);
    assert(surface.has_value());
    return *surface;
}

auto resolve_target(RendererFixture& fx,
                    SurfacePath const& surfacePath) -> SP::ConcretePathString {
    auto targetRel = fx.space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target");
    assert(targetRel.has_value());
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    assert(targetAbs.has_value());
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
    sphereA.center = {40.0f, 24.0f, 0.0f};
    sphereA.radius = std::sqrt(20.0f * 20.0f + 24.0f * 24.0f);
    UIScene::BoundingSphere sphereB{};
    sphereB.center = {88.0f, 40.0f, 0.0f};
    sphereB.radius = std::sqrt(16.0f * 16.0f + 16.0f * 16.0f);
    bucket.bounds_spheres = {sphereA, sphereB};

    UIScene::BoundingBox boxA{};
    boxA.min = {20.0f, 12.0f, 0.0f};
    boxA.max = {60.0f, 36.0f, 0.0f};
    UIScene::BoundingBox boxB{};
    boxB.min = {72.0f, 32.0f, 0.0f};
    boxB.max = {104.0f, 48.0f, 0.0f};
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
    bucket.drawable_fingerprints = {0xABCDu, 0xBCDFu};

    UIScene::RectCommand rect{};
    rect.min_x = 20.0f;
    rect.min_y = 12.0f;
    rect.max_x = 60.0f;
    rect.max_y = 36.0f;
    rect.color = {0.15f, 0.35f, 0.7f, 1.0f};
    append_command(bucket, UIScene::DrawCommandKind::Rect, rect);

    UIScene::RoundedRectCommand rounded{};
    rounded.min_x = 72.0f;
    rounded.min_y = 32.0f;
    rounded.max_x = 104.0f;
    rounded.max_y = 48.0f;
    rounded.radius_top_left = 4.0f;
    rounded.radius_top_right = 2.5f;
    rounded.radius_bottom_right = 3.0f;
    rounded.radius_bottom_left = 1.5f;
    rounded.color = {0.85f, 0.3f, 0.2f, 0.6f};
    append_command(bucket, UIScene::DrawCommandKind::RoundedRect, rounded);

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    return bucket;
}

auto render_to_buffer(RendererFixture& fx,
                      PathRenderer2D& renderer,
                      SP::ConcretePathString const& targetPath,
                      Runtime::SurfaceDesc const& desc,
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
    assert(render_result.has_value());

    std::vector<std::uint8_t> buffer(surface.frame_bytes(), 0);
    auto copied = surface.copy_buffered_frame(buffer);
    assert(copied.has_value());
    return buffer;
}

} // namespace

int main() {
    RendererFixture fx;
    PathRenderer2D renderer{fx.space};

    auto bucket = make_sample_bucket();
    auto scenePath = create_scene(fx, "html_replay_example_scene", bucket);
    auto rendererPath = create_renderer(fx, "html_replay_example_renderer");

    Runtime::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 128;
    surfaceDesc.size_px.height = 96;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "html_replay_example_surface", surfaceDesc, rendererPath.getPath());
    auto set_scene = Surface::SetScene(fx.space, surfacePath, scenePath);
    assert(set_scene.has_value());
    auto targetPath = resolve_target(fx, surfacePath);

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.surface.dpi_scale = 1.0f;
    settings.renderer.backend_kind = RendererKind::Software2D;

    auto baseline = render_to_buffer(fx,
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
    assert(emitted.has_value());
    Html::CanvasReplayOptions replay_opts{};
    replay_opts.stroke_points = emitted->stroke_points;
    auto replay_bucket = Html::commands_to_bucket(emitted->canvas_replay_commands, replay_opts);
    assert(replay_bucket.has_value());

    auto replay = render_to_buffer(fx,
                                   renderer,
                                   targetPath,
                                   surfaceDesc,
                                   settings,
                                   *replay_bucket,
                                   scenePath);

    bool matches = baseline == replay;
    std::cout << "HTML canvas replay " << (matches ? "matches" : "differs from")
              << " PathRenderer2D output (" << baseline.size() << " bytes compared)"
              << std::endl;

    return matches ? 0 : 1;
}
