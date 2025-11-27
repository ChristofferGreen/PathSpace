#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/BuildersShared.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Builders;
namespace UIScene = SP::UI::Scene;

namespace {

auto identity_transform() -> UIScene::Transform {
    UIScene::Transform t{};
    for (std::size_t i = 0; i < t.elements.size(); ++i) {
        t.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return t;
}

UIScene::DrawableBucketSnapshot make_bucket() {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {1, 2};
    bucket.world_transforms = {identity_transform(), identity_transform()};

    UIScene::BoundingSphere sphereA{};
    sphereA.center = {24.0f, 18.0f, 0.0f};
    sphereA.radius = 30.0f;
    UIScene::BoundingSphere sphereB{};
    sphereB.center = {60.0f, 32.0f, 0.0f};
    sphereB.radius = 20.0f;
    bucket.bounds_spheres = {sphereA, sphereB};

    UIScene::BoundingBox boxA{};
    boxA.min = {12.0f, 9.0f, 0.0f};
    boxA.max = {36.0f, 27.0f, 0.0f};
    UIScene::BoundingBox boxB{};
    boxB.min = {50.0f, 20.0f, 0.0f};
    boxB.max = {74.0f, 44.0f, 0.0f};
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
    bucket.drawable_fingerprints = {0x1001u, 0x2020u};

    UIScene::RectCommand rect{};
    rect.min_x = 12.0f;
    rect.min_y = 9.0f;
    rect.max_x = 36.0f;
    rect.max_y = 27.0f;
    rect.color = {0.25f, 0.5f, 0.75f, 1.0f};
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(rect));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

    UIScene::RoundedRectCommand rounded{};
    rounded.min_x = 50.0f;
    rounded.min_y = 20.0f;
    rounded.max_x = 74.0f;
    rounded.max_y = 44.0f;
    rounded.radius_top_left = 2.0f;
    rounded.radius_top_right = 3.5f;
    rounded.radius_bottom_right = 1.5f;
    rounded.radius_bottom_left = 4.0f;
    rounded.color = {0.9f, 0.3f, 0.2f, 0.6f};
    offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rounded, sizeof(rounded));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::RoundedRect));

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    return bucket;
}

struct HtmlFixture {
    PathSpace space;
    AppRootPath app_root{"/system/applications/html_present"};

    auto root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto publish_scene(UIScene::DrawableBucketSnapshot bucket) -> ScenePath {
        SceneParams params{.name = "html_scene", .description = "HTML scene"};
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
        return *scene;
    }
};

} // namespace

TEST_CASE("Window::Present returns HTML payload") {
    HtmlFixture fx;

    auto bucket = make_bucket();
    auto scenePath = fx.publish_scene(bucket);

    RendererParams rendererParams{.name = "html_renderer", .kind = RendererKind::Software2D, .description = "Renderer"};
    auto rendererPath = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(rendererPath);

    HtmlTargetParams htmlParams{};
    htmlParams.name = "main";
    htmlParams.scene = "scenes/html_scene";
    htmlParams.desc.max_dom_nodes = 4;
    htmlParams.desc.prefer_dom = false; // force canvas fallback for variety
    auto htmlTarget = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *rendererPath, htmlParams);
    REQUIRE(htmlTarget);

    WindowParams windowParams{.name = "main_window", .title = "HTML View", .width = 640, .height = 480, .scale = 1.0f, .background = "#000"};
    auto windowPath = Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);

    REQUIRE(Window::AttachHtmlTarget(fx.space, *windowPath, "view", *htmlTarget));

    auto present = Window::Present(fx.space, *windowPath, "view");
    REQUIRE(present);
    CHECK(present->framebuffer.empty());
    REQUIRE(present->html.has_value());
    auto const& payload = *present->html;
    CHECK(payload.revision == 1);
    if (payload.mode == "dom") {
        CHECK_FALSE(payload.dom.empty());
    } else {
        CHECK(payload.mode == "canvas");
        CHECK(payload.dom.empty());
    }
    CHECK_FALSE(payload.commands.empty());
    CHECK(payload.used_canvas_fallback);

    // Canvas fallback may not emit additional assets for rect-only scenes.
    CHECK(payload.assets.empty());

    auto htmlBase = std::string(htmlTarget->getPath()) + "/output/v1/html";
    auto usedCanvas = fx.space.read<bool>(htmlBase + "/usedCanvasFallback");
    REQUIRE(usedCanvas);
    CHECK(*usedCanvas == true);

    auto error = Diagnostics::ReadTargetError(fx.space, ConcretePathView{htmlTarget->getPath()});
    REQUIRE(error);
    bool has_error_message = false;
    if (error->has_value()) {
        INFO("html target error message: " << (*error)->message);
        INFO("html target error detail: " << (*error)->detail);
        has_error_message = !(*error)->message.empty();
    }
    CHECK_FALSE(has_error_message);
}

TEST_CASE("Window::Present writes HTML present metrics and residency") {
    HtmlFixture fx;

    auto bucket = make_bucket();
    auto scenePath = fx.publish_scene(bucket);

    RendererParams rendererParams{.name = "html_renderer_metrics", .kind = RendererKind::Software2D, .description = "Renderer"};
    auto rendererPath = Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(rendererPath);

    HtmlTargetParams htmlParams{};
    htmlParams.name = "metrics";
    htmlParams.scene = "scenes/html_scene";
    htmlParams.desc.max_dom_nodes = 16;
    htmlParams.desc.prefer_dom = true;
    auto htmlTarget = Renderer::CreateHtmlTarget(fx.space, fx.root_view(), *rendererPath, htmlParams);
    REQUIRE(htmlTarget);

    WindowParams windowParams{.name = "metrics_window", .title = "HTML Metrics", .width = 800, .height = 600, .scale = 1.0f, .background = "#111"};
    auto windowPath = Window::Create(fx.space, fx.root_view(), windowParams);
    REQUIRE(windowPath);

    REQUIRE(Window::AttachHtmlTarget(fx.space, *windowPath, "view", *htmlTarget));

    auto present = Window::Present(fx.space, *windowPath, "view");
    REQUIRE(present);
    CHECK(present->framebuffer.empty());
    CHECK_FALSE(present->html->dom.empty());

    auto const& stats = present->stats;
    CHECK(stats.presented);
    CHECK_FALSE(stats.skipped);
    CHECK(stats.backend_kind == "Html");
    CHECK(stats.mode == PathWindowPresentMode::AlwaysLatestComplete);
    CHECK_FALSE(stats.auto_render_on_present);
    CHECK_FALSE(stats.vsync_aligned);
    CHECK(stats.frame.frame_index == 1);
    CHECK(stats.frame.revision == 1);
    CHECK(stats.frame.render_ms >= 0.0);

    auto commonBase = std::string(htmlTarget->getPath()) + "/output/v1/common";
    auto backendKind = fx.space.read<std::string, std::string>(commonBase + "/backendKind");
    REQUIRE(backendKind);
    CHECK(*backendKind == "Html");
    auto presentMode = fx.space.read<std::string, std::string>(commonBase + "/presentMode");
    REQUIRE(presentMode);
    CHECK(*presentMode == "AlwaysLatestComplete");
    auto presentedValue = fx.space.read<bool>(commonBase + "/presented");
    REQUIRE(presentedValue);
    CHECK(*presentedValue);
    auto vsyncAlign = fx.space.read<bool>(commonBase + "/vsyncAlign");
    REQUIRE(vsyncAlign);
    CHECK_FALSE(*vsyncAlign);
    auto autoRender = fx.space.read<bool>(commonBase + "/autoRenderOnPresent");
    REQUIRE(autoRender);
    CHECK_FALSE(*autoRender);
    auto frameIndex = fx.space.read<uint64_t>(commonBase + "/frameIndex");
    REQUIRE(frameIndex);
    CHECK(*frameIndex == 1);
    auto revision = fx.space.read<uint64_t>(commonBase + "/revision");
    REQUIRE(revision);
    CHECK(*revision == 1);

    auto residencyBase = std::string(htmlTarget->getPath()) + "/diagnostics/metrics/residency";
    auto cpuBytes = fx.space.read<uint64_t>(residencyBase + "/cpuBytes");
    REQUIRE(cpuBytes);
    CHECK(*cpuBytes == 0);
    auto gpuBytes = fx.space.read<uint64_t>(residencyBase + "/gpuBytes");
    REQUIRE(gpuBytes);
    CHECK(*gpuBytes == 0);

    const std::string windowMetricsBase = std::string(windowPath->getPath()) +
                                          "/diagnostics/metrics/live/views/view/present";
    auto centralFrameIndex = fx.space.read<uint64_t>(windowMetricsBase + "/frameIndex");
    REQUIRE(centralFrameIndex);
    CHECK(*centralFrameIndex == 1);
    auto centralBackend = fx.space.read<std::string, std::string>(windowMetricsBase + "/backendKind");
    REQUIRE(centralBackend);
    CHECK(*centralBackend == "Html");
    auto centralTimestamp = fx.space.read<std::uint64_t>(windowMetricsBase + "/timestampNs");
    REQUIRE(centralTimestamp);
    CHECK(*centralTimestamp > 0);
    auto viewName = fx.space.read<std::string, std::string>(windowMetricsBase + "/viewName");
    REQUIRE(viewName);
    CHECK(*viewName == "view");
}
