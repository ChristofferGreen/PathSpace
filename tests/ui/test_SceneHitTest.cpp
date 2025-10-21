#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace SP;
namespace UI = SP::UI;
namespace BuildersNS = SP::UI::Builders;
namespace UIScene = SP::UI::Scene;
namespace BuildersScene = SP::UI::Builders::Scene;
using namespace std::chrono_literals;
using namespace SP::UI::PipelineFlags;

namespace {

struct HitTestFixture {
    PathSpace            space;
    SP::App::AppRootPath app_root{ "/system/applications/test_hit" };

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{ app_root.getPath() };
    }

    auto publish_snapshot(BuildersNS::ScenePath const& scenePath,
                          UIScene::DrawableBucketSnapshot bucket) -> std::uint64_t {
        UIScene::SceneSnapshotBuilder builder{ space, root_view(), scenePath };
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

auto create_scene(HitTestFixture& fx,
                  std::string const& name,
                  UIScene::DrawableBucketSnapshot bucket) -> BuildersNS::ScenePath {
    BuildersNS::SceneParams params{
        .name = name,
        .description = "Hit test scene",
    };
    auto scene = BuildersScene::Create(fx.space, fx.root_view(), params);
    REQUIRE(scene);
    fx.publish_snapshot(*scene, std::move(bucket));
    return *scene;
}

auto create_renderer(HitTestFixture& fx, std::string const& name) -> BuildersNS::RendererPath {
    BuildersNS::RendererParams params{
        .name = name,
        .kind = BuildersNS::RendererKind::Software2D,
        .description = "Hit test renderer",
    };
    auto renderer = BuildersNS::Renderer::Create(fx.space, fx.root_view(), params);
    REQUIRE(renderer);
    return *renderer;
}

auto create_surface(HitTestFixture& fx,
                    std::string const& name,
                    BuildersNS::SurfaceDesc desc,
                    std::string const& rendererName) -> BuildersNS::SurfacePath {
    BuildersNS::SurfaceParams params{};
    params.name = name;
    params.desc = desc;
    params.renderer = rendererName;
    auto surface = BuildersNS::Surface::Create(fx.space, fx.root_view(), params);
    REQUIRE(surface);
    return *surface;
}

auto resolve_target(HitTestFixture& fx,
                    BuildersNS::SurfacePath const& surfacePath) -> SP::ConcretePathString {
    auto targetRel = fx.space.read<std::string, std::string>(std::string(surfacePath.getPath()) + "/target");
    REQUIRE(targetRel);
    auto targetAbs = SP::App::resolve_app_relative(fx.root_view(), *targetRel);
    REQUIRE(targetAbs);
    return SP::ConcretePathString{ targetAbs->getPath() };
}

UIScene::DrawableBucketSnapshot make_basic_bucket() {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = { 0x10u, 0x20u };
    bucket.world_transforms = { UIScene::Transform{}, UIScene::Transform{} };
    bucket.bounds_spheres = { UIScene::BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f},
                              UIScene::BoundingSphere{{1.0f, 1.0f, 0.0f}, 2.0f} };
    bucket.bounds_boxes = {
        UIScene::BoundingBox{{0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 0.0f}},
        UIScene::BoundingBox{{0.5f, 0.5f, 0.0f}, {1.5f, 1.5f, 0.0f}},
    };
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 1.0f};
    bucket.material_ids = {1, 1};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {0, 0};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    bucket.clip_head_indices = {-1, -1};
    bucket.authoring_map = {
        UIScene::DrawableAuthoringMapEntry{ bucket.drawable_ids[0], "nodes/root/background", 0, 0 },
        UIScene::DrawableAuthoringMapEntry{ bucket.drawable_ids[1], "nodes/root/card/button", 0, 0 },
    };
    return bucket;
}

UIScene::DrawableBucketSnapshot make_clipped_bucket() {
    auto bucket = make_basic_bucket();
    bucket.clip_nodes = {
        UIScene::ClipNode{
            .type = UIScene::ClipNodeType::Rect,
            .next = -1,
            .rect = UIScene::ClipRect{ .min_x = 0.5f, .min_y = 0.5f, .max_x = 1.0f, .max_y = 1.0f },
        }
    };
    bucket.clip_head_indices = {-1, 0};
    return bucket;
}

} // namespace

TEST_SUITE("Scene Hit Tests") {

TEST_CASE("returns topmost drawable using render order") {
    HitTestFixture fx;
    auto bucket = make_basic_bucket();
    auto scenePath = create_scene(fx, "hit_order", bucket);

    BuildersScene::HitTestRequest request{};
    request.x = 1.0f;
    request.y = 1.0f;

    auto result = BuildersScene::HitTest(fx.space, scenePath, request);
    REQUIRE(result);
    CHECK(result->hit);
    CHECK(result->target.drawable_id == bucket.drawable_ids[1]);
    REQUIRE_FALSE(result->focus_chain.empty());
    CHECK(result->focus_chain.front() == std::string("nodes/root/card/button"));
    CHECK(result->position.has_local);
    CHECK(result->position.scene_x == doctest::Approx(request.x));
    CHECK(result->position.scene_y == doctest::Approx(request.y));
    CHECK(result->position.local_x == doctest::Approx(request.x - bucket.bounds_boxes[1].min[0]));
    CHECK(result->position.local_y == doctest::Approx(request.y - bucket.bounds_boxes[1].min[1]));
    REQUIRE_FALSE(result->focus_path.empty());
    CHECK(result->focus_path.front().focusable);
}

TEST_CASE("respects clip rectangles when evaluating hits") {
    HitTestFixture fx;
    auto bucket = make_clipped_bucket();
    auto scenePath = create_scene(fx, "hit_clip", bucket);

    BuildersScene::HitTestRequest inside{};
    inside.x = 0.75f;
   inside.y = 0.75f;

    auto insideResult = BuildersScene::HitTest(fx.space, scenePath, inside);
    REQUIRE(insideResult);
    CHECK(insideResult->hit);
    CHECK(insideResult->target.drawable_id == bucket.drawable_ids[1]);

    BuildersScene::HitTestRequest outside{};
    outside.x = 1.2f;
    outside.y = 1.2f;

    auto outsideResult = BuildersScene::HitTest(fx.space, scenePath, outside);
    REQUIRE(outsideResult);
    CHECK(outsideResult->hit);
    CHECK(outsideResult->target.drawable_id == bucket.drawable_ids[0]);
}

TEST_CASE("focus chain enumerates authoring ancestors") {
    HitTestFixture fx;
    auto bucket = make_basic_bucket();
    auto scenePath = create_scene(fx, "hit_focus", bucket);

    BuildersScene::HitTestRequest request{};
    request.x = 1.0f;
    request.y = 1.0f;

    auto result = BuildersScene::HitTest(fx.space, scenePath, request);
    REQUIRE(result);
    REQUIRE(result->hit);
    std::vector<std::string> expected{
        "nodes/root/card/button",
        "nodes/root/card",
        "nodes/root",
        "nodes",
    };
    CHECK(result->focus_chain == expected);
    REQUIRE(result->focus_path.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(result->focus_path[i].path == expected[i]);
        if (i == 0) {
            CHECK(result->focus_path[i].focusable);
        } else {
            CHECK_FALSE(result->focus_path[i].focusable);
        }
    }
}

TEST_CASE("pipeline flags influence default draw order for hit testing") {
    HitTestFixture fx;
    auto bucket = make_basic_bucket();
    bucket.opaque_indices.clear();
    bucket.alpha_indices.clear();
    bucket.pipeline_flags = {0u, AlphaBlend};

    auto scenePath = create_scene(fx, "hit_pipeline_flags", bucket);

    BuildersScene::HitTestRequest request{};
    request.x = 1.0f;
    request.y = 1.0f;

    auto result = BuildersScene::HitTest(fx.space, scenePath, request);
    REQUIRE(result);
    CHECK(result->hit);
    CHECK(result->target.drawable_id == bucket.drawable_ids[1]);
}

TEST_CASE("hit test can schedule auto-render events") {
    HitTestFixture fx;

    auto bucket = make_basic_bucket();
    auto scenePath = create_scene(fx, "hit_schedule", bucket);

    auto rendererPath = create_renderer(fx, "renderer_schedule");

    BuildersNS::SurfaceDesc desc{};
    desc.size_px.width = 2;
    desc.size_px.height = 2;
    desc.pixel_format = BuildersNS::PixelFormat::RGBA8Unorm;
    desc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_schedule", desc, rendererPath.getPath());
    REQUIRE(BuildersNS::Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);

    BuildersScene::HitTestRequest request{};
    request.x = 1.0f;
    request.y = 1.0f;
    request.schedule_render = true;
    request.auto_render_target = SP::ConcretePath{ targetPath.getPath() };

    auto result = BuildersScene::HitTest(fx.space, scenePath, request);
    REQUIRE(result);
    CHECK(result->hit);

    auto queuePath = std::string(targetPath.getPath()) + "/events/renderRequested/queue";
    auto event = fx.space.take<BuildersNS::AutoRenderRequestEvent>(queuePath,
                                                                   SP::Out{} & SP::Block{ std::chrono::milliseconds{20} });
    REQUIRE(event);
    CHECK(event->reason == "hit-test");
    CHECK(event->sequence > 0);
    CHECK(event->frame_index == 0);
}

TEST_CASE("hit test auto-render wait-notify latency stays within budget") {
    using namespace std::chrono_literals;

    HitTestFixture fx;
    auto bucket = make_basic_bucket();
    auto scenePath = create_scene(fx, "hit_schedule_latency", bucket);

    auto rendererPath = create_renderer(fx, "renderer_schedule_latency");

    BuildersNS::SurfaceDesc desc{};
    desc.size_px.width = 2;
    desc.size_px.height = 2;
    desc.pixel_format = BuildersNS::PixelFormat::RGBA8Unorm;
    desc.premultiplied_alpha = true;

    auto surfacePath = create_surface(fx, "surface_schedule_latency", desc, rendererPath.getPath());
    REQUIRE(BuildersNS::Surface::SetScene(fx.space, surfacePath, scenePath));
    auto targetPath = resolve_target(fx, surfacePath);
    auto queuePath = std::string(targetPath.getPath()) + "/events/renderRequested/queue";

    BuildersScene::HitTestRequest request{};
    request.x = 1.0f;
    request.y = 1.0f;
    request.schedule_render = true;
    request.auto_render_target = SP::ConcretePath{targetPath.getPath()};

    std::atomic<bool> waiterReady{false};
    BuildersNS::AutoRenderRequestEvent observed{};
    bool observedSuccess = false;
    std::chrono::milliseconds observedLatency{0};

    std::thread waiter([&]() {
        waiterReady.store(true, std::memory_order_release);
        auto start = std::chrono::steady_clock::now();
        auto taken = fx.space.take<BuildersNS::AutoRenderRequestEvent>(queuePath,
                                                                       SP::Out{} & SP::Block{500ms});
        auto end = std::chrono::steady_clock::now();
        observedLatency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (taken) {
            observed = *taken;
            observedSuccess = true;
        }
    });

    while (!waiterReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(20ms);

    auto result = BuildersScene::HitTest(fx.space, scenePath, request);
    REQUIRE(result);
    CHECK(result->hit);

    waiter.join();

    REQUIRE(observedSuccess);
    CHECK(observed.reason == "hit-test");
    CHECK(observed.frame_index == 0);
    CHECK(observed.sequence > 0);
    CHECK(observedLatency >= 20ms);
    CHECK(observedLatency < 200ms);
}

} // TEST_SUITE
