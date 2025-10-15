#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
using SP::UI::Scene::DrawableBucketSnapshot;
using SP::UI::Scene::SceneSnapshotBuilder;
using SP::UI::Scene::SnapshotPublishOptions;

namespace {

struct RendererFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/test_app"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_empty_snapshot(ScenePath const& scenePath) -> std::uint64_t {
        SceneSnapshotBuilder builder{space, root_view(), scenePath};
        DrawableBucketSnapshot bucket{};
        SnapshotPublishOptions opts{};
        opts.metadata.author = "tests";
        opts.metadata.tool_version = "tests";
        opts.metadata.created_at = std::chrono::system_clock::time_point{};
        auto revision = builder.publish(opts, bucket);
        REQUIRE(revision);
        return *revision;
    }
};

auto create_scene(RendererFixture& fx, std::string const& name) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "Test scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    fx.publish_empty_snapshot(*scene);
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

} // namespace

TEST_SUITE("PathRenderer2D") {

TEST_CASE("render clears surface using settings clear color and publishes metrics") {
    RendererFixture fx;

    auto scenePath = create_scene(fx, "main_scene");
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
    CHECK(stats->drawable_count == 0);

    std::vector<std::uint8_t> buffer(surface.frame_bytes());
    auto copy = surface.copy_buffered_frame(buffer);
    REQUIRE(copy);
    CHECK(copy->info.frame_index == 5);
    CHECK(copy->info.revision == 1);

    auto expectedR = expected_byte(settings.clear_color[0], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedG = expected_byte(settings.clear_color[1], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedB = expected_byte(settings.clear_color[2], settings.clear_color[3], surfaceDesc.premultiplied_alpha);
    auto expectedA = expected_byte(settings.clear_color[3], settings.clear_color[3], false);

    for (int row = 0; row < surfaceDesc.size_px.height; ++row) {
        auto base = static_cast<std::size_t>(row) * surface.row_stride_bytes();
        for (int col = 0; col < surfaceDesc.size_px.width; ++col) {
            auto idx = base + static_cast<std::size_t>(col) * 4u;
            CHECK(buffer[idx + 0] == expectedR);
            CHECK(buffer[idx + 1] == expectedG);
            CHECK(buffer[idx + 2] == expectedB);
            CHECK(buffer[idx + 3] == expectedA);
        }
    }

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 5);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/revision").value() == 1);
    CHECK(fx.space.read<double>(metricsBase + "/renderMs").value() >= 0.0);
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
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

} // TEST_SUITE
