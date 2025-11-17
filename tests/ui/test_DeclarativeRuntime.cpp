#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace SP;

TEST_CASE("Declarative runtime wires app, window, and scene") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    CHECK_FALSE(launch->default_theme_path.empty());

    auto app_root = SP::App::Create(space, "hello_widgets");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.title = "Hello Widgets";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "main";
    scene_options.description = "Declarative main scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto relative_scene = std::string(scene->path.getPath()).substr(app_root->getPath().size() + 1);

    auto view_path = std::string(window->path.getPath()) + "/views/" + window->view_name + "/scene";
    auto stored_scene = space.read<std::string, std::string>(view_path);
    REQUIRE(stored_scene);
    CHECK(stored_scene.value() == relative_scene);

    auto structure_view = std::string(scene->path.getPath())
                          + "/structure/window/"
                          + "main_window/view";
    auto stored_view = space.read<std::string, std::string>(structure_view);
    REQUIRE(stored_view);
    CHECK(stored_view.value() == window->view_name);

    auto attached = space.read<bool, std::string>(std::string(scene->path.getPath()) + "/state/attached");
    REQUIRE(attached);
    CHECK(attached.value());

    auto view_renderer_path = std::string(window->path.getPath()) + "/views/" + window->view_name + "/renderer";
    auto renderer_relative = space.read<std::string, std::string>(view_renderer_path);
    REQUIRE(renderer_relative);
    CHECK_FALSE(renderer_relative->empty());

    auto structure_renderer_path = std::string(scene->path.getPath())
                                   + "/structure/window/main_window/renderer";
    auto stored_renderer = space.read<std::string, std::string>(structure_renderer_path);
    REQUIRE(stored_renderer);
    CHECK(*stored_renderer == *renderer_relative);

    auto structure_surface_path = std::string(scene->path.getPath())
                                  + "/structure/window/main_window/surface";
    auto stored_surface = space.read<std::string, std::string>(structure_surface_path);
    REQUIRE(stored_surface);
    CHECK_FALSE(stored_surface->empty());

    auto shutdown_scene = SP::Scene::Shutdown(space, scene->path);
    REQUIRE(shutdown_scene);
}

TEST_CASE("Declarative input task drains widget ops") {
    using namespace std::chrono_literals;
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.input_task_options.poll_interval = std::chrono::milliseconds{1};
    launch_options.start_io_pump = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);

    auto app_root = SP::App::Create(space, "inputwidgets");
    REQUIRE(app_root);

    std::string widget_root = std::string(app_root->getPath()) + "/widgets/test_widget";
    auto queue_path = widget_root + "/ops/inbox/queue";

    SP::UI::Builders::Widgets::Bindings::WidgetOp op{};
    op.kind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate;
    op.widget_path = widget_root;
    op.value = 1.0f;
    (void)space.insert(queue_path, op);

    auto actions_path = widget_root + "/ops/actions/inbox/queue";
    auto action = space.take<SP::UI::Builders::Widgets::Reducers::WidgetAction>(
        actions_path,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(action);
    CHECK(action->kind == op.kind);

    SP::System::ShutdownDeclarativeRuntime(space);
}

TEST_CASE("Declarative input task invokes registered handlers") {
    using namespace std::chrono_literals;
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_io_pump = false;
    launch_options.input_task_options.poll_interval = std::chrono::milliseconds{1};
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);

    auto app_root = SP::App::Create(space, "handlerapp");
    REQUIRE(app_root);

    SP::UI::Declarative::Button::Args args{};
    args.label = "Invoke";
    auto handler_flag = std::make_shared<std::atomic<bool>>(false);
    args.on_press = [handler_flag](SP::UI::Declarative::ButtonContext&) {
        handler_flag->store(true, std::memory_order_release);
    };

    SP::UI::Declarative::MountOptions mount_options;
    mount_options.policy = SP::UI::Declarative::MountPolicy::WindowWidgets;
    auto button = SP::UI::Declarative::Button::Create(space,
                                                      SP::App::ConcretePathView{app_root->getPath()},
                                                      "handler_button",
                                                      std::move(args),
                                                      mount_options);
    REQUIRE(button);

    auto queue_path = std::string(button->getPath()) + "/ops/inbox/queue";
    SP::UI::Builders::Widgets::Bindings::WidgetOp op{};
    op.kind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate;
    op.widget_path = button->getPath();
    op.value = 1.0f;
    (void)space.insert(queue_path, op);

    bool observed = false;
    for (int attempts = 0; attempts < 50; ++attempts) {
        if (handler_flag->load(std::memory_order_acquire)) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(observed);

    SP::System::ShutdownDeclarativeRuntime(space);
}
