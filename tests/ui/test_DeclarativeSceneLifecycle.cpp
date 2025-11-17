#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <pathspace/path/ConcretePath.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace SP;

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

    auto lifecycle_state_path = std::string(scene->path.getPath()) + "/runtime/lifecycle/state/running";
    auto running = space.read<bool, std::string>(lifecycle_state_path);
    REQUIRE(running);
    CHECK(*running);

    auto widget = SP::UI::Declarative::Button::Create(space,
                                                      SP::App::ConcretePathView{window->path.getPath()},
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

    auto button = SP::UI::Declarative::Button::Create(space,
                                                      SP::App::ConcretePathView{window->path.getPath()},
                                                      "metrics_button",
                                                      {});
    REQUIRE(button);

    auto metrics_base = std::string(scene->path.getPath()) + "/runtime/lifecycle/metrics";
    auto buckets_path = metrics_base + "/widgets_with_buckets";
    auto builds_root = std::string(scene->path.getPath()) + "/builds";
    SP::ConcretePathStringView builds_view{builds_root};

    auto wait_until = [&](auto&& predicate) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return false;
    };

    auto wait_for_revision = [&](std::size_t target) {
        return wait_until([&]() {
            auto builds = space.listChildren(builds_view);
            return builds.size() >= target;
        });
    };

    auto first_revision_ready = wait_for_revision(1);
    REQUIRE(first_revision_ready);

    REQUIRE(SP::UI::Declarative::Button::SetLabel(space, *button, "cycle"));
    auto second_revision_ready = wait_for_revision(2);
    REQUIRE(second_revision_ready);

    auto buckets = space.read<std::uint64_t, std::string>(buckets_path);
    REQUIRE(buckets);
    CHECK_EQ(*buckets, 1);

    REQUIRE(SP::UI::Declarative::Remove(space, *button));

    auto buckets_cleared = wait_until([&]() {
        auto remaining = space.read<std::uint64_t, std::string>(buckets_path);
        return remaining && *remaining == 0;
    });
    CHECK(buckets_cleared);

    REQUIRE(SP::Scene::Shutdown(space, scene->path));
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

    auto button = SP::UI::Declarative::Button::Create(space,
                                                      SP::App::ConcretePathView{window->path.getPath()},
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
    auto ensured = SP::UI::Builders::Config::Theme::Ensure(space,
                                                          SP::App::AppRootPathView{app_root->getPath()},
                                                          "sunset",
                                                          sunset_theme);
    REQUIRE(ensured);
    auto set_theme = SP::UI::Builders::Config::Theme::SetActive(space,
                                                                SP::App::AppRootPathView{app_root->getPath()},
                                                                "sunset");
    REQUIRE(set_theme);

    auto theme_event = space.take<std::string>(dirty_queue, SP::Out{} & SP::Block{std::chrono::milliseconds{200}});
    REQUIRE(theme_event);
    CHECK_EQ(*theme_event, button->getPath());

    REQUIRE(SP::Scene::Shutdown(space, scene->path));
}
