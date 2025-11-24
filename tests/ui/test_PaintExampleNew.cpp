#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/examples/paint/PaintExampleNewUI.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};
}

TEST_CASE("paint_example_new button reacts to pointer device events") {
    using namespace std::chrono_literals;
    SP::PathSpace space;

    SP::System::LaunchOptions launch_opts{};
    auto launch = SP::System::LaunchStandard(space, launch_opts);
    REQUIRE(launch);
    RuntimeGuard guard{space};

    auto app = SP::App::Create(space, "paint_example_new_device_test");
    REQUIRE(app);

    SP::Window::CreateOptions window_opts;
    window_opts.title = "Paint Example Device Test";
    window_opts.width = 640;
    window_opts.height = 480;
    auto window = SP::Window::Create(space, *app, window_opts);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app, window->path);
    REQUIRE(scene);

    auto view_base = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto surface_rel = space.read<std::string, std::string>(view_base + "/surface");
    REQUIRE(surface_rel);
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()}, *surface_rel);
    REQUIRE(surface_abs);
    auto surface_path = SP::UI::Builders::SurfacePath{surface_abs->getPath()};
    auto set_scene = SP::UI::Builders::Surface::SetScene(space, surface_path, scene->path);
    REQUIRE(set_scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto pressed = std::make_shared<std::atomic<bool>>(false);
    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Press Me";
    button_args.style.width = 240.0f;
    button_args.style.height = 64.0f;
    button_args.on_press = [pressed](SP::UI::Declarative::ButtonContext&) {
        pressed->store(true, std::memory_order_release);
    };

    auto mounted = PathSpaceExamples::PaintExampleNew::MountButtonUI(space,
                                                                     window_view,
                                                                     window_opts.width,
                                                                     window_opts.height,
                                                                     std::move(button_args));
    REQUIRE(mounted);

    auto pointer_subscriber_path =
        std::string("/system/devices/in/pointer/default/config/push/subscribers/paint_example_new_device_test");
    auto keyboard_subscriber_path =
        std::string("/system/devices/in/text/default/config/push/subscribers/paint_example_new_device_test");

    auto pointer_missing = space.read<bool, std::string>(pointer_subscriber_path);
    CHECK_FALSE(pointer_missing);
    auto keyboard_missing = space.read<bool, std::string>(keyboard_subscriber_path);
    CHECK_FALSE(keyboard_missing);

    auto input_ready = PathSpaceExamples::PaintExampleNew::EnableWindowInput(space,
                                                                             *window,
                                                                             "paint_example_new_device_test");
    REQUIRE(input_ready);

    auto pointer_subscriber = space.read<bool, std::string>(pointer_subscriber_path);
    REQUIRE(pointer_subscriber);
    CHECK(*pointer_subscriber);

    auto keyboard_subscriber = space.read<bool, std::string>(keyboard_subscriber_path);
    REQUIRE(keyboard_subscriber);
    CHECK(*keyboard_subscriber);
}
