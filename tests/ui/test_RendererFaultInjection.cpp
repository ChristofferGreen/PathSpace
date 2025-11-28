#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
namespace Runtime = SP::UI::Runtime;
namespace UIScene = SP::UI::Scene;

namespace {

struct ScopedEnv {
    std::string name;
    std::optional<std::string> previous;

    ScopedEnv(char const* key, char const* value) : name(key) {
        if (auto* existing = std::getenv(key)) {
            previous = std::string(existing);
        }
        if (value) {
            ::setenv(name.c_str(), value, 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    ~ScopedEnv() {
        if (previous.has_value()) {
            ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

struct RendererFixture {
    PathSpace   space;
    AppRootPath app_root{"/system/applications/fault_harness"};

    auto app_root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_snapshot(ScenePath const& scenePath,
                          UIScene::DrawableBucketSnapshot bucket) -> std::uint64_t {
        UIScene::SceneSnapshotBuilder builder{space, app_root_view(), scenePath};
        UIScene::SnapshotPublishOptions opts{};
        opts.metadata.author = "test";
        opts.metadata.tool_version = "test";
        opts.metadata.drawable_count = bucket.drawable_ids.size();
        opts.metadata.command_count = bucket.command_kinds.size();
        auto revision = builder.publish(opts, std::move(bucket));
        REQUIRE(revision);
        return *revision;
    }
};

struct RectDrawableDef {
    std::uint64_t id = 0;
    std::uint64_t fingerprint = 0;
    UIScene::RectCommand rect{};
};

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform transform{};
    auto& m = transform.elements;
    m = {1.0f, 0.0f, 0.0f, 0.0f,
         0.0f, 1.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 1.0f};
    return transform;
}

auto make_rect_bucket(std::vector<RectDrawableDef> const& defs) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    auto count = defs.size();
    bucket.drawable_ids.reserve(count);
    bucket.world_transforms.reserve(count);
    bucket.bounds_spheres.reserve(count);
    bucket.bounds_boxes.reserve(count);
    bucket.bounds_box_valid.reserve(count);
    bucket.layers.reserve(count);
    bucket.z_values.reserve(count);
    bucket.material_ids.reserve(count);
    bucket.pipeline_flags.reserve(count);
    bucket.visibility.reserve(count);
    bucket.command_offsets.reserve(count);
    bucket.command_counts.reserve(count);
    bucket.clip_head_indices.reserve(count);
    bucket.authoring_map.reserve(count);
    bucket.drawable_fingerprints.reserve(count);

    for (std::size_t index = 0; index < defs.size(); ++index) {
        auto const& def = defs[index];
        bucket.drawable_ids.push_back(def.id);
        bucket.world_transforms.push_back(identity_transform());

        UIScene::BoundingBox box{};
        box.min = {def.rect.min_x, def.rect.min_y, 0.0f};
        box.max = {def.rect.max_x, def.rect.max_y, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        auto width = std::max(def.rect.max_x - def.rect.min_x, 0.0f);
        auto height = std::max(def.rect.max_y - def.rect.min_y, 0.0f);
        float radius = std::sqrt(width * width + height * height) * 0.5f;
        UIScene::BoundingSphere sphere{};
        sphere.center = {(def.rect.min_x + def.rect.max_x) * 0.5f,
                         (def.rect.min_y + def.rect.max_y) * 0.5f,
                         0.0f};
        sphere.radius = radius;
        bucket.bounds_spheres.push_back(sphere);

        bucket.layers.push_back(0);
        bucket.z_values.push_back(static_cast<float>(index));
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.command_offsets.push_back(static_cast<std::uint32_t>(bucket.command_kinds.size()));
        bucket.command_counts.push_back(1);
        bucket.clip_head_indices.push_back(-1);
        bucket.authoring_map.push_back(UIScene::DrawableAuthoringMapEntry{
            def.id,
            "drawable_" + std::to_string(index),
            0,
            0,
        });
        bucket.drawable_fingerprints.push_back(def.fingerprint);

        auto offset = bucket.command_payload.size();
        bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
        std::memcpy(bucket.command_payload.data() + offset,
                    &def.rect,
                    sizeof(UIScene::RectCommand));
        bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));
    }

    bucket.opaque_indices.resize(defs.size());
    std::iota(bucket.opaque_indices.begin(), bucket.opaque_indices.end(), 0u);
    bucket.alpha_indices.clear();

    return bucket;
}

auto create_scene(RendererFixture& fx,
                  std::string const& name,
                  UIScene::DrawableBucketSnapshot bucket) -> ScenePath {
    SceneParams params{};
    params.name = name;
    params.description = "Fault harness scene";
    auto scene = Builders::Scene::Create(fx.space, fx.app_root_view(), params);
    REQUIRE(scene);
    fx.publish_snapshot(*scene, std::move(bucket));
    return *scene;
}

auto create_renderer(RendererFixture& fx,
                     std::string const& name,
                     RendererKind kind) -> RendererPath {
    RendererParams params{};
    params.name = name;
    params.description = "Fault harness renderer";
    params.kind = kind;
    auto renderer = Builders::Renderer::Create(fx.space, fx.app_root_view(), params);
    REQUIRE(renderer);
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
    auto surface = Builders::Surface::Create(fx.space, fx.app_root_view(), params);
    REQUIRE(surface);
    return *surface;
}

auto resolve_target(RendererFixture& fx,
                    SurfacePath const& surfacePath) -> SP::ConcretePathString {
    auto rel = fx.space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target");
    REQUIRE(rel);
    auto abs = SP::App::resolve_app_relative(fx.app_root_view(), *rel);
    REQUIRE(abs);
    return *abs;
}

struct SimpleRenderFixture {
    RendererFixture fx;
    ScenePath scene;
    RendererPath renderer;
    SurfacePath surface;
    SP::ConcretePathString target_path;
    Runtime::SurfaceDesc surface_desc{};

    SimpleRenderFixture(RendererKind kind = RendererKind::Software2D) {
        auto bucket = make_rect_bucket({RectDrawableDef{
            .id = 1,
            .fingerprint = 1234,
            .rect = UIScene::RectCommand{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = 64.0f,
                .max_y = 64.0f,
                .color = {0.2f, 0.4f, 0.6f, 1.0f},
            },
        }});

        scene = create_scene(fx, "scene", std::move(bucket));
        renderer = create_renderer(fx, "renderer", kind);

        surface_desc.size_px = {.width = 128, .height = 128};
        surface_desc.pixel_format = Runtime::PixelFormat::RGBA8Unorm;
        surface_desc.color_space = Runtime::ColorSpace::sRGB;
        surface_desc.premultiplied_alpha = true;
        surface = create_surface(fx, "surface", surface_desc, renderer.getPath());
        target_path = resolve_target(fx, surface);

        auto scene_binding = fx.space.insert(std::string(target_path.getPath()) + "/scene", std::string(scene.getPath()));
        REQUIRE(scene_binding.errors.empty());
    }
};

auto default_render_settings(Runtime::SurfaceDesc const& desc) -> Builders::RenderSettings {
    Builders::RenderSettings settings{};
    settings.surface.size_px.width = desc.size_px.width;
    settings.surface.size_px.height = desc.size_px.height;
    return settings;
}

} // namespace

TEST_SUITE("Renderer fault harness") {
    TEST_CASE("surface descriptor mismatch reports last error") {
        SimpleRenderFixture fixture{};
        PathRenderer2D renderer{fixture.fx.space};

        PathSurfaceSoftware surface{fixture.surface_desc};
        auto settings = default_render_settings(fixture.surface_desc);
        settings.surface.size_px.width = fixture.surface_desc.size_px.width + 16; // intentionally mismatch

        PathRenderer2D::RenderParams params{
            .target_path = SP::ConcretePathStringView{fixture.target_path.getPath()},
            .settings = settings,
            .surface = surface,
        };

        auto result = renderer.render(params);
        CHECK_FALSE(result);

        auto last_error = Builders::Diagnostics::ReadTargetError(fixture.fx.space,
                                                                 SP::ConcretePathStringView{fixture.target_path.getPath()});
        REQUIRE(last_error);
        REQUIRE(last_error->has_value());
        auto const& error = last_error->value();
        CHECK_EQ(error.severity, Builders::Diagnostics::PathSpaceError::Severity::Recoverable);
        CHECK_NE(error.code, 0);
    }

    TEST_CASE("drawables removed between frames do not crash") {
        SimpleRenderFixture fixture{};
        PathRenderer2D renderer{fixture.fx.space};

        PathSurfaceSoftware surface{fixture.surface_desc};
        auto settings = default_render_settings(fixture.surface_desc);

        PathRenderer2D::RenderParams params{
            .target_path = SP::ConcretePathStringView{fixture.target_path.getPath()},
            .settings = settings,
            .surface = surface,
        };

        auto first = renderer.render(params);
        REQUIRE(first);

        // Republish snapshot with no drawables to simulate mid-frame removal.
        UIScene::DrawableBucketSnapshot empty_bucket{};
        fixture.fx.publish_snapshot(fixture.scene, std::move(empty_bucket));

        auto second = renderer.render(params);
        REQUIRE(second);
        CHECK_EQ(second->drawable_count, 0);
    }

#if defined(__APPLE__) && PATHSPACE_UI_METAL
    TEST_CASE("metal uploads toggle remains stable") {
        ScopedEnv enable_uploads{"PATHSPACE_ENABLE_METAL_UPLOADS", "1"};
        SimpleRenderFixture fixture{RendererKind::Metal2D};

        PathRenderer2D renderer{fixture.fx.space};
        PathSurfaceSoftware surface{fixture.surface_desc};
        auto settings = default_render_settings(fixture.surface_desc);
        settings.renderer.backend_kind = RendererKind::Metal2D;

        PathRenderer2D::RenderParams params{
            .target_path = SP::ConcretePathStringView{fixture.target_path.getPath()},
            .settings = settings,
            .surface = surface,
        };

        auto result = renderer.render(params);
        REQUIRE(result);
        CHECK(result->backend_kind == RendererKind::Software2D);
    }
#endif
}
