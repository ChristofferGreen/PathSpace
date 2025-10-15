#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
using SP::UI::Scene::DrawableBucketSnapshot;
using SP::UI::Scene::DrawableAuthoringMapEntry;
using SP::UI::Scene::SceneSnapshotBuilder;
using SP::UI::Scene::SnapshotPublishOptions;
using SP::UI::Scene::Transform;
using SP::UI::Scene::BoundingSphere;
using SP::UI::Scene::BoundingBox;

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
                  DrawableBucketSnapshot bucket = {}) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "Test scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    fx.publish_snapshot(*scene, std::move(bucket));
    return *scene;
}

auto create_renderer(RendererFixture& fx, std::string const& name, RendererKind kind) -> RendererPath {
    RendererParams params{
        .name = name,
        .description = "Test renderer",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params, kind);
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

auto expected_byte(float value, float alpha, bool premultiplied) -> std::uint8_t {
    auto component = std::clamp(value, 0.0f, 1.0f);
    auto a = std::clamp(alpha, 0.0f, 1.0f);
    if (premultiplied) {
        component *= a;
    }
    return static_cast<std::uint8_t>(std::lround(component * 255.0f));
}

auto color_from_drawable(std::uint64_t drawableId) -> std::array<float, 4> {
    auto r = static_cast<float>(drawableId & 0xFFu) / 255.0f;
    auto g = static_cast<float>((drawableId >> 8) & 0xFFu) / 255.0f;
    auto b = static_cast<float>((drawableId >> 16) & 0xFFu) / 255.0f;
    if (r == 0.0f && g == 0.0f && b == 0.0f) {
        r = 0.9f;
        g = 0.9f;
        b = 0.9f;
    }
    return {r, g, b, 1.0f};
}

auto make_rect_bucket(float min_x,
                      float min_y,
                      float max_x,
                      float max_y,
                      std::uint64_t drawableId) -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {drawableId};

    Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    bucket.world_transforms.push_back(transform);

    BoundingSphere sphere{};
    sphere.center = {0.0f, 0.0f, 0.0f};
    sphere.radius = 1.0f;
    bucket.bounds_spheres.push_back(sphere);

    BoundingBox box{};
    box.min = {min_x, min_y, 0.0f};
    box.max = {max_x, max_y, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid = {1};

    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {0};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    bucket.layer_indices = {};
    bucket.command_kinds = {};
    bucket.command_payload = {};
    bucket.clip_nodes = {};
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{
        .drawable_id = drawableId,
        .authoring_node_id = "node",
        .drawable_index_within_node = 0,
        .generation = 0,
    }};

    return bucket;
}

} // namespace

TEST_SUITE("PathRenderer2D") {

TEST_CASE("render clears surface using settings clear color and publishes metrics") {
    RendererFixture fx;

    auto bucket = make_rect_bucket(1.0f, 1.0f, 3.0f, 3.0f, 0x112233u);
    auto scenePath = create_scene(fx, "main_scene", std::move(bucket));
    auto rendererPath = create_renderer(fx, "renderer2d", RendererKind::Software2D);

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "main_surface", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.time.frame_index = 5;
    settings.time.time_ms = 16.0;
    settings.time.delta_ms = 16.0;
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.25f, 0.5f, 0.75f, 1.0f};

    auto stats = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(stats);
    CHECK(stats->frame_index == 5);
    CHECK(stats->revision == 1);
    CHECK(stats->drawable_count == 1);

    std::vector<std::uint8_t> buffer(surface.frame_bytes());
    auto copy = surface.copy_buffered_frame(buffer);
    REQUIRE(copy);
    CHECK(copy->info.frame_index == 5);
    CHECK(copy->info.revision == 1);

    auto expectedClearR = expected_byte(settings.clear_color[0], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedClearG = expected_byte(settings.clear_color[1], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedClearB = expected_byte(settings.clear_color[2], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedClearA = expected_byte(settings.clear_color[3], settings.clear_color[3], false);

    auto drawableColor = color_from_drawable(0x112233u);
    auto expectedRectR = expected_byte(drawableColor[0], drawableColor[3], surfaceDesc.premultiplied_alpha);
    auto expectedRectG = expected_byte(drawableColor[1], drawableColor[3], surfaceDesc.premultiplied_alpha);
    auto expectedRectB = expected_byte(drawableColor[2], drawableColor[3], surfaceDesc.premultiplied_alpha);
    auto expectedRectA = expected_byte(drawableColor[3], drawableColor[3], false);

    for (int row = 0; row < surfaceDesc.size_px.height; ++row) {
        auto base = static_cast<std::size_t>(row) * surface.row_stride_bytes();
        for (int col = 0; col < surfaceDesc.size_px.width; ++col) {
            auto idx = base + static_cast<std::size_t>(col) * 4u;
            bool insideRect = (col >= 1 && col < 3 && row >= 1 && row < 3);
            auto expR = insideRect ? expectedRectR : expectedClearR;
            auto expG = insideRect ? expectedRectG : expectedClearG;
            auto expB = insideRect ? expectedRectB : expectedClearB;
            auto expA = insideRect ? expectedRectA : expectedClearA;
            CHECK(buffer[idx + 0] == expR);
            CHECK(buffer[idx + 1] == expG);
            CHECK(buffer[idx + 2] == expB);
            CHECK(buffer[idx + 3] == expA);
        }
    }

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 5);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/revision").value() == 1);
    CHECK(fx.space.read<double>(metricsBase + "/renderMs").value() >= 0.0);
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 1);
}

TEST_CASE("render reports error when target scene binding missing") {
    RendererFixture fx;

    auto scenePath = create_scene(fx, "main_scene");
    auto rendererPath = create_renderer(fx, "renderer2d", RendererKind::Software2D);

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto targetPath = resolve_target(fx, surfacePath);

    // Remove scene binding to trigger error.
    (void)fx.space.take<std::string>(std::string(targetPath.getPath()) + "/scene");

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.time.frame_index = 1;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    CHECK_FALSE(result);
    CHECK((result.error().code == SP::Error::Code::NoObjectFound
           || result.error().code == SP::Error::Code::NoSuchPath));

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "target missing scene binding");
}

TEST_CASE("Surface::RenderOnce drives renderer and records metrics") {
    RendererFixture fx;

    auto bucket = make_rect_bucket(0.0f, 0.0f, 4.0f, 4.0f, 0xABCD01u);
    auto scenePath = create_scene(fx, "scene_for_surface", std::move(bucket));
    auto rendererPath = create_renderer(fx, "renderer_pipeline", RendererKind::Software2D);

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_main", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto first = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(first);
    CHECK(first->ready());

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/revision").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 1);
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());

    auto storedSettings = Renderer::ReadSettings(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->time.frame_index == 1);

    auto second = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(second);
    CHECK(second->ready());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 2);
}

} // TEST_SUITE
