#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <string>

using namespace SP;

TEST_CASE("Scene lifecycle exposes dirty event queues") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);

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
    SP::System::ShutdownDeclarativeRuntime(space);
}
