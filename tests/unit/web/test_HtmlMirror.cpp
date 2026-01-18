#include "third_party/doctest.h"

#include "pathspace/web/HtmlMirror.hpp"

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Runtime;
namespace UIScene = SP::UI::Scene;

namespace {

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

auto make_bucket() -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {1};
    bucket.world_transforms = {identity_transform()};

    UIScene::BoundingSphere sphere{};
    sphere.center = {24.0f, 18.0f, 0.0f};
    sphere.radius = 30.0f;
    bucket.bounds_spheres = {sphere};

    UIScene::BoundingBox box{};
    box.min = {12.0f, 9.0f, 0.0f};
    box.max = {36.0f, 27.0f, 0.0f};
    bucket.bounds_boxes = {box};
    bucket.bounds_box_valid = {1};

    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.clip_head_indices = {-1};
    bucket.drawable_fingerprints = {0x1001u};

    UIScene::RectCommand rect{};
    rect.min_x = 12.0f;
    rect.min_y = 9.0f;
    rect.max_x = 36.0f;
    rect.max_y = 27.0f;
    rect.color = {0.25f, 0.5f, 0.75f, 1.0f};

    bucket.command_payload.resize(sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data(), &rect, sizeof(rect));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {};
    return bucket;
}

struct HtmlMirrorFixture {
    PathSpace   space{};
    AppRootPath app_root{"/system/applications/html_mirror"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_scene() -> ScenePath {
        auto bucket = make_bucket();

        SceneParams params{.name = "html_scene", .description = "HTML scene"};
        auto scene = Runtime::Scene::Create(space, root_view(), params);
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
        return *scene;
    }

    auto create_window() -> WindowPath {
        WindowParams window_params{.name = "main_window",
                                   .title = "HTML Mirror",
                                   .width = 640,
                                   .height = 480,
                                   .scale = 1.0f,
                                   .background = "#000"};
        auto window = Window::Create(space, root_view(), window_params);
        REQUIRE(window);
        return *window;
    }
};

} // namespace

TEST_SUITE("web.html.mirror") {
TEST_CASE("CreateHtmlMirrorTargets wires renderer and target") {
    HtmlMirrorFixture fx;

    auto scene  = fx.publish_scene();
    auto window = fx.create_window();

    SP::ServeHtml::HtmlMirrorConfig mirror_config{
        .renderer_name = "html_helper_renderer",
        .target_name   = "web",
        .view_name     = "web",
    };

    auto mirror = SP::ServeHtml::CreateHtmlMirrorTargets(fx.space,
                                                         fx.app_root,
                                                         window,
                                                         scene,
                                                         mirror_config);
    REQUIRE(mirror);
    CHECK(mirror->renderer.getPath().find(mirror_config.renderer_name) != std::string::npos);
    CHECK(mirror->target.getPath().find(mirror_config.target_name) != std::string::npos);

    auto present = SP::ServeHtml::PresentHtmlMirror(fx.space, *mirror);
    REQUIRE(present);

    auto html_base = std::string(mirror->target.getPath()) + "/output/v1/html";
    auto mode = fx.space.read<std::string, std::string>(html_base + "/mode");
    REQUIRE(mode);
    CHECK_FALSE(mode->empty());
}
}
