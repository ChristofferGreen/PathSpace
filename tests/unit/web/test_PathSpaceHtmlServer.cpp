#include "third_party/doctest.h"

#include "pathspace/web/PathSpaceHtmlServer.hpp"

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace UIScene = SP::UI::Scene;

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

struct HtmlServerFixture {
    SP::ServeHtml::ServeHtmlSpace space{};
    SP::App::AppRootPath          app_root{"/system/applications/html_server"};

    auto publish_scene() -> SP::UI::Runtime::ScenePath {
        auto bucket = make_bucket();

        SP::UI::Runtime::SceneParams params{.name = "html_scene", .description = "HTML scene"};
        auto scene = SP::UI::Runtime::Scene::Create(space,
                                                    SP::App::AppRootPathView{app_root.getPath()},
                                                    params);
        REQUIRE(scene);

        UIScene::SceneSnapshotBuilder builder{space, SP::App::AppRootPathView{app_root.getPath()}, *scene};
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

    auto create_window() -> SP::UI::Runtime::WindowPath {
        SP::UI::Runtime::WindowParams params{.name = "html_window",
                                             .title = "HTML Window",
                                             .width = 640,
                                             .height = 480,
                                             .scale = 1.0f,
                                             .background = "#000"};
        auto window = SP::UI::Runtime::Window::Create(space,
                                                      SP::App::AppRootPathView{app_root.getPath()},
                                                      params);
        REQUIRE(window);
        return *window;
    }
};

} // namespace

TEST_SUITE("web.pathspace.htmlserver") {
TEST_CASE("PathSpaceHtmlServer rejects invalid options") {
    SP::ServeHtml::ServeHtmlSpace          space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.serve_html.port = -1;

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto started = server.start();
    CHECK_FALSE(started);
    CHECK(started.error().code == SP::Error::Code::MalformedInput);
}

TEST_CASE("PathSpaceHtmlServer forwards log hooks to launcher") {
    SP::ServeHtml::ServeHtmlSpace          space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};

    bool info_called = false;
    options.log_hooks = SP::ServeHtml::ServeHtmlLogHooks{
        .info = [&](std::string_view message) {
            info_called = true;
            CHECK(message == "hello");
        },
    };

    auto launcher = [](SP::ServeHtml::ServeHtmlSpace&,
                       SP::ServeHtml::ServeHtmlOptions const&,
                       std::atomic<bool>& stop_flag,
                       SP::ServeHtml::ServeHtmlLogHooks const& hooks,
                       std::function<void(SP::Expected<void>)> on_listen) {
        if (hooks.info) {
            hooks.info("hello");
        }
        stop_flag.store(true, std::memory_order_release);
        if (on_listen) {
            on_listen({});
        }
        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{space, options, launcher};

    auto started = server.start();
    REQUIRE(started);
    CHECK(info_called);
    server.stop();
}

TEST_CASE("PathSpaceHtmlServer surfaces listen failures") {
    SP::ServeHtml::ServeHtmlSpace          space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.serve_html.host = "256.256.256.256"; // invalid host triggers bind failure
    options.serve_html.port = 9099;

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto started = server.start();
    CHECK_FALSE(started);
    CHECK(started.error().code == SP::Error::Code::InvalidError);
}

TEST_CASE("PathSpaceHtmlServer assigns random port when zero") {
    SP::ServeHtml::ServeHtmlSpace          space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.serve_html.port = 0;

    auto captured_port = std::make_shared<int>(-1);

    auto launcher = [captured_port](SP::ServeHtml::ServeHtmlSpace&,
                                    SP::ServeHtml::ServeHtmlOptions const& opts,
                                    std::atomic<bool>& stop_flag,
                                    SP::ServeHtml::ServeHtmlLogHooks const&,
                                    std::function<void(SP::Expected<void>)> on_listen) {
        *captured_port = opts.port;
        stop_flag.store(true, std::memory_order_release);
        if (on_listen) {
            on_listen({});
        }
        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{space, options, launcher};

    auto started = server.start();
    REQUIRE(started);
    server.stop();

    CHECK(*captured_port > 0);
    CHECK(server.options().serve_html.port == *captured_port);
}

TEST_CASE("PathSpaceHtmlServer start/stop uses injected launcher") {
    using namespace std::chrono_literals;

    SP::ServeHtml::ServeHtmlSpace          space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.serve_html.port = 8080;

    auto stop_signal = std::make_shared<std::atomic<bool>>(false);
    auto run_count   = std::make_shared<std::atomic<int>>(0);

    auto launcher = [stop_signal, run_count](SP::ServeHtml::ServeHtmlSpace&,
                                             SP::ServeHtml::ServeHtmlOptions const&,
                                             std::atomic<bool>& stop_flag,
                                             SP::ServeHtml::ServeHtmlLogHooks const&,
                                             std::function<void(SP::Expected<void>)> on_listen) {
        run_count->fetch_add(1);

        while (!stop_flag.load(std::memory_order_acquire)
               && !stop_signal->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }

        if (on_listen) {
            on_listen({});
        }

        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{space, options, launcher};

    auto started = server.start();
    REQUIRE(started);
    CHECK(server.is_running());

    std::this_thread::sleep_for(5ms);
    CHECK(run_count->load(std::memory_order_acquire) == 1);

    server.stop();
    CHECK_FALSE(server.is_running());

    // Restart after a clean stop to ensure the stop flag resets.
    auto restarted = server.start();
    REQUIRE(restarted);
    stop_signal->store(true, std::memory_order_release);
    server.stop();
    CHECK_FALSE(server.is_running());
}

TEST_CASE("PathSpaceHtmlServer prefixes remote mount roots") {
    using namespace std::chrono_literals;

    SP::ServeHtml::ServeHtmlSpace              space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions  options{};
    SP::ServeHtml::RemoteMountSource           remote{};
    remote.alias = "alpha";
    options.remote_mount = remote;
    options.serve_html.port = 8080;

    auto inserted = space.insert("/inspector/metrics/remotes/alpha/client/connected", 1);
    REQUIRE(inserted.errors.empty());

    auto captured_options = std::make_shared<SP::ServeHtml::ServeHtmlOptions>();

    auto launcher = [captured_options](SP::ServeHtml::ServeHtmlSpace&,
                                       SP::ServeHtml::ServeHtmlOptions const& opts,
                                       std::atomic<bool>& stop_flag,
                                       SP::ServeHtml::ServeHtmlLogHooks const&,
                                       std::function<void(SP::Expected<void>)> on_listen) {
        *captured_options = opts;
        stop_flag.store(true, std::memory_order_release);
        if (on_listen) {
            on_listen({});
        }
        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{space, options, launcher};

    auto started = server.start();
    REQUIRE(started);

    std::this_thread::sleep_for(2ms);
    server.stop();

    CHECK(captured_options->apps_root == "/remote/alpha/system/applications");
    CHECK(captured_options->users_root == "/remote/alpha/system/auth/users");
    CHECK(captured_options->session_store_path == "/remote/alpha/system/web/sessions");
    CHECK(captured_options->google_users_root == "/remote/alpha/system/auth/oauth/google");
}

TEST_CASE("PathSpaceHtmlServer rejects unhealthy remote mount") {
    SP::ServeHtml::ServeHtmlSpace             space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.remote_mount = SP::ServeHtml::RemoteMountSource{.alias = "beta"};

    auto inserted = space.insert("/inspector/metrics/remotes/beta/client/connected", 0);
    REQUIRE(inserted.errors.empty());

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto started = server.start();
    CHECK_FALSE(started);
    CHECK(started.error().code == SP::Error::Code::InvalidError);
}

TEST_CASE("PathSpaceHtmlServer allows remote mount when health optional") {
    using namespace std::chrono_literals;

    SP::ServeHtml::ServeHtmlSpace              space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions  options{};
    SP::ServeHtml::RemoteMountSource           remote{};
    remote.alias          = "gamma";
    remote.require_healthy = false;
    options.remote_mount  = remote;

    auto captured_options = std::make_shared<SP::ServeHtml::ServeHtmlOptions>();

    auto launcher = [captured_options](SP::ServeHtml::ServeHtmlSpace&,
                                       SP::ServeHtml::ServeHtmlOptions const& opts,
                                       std::atomic<bool>& stop_flag,
                                       SP::ServeHtml::ServeHtmlLogHooks const&,
                                       std::function<void(SP::Expected<void>)> on_listen) {
        *captured_options = opts;
        stop_flag.store(true, std::memory_order_release);
        if (on_listen) {
            on_listen({});
        }
        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{space, options, launcher};

    auto started = server.start();
    REQUIRE(started);
    std::this_thread::sleep_for(2ms);
    server.stop();

    CHECK(captured_options->apps_root == "/remote/gamma/system/applications");
    CHECK(captured_options->users_root == "/remote/gamma/system/auth/users");
}

TEST_CASE("PathSpaceHtmlServer rejects mismatched remote roots") {
    SP::ServeHtml::ServeHtmlSpace             space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.remote_mount = SP::ServeHtml::RemoteMountSource{.alias = "delta"};
    options.serve_html.session_store_path = "/remote/other/system/web/sessions";

    auto inserted = space.insert("/inspector/metrics/remotes/delta/client/connected", 1);
    REQUIRE(inserted.errors.empty());

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto started = server.start();
    CHECK_FALSE(started);
    CHECK(started.error().code == SP::Error::Code::InvalidError);
}

TEST_CASE("PathSpaceHtmlServer forward helpers work locally") {
    SP::ServeHtml::ServeHtmlSpace             space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto inserted = server.forward_insert("/system/applications/demo/value", 42);
    REQUIRE(inserted);

    auto read_back = server.forward_read<int>("/system/applications/demo/value");
    REQUIRE(read_back);
    CHECK(*read_back == 42);

    auto children = server.forward_list_children("/system/applications");
    REQUIRE(children);
    CHECK(std::find(children->begin(), children->end(), std::string{"demo"}) != children->end());
}

TEST_CASE("PathSpaceHtmlServer forward helpers prefix remote mount") {
    SP::ServeHtml::ServeHtmlSpace             space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.remote_mount = SP::ServeHtml::RemoteMountSource{.alias = "alpha"};

    auto inserted_metric = space.insert("/inspector/metrics/remotes/alpha/client/connected", 1);
    REQUIRE(inserted_metric.errors.empty());

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto inserted = server.forward_insert("/system/applications/demo/config", std::string{"payload"});
    REQUIRE(inserted);

    auto read_back = server.forward_read<std::string>("/system/applications/demo/config");
    REQUIRE(read_back);
    CHECK(*read_back == "payload");

    auto direct = space.read<std::string>("/remote/alpha/system/applications/demo/config");
    REQUIRE(direct);
    CHECK(*direct == "payload");

    auto children = server.forward_list_children("/system");
    REQUIRE(children);
    CHECK(std::find(children->begin(), children->end(), std::string{"applications"}) != children->end());
}

TEST_CASE("PathSpaceHtmlServer forward helpers block unhealthy remote") {
    SP::ServeHtml::ServeHtmlSpace             space{};
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.remote_mount = SP::ServeHtml::RemoteMountSource{.alias = "beta"};

    auto inserted_metric = space.insert("/inspector/metrics/remotes/beta/client/connected", 0);
    REQUIRE(inserted_metric.errors.empty());

    SP::ServeHtml::PathSpaceHtmlServer server{space, options};

    auto inserted = server.forward_insert("/system/applications/demo/value", 7);
    CHECK_FALSE(inserted);
    CHECK(inserted.error().code == SP::Error::Code::InvalidError);
}

TEST_CASE("PathSpaceHtmlServer attaches default HTML mirror targets") {
    HtmlServerFixture                         fx;
    auto                                     scene  = fx.publish_scene();
    auto                                     window = fx.create_window();
    SP::ServeHtml::PathSpaceHtmlServerOptions options{};
    options.attach_default_targets = true;
    options.html_mirror            = SP::ServeHtml::HtmlMirrorBootstrap{
        .app_root      = fx.app_root,
        .window        = window,
        .scene         = scene,
        .mirror_config = SP::ServeHtml::HtmlMirrorConfig{
            .renderer_name = "html_helper_renderer",
            .target_name   = "web",
            .view_name     = "web",
        },
        .present_on_start = true,
    };
    options.serve_html.port = 0;

    auto captured_options = std::make_shared<SP::ServeHtml::ServeHtmlOptions>();

    auto launcher = [captured_options](SP::ServeHtml::ServeHtmlSpace&,
                                       SP::ServeHtml::ServeHtmlOptions const& opts,
                                       std::atomic<bool>& stop_flag,
                                       SP::ServeHtml::ServeHtmlLogHooks const&,
                                       std::function<void(SP::Expected<void>)> on_listen) {
        *captured_options = opts;
        stop_flag.store(true, std::memory_order_release);
        if (on_listen) {
            on_listen({});
        }
        return 0;
    };

    SP::ServeHtml::PathSpaceHtmlServer server{fx.space, options, launcher};

    auto started = server.start();
    REQUIRE(started);
    server.stop();

    CHECK(captured_options->renderer == "html_helper_renderer");
    REQUIRE(server.mirror_context());

    auto html_base = std::string{server.mirror_context()->target.getPath()} + "/output/v1/html";
    auto mode      = fx.space.read<std::string, std::string>(html_base + "/mode");
    REQUIRE(mode);
    CHECK_FALSE(mode->empty());
}
}
