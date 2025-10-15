#include "ext/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
using SP::UI::Scene::BoundingBox;
using SP::UI::Scene::BoundingSphere;
using SP::UI::Scene::DrawableAuthoringMapEntry;
using SP::UI::Scene::DrawableBucketSnapshot;
using SP::UI::Scene::SceneSnapshotBuilder;
using SP::UI::Scene::SnapshotPublishOptions;
using SP::UI::Scene::Transform;
using SP::UI::Scene::RectCommand;
using SP::UI::Scene::DrawCommandKind;

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
                  DrawableBucketSnapshot bucket) -> ScenePath {
    SceneParams params{
        .name = name,
        .description = "Test scene",
    };
    auto scene = Builders::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    fx.publish_snapshot(*scene, std::move(bucket));
    return *scene;
}

auto create_renderer(RendererFixture& fx, std::string const& name) -> RendererPath {
    RendererParams params{
        .name = name,
        .description = "Test renderer",
    };
    auto renderer = Renderer::Create(fx.space, fx.root_view(), params, RendererKind::Software2D);
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

struct LinearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

auto srgb_to_linear(float value) -> float {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto linear_to_srgb(float value) -> float {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

auto make_linear_color(std::array<float, 4> const& rgba) -> LinearColor {
    auto alpha = std::clamp(rgba[3], 0.0f, 1.0f);
    return LinearColor{
        .r = srgb_to_linear(rgba[0]) * alpha,
        .g = srgb_to_linear(rgba[1]) * alpha,
        .b = srgb_to_linear(rgba[2]) * alpha,
        .a = alpha,
    };
}

auto blend(LinearColor dest, LinearColor src) -> LinearColor {
    auto const inv = 1.0f - src.a;
    dest.r = src.r + dest.r * inv;
    dest.g = src.g + dest.g * inv;
    dest.b = src.b + dest.b * inv;
    dest.a = src.a + dest.a * inv;
    return dest;
}

auto encode_linear_to_bytes(LinearColor color,
                            Builders::SurfaceDesc const& desc,
                            bool encode_srgb) -> std::array<std::uint8_t, 4> {
    auto alpha = std::clamp(color.a, 0.0f, 1.0f);
    std::array<float, 3> premul{
        std::clamp(color.r, 0.0f, 1.0f),
        std::clamp(color.g, 0.0f, 1.0f),
        std::clamp(color.b, 0.0f, 1.0f),
    };

    std::array<float, 3> straight{0.0f, 0.0f, 0.0f};
    if (alpha > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            straight[i] = std::clamp(premul[i] / alpha, 0.0f, 1.0f);
        }
    }

    auto apply_channel = [&](float linear_value) -> float {
        if (encode_srgb) {
            return linear_to_srgb(linear_value);
        }
        return linear_value;
    };

    std::array<float, 3> encoded{};
    for (int i = 0; i < 3; ++i) {
        auto value = apply_channel(straight[i]);
        if (desc.premultiplied_alpha) {
            value *= alpha;
        }
        encoded[i] = std::clamp(value, 0.0f, 1.0f);
    }

    return {
        static_cast<std::uint8_t>(std::lround(encoded[0] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(encoded[1] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(encoded[2] * 255.0f)),
        static_cast<std::uint8_t>(std::lround(alpha * 255.0f)),
    };
}

auto copy_buffer(PathSurfaceSoftware& surface) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> buffer(surface.frame_bytes(), 0);
    auto copied = surface.copy_buffered_frame(buffer);
    REQUIRE(copied.has_value());
    return buffer;
}

} // namespace

TEST_SUITE("PathRenderer2D") {

TEST_CASE("render executes rect commands across passes and encodes pixels") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x010000u, 0x000100u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{2.0f, 2.0f, 0.0f}, 3.0f},
                             BoundingSphere{{2.0f, 2.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}},
        BoundingBox{{1.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.5f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node1", 0, 0},
    };

    RectCommand base_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 4.0f,
        .max_y = 4.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    encode_rect_command(base_rect, bucket);

    RectCommand overlay_rect{
        .min_x = 1.0f,
        .min_y = 1.0f,
        .max_x = 3.0f,
        .max_y = 3.0f,
        .color = {0.0f, 1.0f, 0.0f, 0.5f},
    };
    encode_rect_command(overlay_rect, bucket);

    auto scenePath = create_scene(fx, "scene_rects", bucket);
    auto rendererPath = create_renderer(fx, "renderer_rects");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_rects", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;
    settings.clear_color = {0.1f, 0.2f, 0.3f, 1.0f};
    settings.time.frame_index = 7;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 2);

    auto buffer = copy_buffer(surface);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 7);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/revision").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/alphaDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/culledDrawables").value() == 0);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 2);

    auto encode_srgb = true;
    auto clear_linear = make_linear_color(settings.clear_color);
    auto base_linear = make_linear_color(base_rect.color);
    auto overlay_linear = make_linear_color(overlay_rect.color);
    auto desc = surfaceDesc;

    auto pixel_bytes = [&](int x, int y) -> std::array<std::uint8_t, 4> {
        auto color = clear_linear;
        color = blend(color, base_linear);
        if (x >= 1 && x < 3 && y >= 1 && y < 3) {
            color = blend(color, overlay_linear);
        }
        auto encoded = encode_linear_to_bytes(color, desc, encode_srgb);
        if (surfaceDesc.pixel_format == PixelFormat::RGBA8Unorm_sRGB) {
            return encoded;
        }
        return encoded;
    };

    auto stride = surface.row_stride_bytes();
    auto &desc_ref = surfaceDesc;
    auto check_pixel = [&](int x, int y, std::array<std::uint8_t, 4> expected) {
        auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
        CHECK(buffer[offset + 0] == expected[0]);
        CHECK(buffer[offset + 1] == expected[1]);
        CHECK(buffer[offset + 2] == expected[2]);
        CHECK(buffer[offset + 3] == expected[3]);
    };

    check_pixel(0, 0, pixel_bytes(0, 0));
    check_pixel(1, 1, pixel_bytes(1, 1));
    check_pixel(3, 3, pixel_bytes(3, 3));

    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
}

TEST_CASE("render tracks culled drawables and executed commands") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x010000u, 0x020000u};
    bucket.world_transforms = {identity_transform(), identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{2.0f, 2.0f, 0.0f}, 3.0f},
                             BoundingSphere{{10.0f, 10.0f, 0.0f}, 1.0f}};
    bucket.bounds_boxes = {
        BoundingBox{{0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 0.0f}},
        BoundingBox{{10.0f, 10.0f, 0.0f}, {12.0f, 12.0f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.0f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0, 1};
    bucket.alpha_indices = {};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node0", 0, 0},
        DrawableAuthoringMapEntry{bucket.drawable_ids[1], "node1", 0, 0},
    };

    RectCommand first_rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.0f, 0.0f, 1.0f, 1.0f},
    };
    encode_rect_command(first_rect, bucket);

    RectCommand offscreen_rect{
        .min_x = 10.0f,
        .min_y = 10.0f,
        .max_x = 12.0f,
        .max_y = 12.0f,
        .color = {1.0f, 0.0f, 0.0f, 1.0f},
    };
    encode_rect_command(offscreen_rect, bucket);

    auto scenePath = create_scene(fx, "scene_cull", bucket);
    auto rendererPath = create_renderer(fx, "renderer_cull");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 4;
    surfaceDesc.size_px.height = 4;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_cull", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    REQUIRE(result);
    CHECK(result->drawable_count == 1);

    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 2);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/opaqueDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/culledDrawables").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);
}

TEST_CASE("render reports error when target scene binding missing") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {};
    bucket.world_transforms = {};

    auto scenePath = create_scene(fx, "scene_error", bucket);
    auto rendererPath = create_renderer(fx, "renderer_error");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_error", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    // Remove binding to induce error.
    (void)fx.space.take<std::string>(std::string(targetPath.getPath()) + "/scene");

    PathSurfaceSoftware surface{surfaceDesc};
    PathRenderer2D renderer{fx.space};

    RenderSettings settings{};
    settings.surface.size_px.width = surfaceDesc.size_px.width;
    settings.surface.size_px.height = surfaceDesc.size_px.height;

    auto result = renderer.render({
        .target_path = SP::ConcretePathStringView{targetPath.getPath()},
        .settings = settings,
        .surface = surface,
    });
    CHECK_FALSE(result);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(*lastError == "target missing scene binding");
}

TEST_CASE("Surface::RenderOnce drives renderer and stores metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0xABCDu};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
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
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.4f, 0.4f, 0.4f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_surface", bucket);
    auto rendererPath = create_renderer(fx, "renderer_surface");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_main", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    auto renderFuture = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(renderFuture);
    CHECK(renderFuture->ready());

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/drawableCount").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandsExecuted").value() == 1);

    auto storedSettings = Renderer::ReadSettings(fx.space, SP::ConcretePathStringView{targetPath.getPath()});
    REQUIRE(storedSettings);
    CHECK(storedSettings->time.frame_index == 1);

    auto second = Surface::RenderOnce(fx.space, surfacePath, std::nullopt);
    REQUIRE(second);
    CHECK(second->ready());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 2);
}

TEST_CASE("Window::Present renders and presents a frame with metrics") {
    RendererFixture fx;

    DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x123456u};
    bucket.world_transforms = {identity_transform()};
    bucket.bounds_spheres = {BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f}};
    bucket.bounds_boxes = {BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}}};
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
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {DrawableAuthoringMapEntry{bucket.drawable_ids[0], "node", 0, 0}};

    RectCommand rect{
        .min_x = 0.0f,
        .min_y = 0.0f,
        .max_x = 2.0f,
        .max_y = 2.0f,
        .color = {0.2f, 0.3f, 0.4f, 1.0f},
    };
    encode_rect_command(rect, bucket);

    auto scenePath = create_scene(fx, "scene_window", bucket);
    auto rendererPath = create_renderer(fx, "renderer_window");

    Builders::SurfaceDesc surfaceDesc{};
    surfaceDesc.size_px.width = 2;
    surfaceDesc.size_px.height = 2;
    surfaceDesc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    surfaceDesc.color_space = ColorSpace::sRGB;
    surfaceDesc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_window", surfaceDesc, rendererPath.getPath());
    REQUIRE(Surface::SetScene(fx.space, surfacePath, scenePath));

    Builders::WindowParams windowParams{};
    windowParams.name = "main_window";
    windowParams.title = "Test";
    windowParams.width = 640;
    windowParams.height = 480;
    auto windowPath = Builders::Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);
    REQUIRE(Builders::Window::AttachSurface(fx.space, *windowPath, "main", surfacePath));

    auto presentStatus = Builders::Window::Present(fx.space, *windowPath, "main");
    REQUIRE(presentStatus);

    auto targetPath = resolve_target(fx, surfacePath);
    auto metricsBase = std::string(targetPath.getPath()) + "/output/v1/common";
    CHECK(fx.space.read<uint64_t>(metricsBase + "/frameIndex").value() == 1);
    CHECK(fx.space.read<uint64_t>(metricsBase + "/commandCount").value() == 1);
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/lastPresentSkipped").value());
    CHECK(fx.space.read<bool>(metricsBase + "/presented").value());
    CHECK(fx.space.read<bool>(metricsBase + "/bufferedFrameConsumed").value());
    CHECK_FALSE(fx.space.read<bool>(metricsBase + "/usedProgressive").value());
    CHECK(fx.space.read<uint64_t>(metricsBase + "/progressiveTilesCopied").value() == 0);
    CHECK(fx.space.read<double>(metricsBase + "/waitBudgetMs").value() == doctest::Approx(16.0).epsilon(0.1));
    CHECK(fx.space.read<double>(metricsBase + "/presentMs").value() >= 0.0);
    auto lastError = fx.space.read<std::string, std::string>(metricsBase + "/lastError");
    REQUIRE(lastError);
    CHECK(lastError->empty());
}

} // TEST_SUITE
