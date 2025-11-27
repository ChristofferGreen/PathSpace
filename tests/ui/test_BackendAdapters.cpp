#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/HtmlRunner.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

namespace {

struct BackendFixture {
    PathSpace   space;
    SP::App::AppRootPath app_root{"/system/applications/backend_adapters"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }
};

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

UIScene::DrawableBucketSnapshot make_integration_bucket() {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x1u, 0x2u};
    bucket.world_transforms = {identity_transform(), identity_transform()};

    UIScene::BoundingSphere sphereA{};
    sphereA.center = {18.0f, 18.0f, 0.0f};
    sphereA.radius = std::sqrt(18.0f * 18.0f + 18.0f * 18.0f);
    UIScene::BoundingSphere sphereB{};
    sphereB.center = {54.0f, 30.0f, 0.0f};
    sphereB.radius = std::sqrt(12.0f * 12.0f + 12.0f * 12.0f);
    bucket.bounds_spheres = {sphereA, sphereB};

    UIScene::BoundingBox boxA{};
    boxA.min = {6.0f, 6.0f, 0.0f};
    boxA.max = {30.0f, 30.0f, 0.0f};
    UIScene::BoundingBox boxB{};
    boxB.min = {46.0f, 18.0f, 0.0f};
    boxB.max = {62.0f, 42.0f, 0.0f};
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
    rect.min_x = 6.0f;
    rect.min_y = 6.0f;
    rect.max_x = 30.0f;
    rect.max_y = 30.0f;
    rect.color = {0.2f, 0.5f, 0.8f, 1.0f};
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(UIScene::RectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

    UIScene::RoundedRectCommand rounded{};
    rounded.min_x = 46.0f;
    rounded.min_y = 18.0f;
    rounded.max_x = 62.0f;
    rounded.max_y = 42.0f;
    rounded.radius_top_left = 3.0f;
    rounded.radius_top_right = 4.0f;
    rounded.radius_bottom_right = 2.0f;
    rounded.radius_bottom_left = 5.0f;
    rounded.color = {0.9f, 0.2f, 0.3f, 0.6f};
    offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rounded, sizeof(UIScene::RoundedRectCommand));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::RoundedRect));

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    return bucket;
}

Builders::ScenePath create_scene(BackendFixture& fx,
                                 std::string const& name,
                                 UIScene::DrawableBucketSnapshot const& bucket) {
    Builders::SceneParams params{
        .name = name,
        .description = "backend integration scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);

    UIScene::SceneSnapshotBuilder builder{fx.space, fx.root_view(), *scene};
    UIScene::SnapshotPublishOptions opts{};
    opts.metadata.author = "backend_adapters";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);
    (void)revision;
    return *scene;
}

Builders::RendererPath create_renderer(BackendFixture& fx,
                                       std::string const& name,
                                       Builders::RendererKind kind) {
    Builders::RendererParams params{
        .name = name,
        .kind = kind,
        .description = "backend integration renderer",
    };
    auto renderer = Builders::Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);
    return *renderer;
}

Builders::SurfacePath create_surface(BackendFixture& fx,
                                     std::string const& name,
                                     Builders::SurfaceDesc desc,
                                     std::string const& renderer) {
    Builders::SurfaceParams params{};
    params.name = name;
    params.desc = desc;
    params.renderer = renderer;
    auto surface = Builders::Surface::Create(fx.space, fx.root_view(), params);
    REQUIRE(surface);
    return *surface;
}

auto resolve_target(BackendFixture& fx,
                    Builders::SurfacePath const& surface) -> SP::ConcretePathString {
    auto target_rel = fx.space.read<std::string, std::string>(std::string(surface.getPath()) + "/target");
    REQUIRE(target_rel);
    auto target_abs = SP::App::resolve_app_relative(fx.root_view(), *target_rel);
    REQUIRE(target_abs);
    return SP::ConcretePathString{target_abs->getPath()};
}

std::vector<std::uint8_t> render_bucket_to_buffer(PathRenderer2D& renderer,
                                                  SP::ConcretePathString const& target_path,
                                                  Builders::SurfaceDesc const& desc,
                                                  Builders::RenderSettings const& settings,
                                                  UIScene::DrawableBucketSnapshot const& bucket,
                                                  BackendFixture& fx,
                                                  Builders::ScenePath const& scene_path) {
    UIScene::SceneSnapshotBuilder builder{fx.space, fx.root_view(), scene_path};
    UIScene::SnapshotPublishOptions opts{};
    opts.metadata.author = "backend_adapters";
    opts.metadata.tool_version = "tests";
    opts.metadata.created_at = std::chrono::system_clock::time_point{};
    opts.metadata.drawable_count = bucket.drawable_ids.size();
    opts.metadata.command_count = bucket.command_kinds.size();
    auto revision = builder.publish(opts, bucket);
    REQUIRE(revision);

    PathSurfaceSoftware surface{
        desc,
        PathSurfaceSoftware::Options{
            .enable_progressive = false,
            .enable_buffered = true,
            .progressive_tile_size_px = 32,
        },
    };

    auto render_result = renderer.render({
        .target_path = SP::ConcretePathStringView{target_path.getPath()},
        .settings = settings,
        .surface = surface,
        .backend_kind = Builders::RendererKind::Software2D,
    });
    REQUIRE(render_result);

    std::vector<std::uint8_t> buffer(surface.frame_bytes(), 0);
    auto copied = surface.copy_buffered_frame(buffer);
    REQUIRE(copied);
    return buffer;
}

} // namespace

TEST_CASE("Renderer integration replay retains framebuffer parity") {
    BackendFixture fx;
    PathRenderer2D renderer{fx.space};

    auto bucket = make_integration_bucket();
    auto scene = create_scene(fx, "integration_replay_scene", bucket);
    auto renderer_path = create_renderer(fx, "integration_renderer", Builders::RendererKind::Software2D);

    Builders::SurfaceDesc surface_desc{};
    surface_desc.size_px.width = 96;
    surface_desc.size_px.height = 64;
    surface_desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    surface_desc.color_space = Builders::ColorSpace::sRGB;
    surface_desc.premultiplied_alpha = true;

    auto surface = create_surface(fx, "integration_surface", surface_desc, renderer_path.getPath());
    REQUIRE(Builders::Surface::SetScene(fx.space, surface, scene));
    auto target_path = resolve_target(fx, surface);

    Builders::RenderSettings settings{};
    settings.surface.size_px.width = surface_desc.size_px.width;
    settings.surface.size_px.height = surface_desc.size_px.height;
    settings.surface.dpi_scale = 1.0f;
    settings.renderer.backend_kind = Builders::RendererKind::Software2D;
    settings.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    settings.time.frame_index = 0;

    auto baseline = render_bucket_to_buffer(renderer,
                                            target_path,
                                            surface_desc,
                                            settings,
                                            bucket,
                                            fx,
                                            scene);

    Html::Adapter adapter;
    Html::EmitOptions options{};
   options.prefer_dom = false;
   auto emitted = adapter.emit(bucket, options);
   REQUIRE(emitted);

    Html::CanvasReplayOptions replay_opts{};
    replay_opts.stroke_points = emitted->stroke_points;
    auto replay_bucket = Html::commands_to_bucket(emitted->canvas_replay_commands, replay_opts);
    REQUIRE(replay_bucket);

    settings.time.frame_index = 1;
    auto replay = render_bucket_to_buffer(renderer,
                                          target_path,
                                          surface_desc,
                                          settings,
                                          *replay_bucket,
                                          fx,
                                          scene);

    REQUIRE(baseline.size() == replay.size());
    CHECK(baseline == replay);
}

TEST_CASE("HtmlAdapter emits DOM/Canvas command parity") {
    auto bucket = make_integration_bucket();

    Html::Adapter adapter;

    Html::EmitOptions dom_options{};
    dom_options.prefer_dom = true;
    auto dom_emit = adapter.emit(bucket, dom_options);
    REQUIRE(dom_emit);

    Html::EmitOptions canvas_options{};
    canvas_options.prefer_dom = false;
    auto canvas_emit = adapter.emit(bucket, canvas_options);
    REQUIRE(canvas_emit);
    CHECK(canvas_emit->used_canvas_fallback);

    auto const& dom_cmds = dom_emit->canvas_replay_commands;
    auto const& canvas_cmds = canvas_emit->canvas_replay_commands;
    REQUIRE(canvas_cmds.size() == bucket.drawable_ids.size());
    REQUIRE(dom_cmds.size() == canvas_cmds.size());

    auto compare_commands = [](Html::CanvasCommand const& lhs,
                               Html::CanvasCommand const& rhs) {
        CHECK(lhs.type == rhs.type);
        CHECK(lhs.x == doctest::Approx(rhs.x));
        CHECK(lhs.y == doctest::Approx(rhs.y));
        CHECK(lhs.width == doctest::Approx(rhs.width));
        CHECK(lhs.height == doctest::Approx(rhs.height));
        for (std::size_t i = 0; i < lhs.color.size(); ++i) {
            CHECK(lhs.color[i] == doctest::Approx(rhs.color[i]));
        }
        CHECK(lhs.opacity == doctest::Approx(rhs.opacity));
        for (std::size_t i = 0; i < lhs.corner_radii.size(); ++i) {
            CHECK(lhs.corner_radii[i] == doctest::Approx(rhs.corner_radii[i]));
        }
    };

    for (std::size_t i = 0; i < canvas_cmds.size(); ++i) {
        compare_commands(canvas_cmds[i], dom_cmds[i]);
    }

    REQUIRE(canvas_cmds.size() == 2);
    auto const& cmd_rect = canvas_cmds[0];
    CHECK(cmd_rect.type == Html::CanvasCommandType::Rect);
    CHECK(cmd_rect.x == doctest::Approx(6.0f));
    CHECK(cmd_rect.y == doctest::Approx(6.0f));
    CHECK(cmd_rect.width == doctest::Approx(24.0f));
    CHECK(cmd_rect.height == doctest::Approx(24.0f));
    CHECK(cmd_rect.color[0] == doctest::Approx(0.2f));
    CHECK(cmd_rect.color[1] == doctest::Approx(0.5f));
    CHECK(cmd_rect.color[2] == doctest::Approx(0.8f));
    CHECK(cmd_rect.color[3] == doctest::Approx(1.0f));
    CHECK(cmd_rect.opacity == doctest::Approx(1.0f));

    auto const& cmd_rounded = canvas_cmds[1];
    CHECK(cmd_rounded.type == Html::CanvasCommandType::RoundedRect);
    CHECK(cmd_rounded.x == doctest::Approx(46.0f));
    CHECK(cmd_rounded.y == doctest::Approx(18.0f));
    CHECK(cmd_rounded.width == doctest::Approx(16.0f));
    CHECK(cmd_rounded.height == doctest::Approx(24.0f));
    CHECK(cmd_rounded.corner_radii[0] == doctest::Approx(3.0f));
    CHECK(cmd_rounded.corner_radii[1] == doctest::Approx(4.0f));
    CHECK(cmd_rounded.corner_radii[2] == doctest::Approx(2.0f));
    CHECK(cmd_rounded.corner_radii[3] == doctest::Approx(5.0f));
    CHECK(cmd_rounded.opacity == doctest::Approx(0.6f));
    CHECK(cmd_rounded.color[0] == doctest::Approx(0.9f));
    CHECK(cmd_rounded.color[1] == doctest::Approx(0.2f));
    CHECK(cmd_rounded.color[2] == doctest::Approx(0.3f));
    CHECK(cmd_rounded.color[3] == doctest::Approx(0.6f));
}
