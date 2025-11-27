#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <pathspace/path/ConcretePath.hpp>

#include "DeclarativeTestUtils.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace SP;
namespace ThemeConfig = SP::UI::Declarative::ThemeConfig;

namespace {

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

} // namespace

TEST_CASE("Scene lifecycle exposes dirty event queues") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "scene_lifecycle_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "main_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "main_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto lifecycle_state_path = std::string(scene->path.getPath()) + "/runtime/lifecycle/state/running";
    auto running = space.read<bool, std::string>(lifecycle_state_path);
    REQUIRE(running);
    CHECK(*running);

    auto widget = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "watch_me",
                                                      {});
    REQUIRE(widget);

    auto dirty_queue = std::string(widget->getPath()) + "/render/events/dirty";
    auto initial_event = space.take<std::string>(dirty_queue);
    REQUIRE(initial_event);
    CHECK(*initial_event == widget->getPath());

    auto relabel = SP::UI::Declarative::Button::SetLabel(space, *widget, "updated label");
    REQUIRE(relabel);
    auto update_event = space.take<std::string>(dirty_queue);
    REQUIRE(update_event);
    CHECK(*update_event == widget->getPath());

    auto shutdown = SP::Scene::Shutdown(space, scene->path);
    REQUIRE(shutdown);
}

TEST_CASE("Scene lifecycle publishes scene snapshots and tracks metrics") {
    PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "scene_lifecycle_metrics");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "metrics_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "metrics_button",
                                                      {});
    REQUIRE(button);

    auto metrics_base = std::string(scene->path.getPath()) + "/runtime/lifecycle/metrics";
    auto buckets_path = metrics_base + "/widgets_with_buckets";
    auto last_revision_path = metrics_base + "/last_revision";

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.widget_timeout = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2500}, 3.0);
    readiness_options.revision_timeout = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0);
    readiness_options.min_revision = std::uint64_t{0}; // wait for the first non-zero revision
    readiness_options.scene_window_component_override = PathSpaceExamples::window_component_name(
        std::string(window->path.getPath()));
    readiness_options.scene_view_override = window->view_name;
    readiness_options.ensure_scene_window_mirror = true;
    readiness_options.wait_for_buckets = false;
    readiness_options.wait_for_structure = false;
    readiness_options.force_scene_publish = true;
    auto readiness = DeclarativeTestUtils::ensure_scene_ready(space,
                                                              scene->path,
                                                              window->path,
                                                              window->view_name,
                                                              readiness_options);
    if (!readiness) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("scene lifecycle readiness", readiness.error()));
    }
    REQUIRE(readiness);

    auto first_revision_ready = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        last_revision_path,
        std::uint64_t{1},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0));
    REQUIRE(first_revision_ready);

    REQUIRE(SP::UI::Declarative::Button::SetLabel(space, *button, "cycle"));
    SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
    publish_options.min_revision = std::uint64_t{1};
    auto force_publish = SP::UI::Declarative::SceneLifecycle::ForcePublish(space,
                                                                           scene->path,
                                                                           publish_options);
    REQUIRE(force_publish);
    CHECK_GE(*force_publish, std::uint64_t{2});
    auto second_revision_ready = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        last_revision_path,
        std::uint64_t{2},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0));
    REQUIRE(second_revision_ready);

    auto buckets = space.read<std::uint64_t, std::string>(buckets_path);
    REQUIRE(buckets);
    CHECK_EQ(*buckets, 1);

    REQUIRE(SP::UI::Declarative::Remove(space, *button));

    auto buckets_cleared = DeclarativeTestUtils::wait_for_metric_equal(
        space,
        buckets_path,
        std::uint64_t{0},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0));
    REQUIRE(buckets_cleared);

    REQUIRE(SP::Scene::Shutdown(space, scene->path));
}

TEST_CASE("Scene lifecycle manual pump synthesizes widget buckets") {
    PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "scene_lifecycle_manual_pump");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "manual_pump_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto label = SP::UI::Declarative::Label::Create(space,
                                                    window_view,
                                                    "manual_label",
                                                    std::string{"ready"});
    REQUIRE(label);

    SP::UI::Declarative::SceneLifecycle::ManualPumpOptions pump_options{};
    pump_options.wait_timeout = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0);
    auto pump_result = SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space,
                                                                          scene->path,
                                                                          pump_options);
    REQUIRE(pump_result);
    CHECK_GT(pump_result->widgets_processed, std::uint64_t{0});
    CHECK_GT(pump_result->buckets_ready, std::uint64_t{0});

    REQUIRE(SP::Scene::Shutdown(space, scene->path));
}

TEST_CASE("Scene lifecycle force publish reports missing worker") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "force_publish_missing_worker");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "missing_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);
    REQUIRE(SP::Scene::Shutdown(space, scene->path));

    auto force_publish = SP::UI::Declarative::SceneLifecycle::ForcePublish(space, scene->path, {});
    REQUIRE_FALSE(force_publish);
    CHECK_EQ(force_publish.error().code, SP::Error::Code::NotFound);
}

TEST_CASE("Focus and theme changes invalidate declarative widgets") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "scene_lifecycle_focus_theme");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "focus_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "focus_button",
                                                      {});
    REQUIRE(button);

    auto dirty_queue = std::string(button->getPath()) + "/render/events/dirty";
    (void)space.take<std::string>(dirty_queue, SP::Out{} & SP::Block{std::chrono::milliseconds{500}});

    auto focus_config = SP::UI::Builders::Widgets::Focus::MakeConfig(SP::App::AppRootPathView{app_root->getPath()});
    auto set_focus = SP::UI::Builders::Widgets::Focus::Set(space, focus_config, *button);
    REQUIRE(set_focus);

    auto focus_event = space.take<std::string>(dirty_queue, SP::Out{} & SP::Block{std::chrono::milliseconds{200}});
    REQUIRE(focus_event);
    CHECK_EQ(*focus_event, button->getPath());

    auto sunset_theme = SP::UI::Builders::Widgets::MakeSunsetWidgetTheme();
    auto ensured = ThemeConfig::Ensure(space,
                                                          SP::App::AppRootPathView{app_root->getPath()},
                                                          "sunset",
                                                          sunset_theme);
    REQUIRE(ensured);
    auto set_theme = ThemeConfig::SetActive(space,
                                                                SP::App::AppRootPathView{app_root->getPath()},
                                                                "sunset");
    REQUIRE(set_theme);

    auto theme_event = space.take<std::string>(dirty_queue, SP::Out{} & SP::Block{std::chrono::milliseconds{200}});
    REQUIRE(theme_event);
    CHECK_EQ(*theme_event, button->getPath());

    REQUIRE(SP::Scene::Shutdown(space, scene->path));
}
