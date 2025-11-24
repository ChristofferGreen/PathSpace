#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace SP;

namespace {

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

} // namespace

TEST_CASE("Declarative runtime wires app, window, and scene") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    RuntimeGuard runtime_guard{space};
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
    launch_options.start_io_telemetry_control = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    RuntimeGuard runtime_guard{space};

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

}

TEST_CASE("Declarative input task invokes registered handlers") {
    using namespace std::chrono_literals;
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_io_pump = false;
    launch_options.input_task_options.poll_interval = std::chrono::milliseconds{1};
    launch_options.start_io_telemetry_control = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    RuntimeGuard runtime_guard{space};

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

    auto events_inbox = std::string(button->getPath()) + "/events/inbox/queue";
    auto press_events = std::string(button->getPath()) + "/events/press/queue";

    auto inbox_event = space.take<SP::UI::Builders::Widgets::Reducers::WidgetAction>(
        events_inbox,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(inbox_event);
    CHECK(inbox_event->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate);

    auto press_event = space.take<SP::UI::Builders::Widgets::Reducers::WidgetAction>(
        press_events,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(press_event);
    CHECK(press_event->sequence == inbox_event->sequence);

}

TEST_CASE("paint_example_new-style button reacts to pointer press via widget runtime") {
    using namespace std::chrono_literals;
    PathSpace space;

    auto target_widget = std::make_shared<std::string>();
    auto target_authoring = std::make_shared<std::string>();

    SP::System::LaunchOptions launch_options{};
    launch_options.widget_event_options.refresh_interval = 1ms;
    launch_options.widget_event_options.idle_sleep = 1ms;
    launch_options.widget_event_options.hit_test_override =
        [target_widget, target_authoring](PathSpace&,
                                          std::string const&,
                                          float scene_x,
                                          float scene_y) -> SP::Expected<SP::UI::Builders::Scene::HitTestResult> {
            SP::UI::Builders::Scene::HitTestResult result{};
            if (!target_widget->empty()) {
                result.hit = true;
                result.target.authoring_node_id = *target_authoring;
                result.position.scene_x = scene_x;
                result.position.scene_y = scene_y;
                result.position.has_local = true;
                result.position.local_x = 8.0f;
                result.position.local_y = 8.0f;
            }
            return result;
        };

    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "paint_example_button");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.title = "Declarative Button";
    window_options.width = 400;
    window_options.height = 240;
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto pressed = std::make_shared<std::atomic<bool>>(false);

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Press Me";
    button_args.style.width = 240.0f;
    button_args.style.height = 64.0f;
    button_args.style.corner_radius = 16.0f;
    button_args.style.text_color = {0.95f, 0.98f, 1.0f, 1.0f};
    button_args.style.typography.font_size = 30.0f;
    button_args.style.typography.line_height = 36.0f;
    button_args.on_press = [pressed](SP::UI::Declarative::ButtonContext&) {
        pressed->store(true, std::memory_order_release);
    };

    auto button_width = button_args.style.width;
    auto button_height = button_args.style.height;

    SP::UI::Declarative::Stack::Args layout_args{};
    layout_args.style.axis = SP::UI::Builders::Widgets::StackAxis::Vertical;
    layout_args.style.align_main = SP::UI::Builders::Widgets::StackAlignMain::Center;
    layout_args.style.align_cross = SP::UI::Builders::Widgets::StackAlignCross::Center;
    layout_args.style.width = static_cast<float>(window_options.width);
    layout_args.style.height = static_cast<float>(window_options.height);
    auto vertical_padding = (layout_args.style.height - button_height) * 0.5f;
    if (vertical_padding < 0.0f) {
        vertical_padding = 0.0f;
    }
    auto horizontal_padding = (layout_args.style.width - button_width) * 0.5f;
    if (horizontal_padding < 0.0f) {
        horizontal_padding = 0.0f;
    }
    layout_args.style.padding_main_start = vertical_padding;
    layout_args.style.padding_main_end = vertical_padding;
    layout_args.style.padding_cross_start = horizontal_padding;
    layout_args.style.padding_cross_end = horizontal_padding;
    layout_args.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "button_panel",
        .fragment = SP::UI::Declarative::Button::Fragment(std::move(button_args)),
    });
    layout_args.active_panel = "button_panel";

    auto layout_width = layout_args.style.width;
    auto layout_height = layout_args.style.height;

    auto layout = SP::UI::Declarative::Stack::Create(space,
                                                     window_view,
                                                     "button_panel_root",
                                                     std::move(layout_args));
    REQUIRE(layout);
    auto activate = SP::UI::Declarative::Stack::SetActivePanel(space, *layout, "button_panel");
    REQUIRE(activate);

    auto button_path = layout->getPath() + "/children/button_panel";
    *target_widget = button_path;
    *target_authoring = button_path + "/authoring/button/background";

    auto token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    auto events_root = std::string("/system/widgets/runtime/events/");
    auto pointer_queue = events_root + token + "/pointer/queue";
    auto button_queue = events_root + token + "/button/queue";

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.pointer_id = 1;
    move.motion.absolute = true;
    move.motion.absolute_x = layout_width * 0.5f;
    move.motion.absolute_y = layout_height * 0.5f;
    (void)space.insert(pointer_queue, move);

    std::this_thread::sleep_for(5ms);

    SP::IO::ButtonEvent press_event{};
    press_event.source = SP::IO::ButtonSource::Mouse;
    press_event.device_path = "/system/devices/in/pointer/default";
    press_event.button_code = 1;
    press_event.button_id = 1;
    press_event.state.pressed = true;
    (void)space.insert(button_queue, press_event);

    auto release_event = press_event;
    release_event.state.pressed = false;
    (void)space.insert(button_queue, release_event);

    bool observed = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (pressed->load(std::memory_order_acquire)) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    CHECK(observed);
}
