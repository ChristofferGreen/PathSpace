#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <pathspace/path/ConcretePath.hpp>

#include "DeclarativeTestUtils.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

using namespace SP;

namespace {

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

auto app_component_from_window(SP::UI::WindowPath const& window) -> std::string {
    constexpr std::string_view kPrefix = "/system/applications/";
    std::string path = std::string(window.getPath());
    if (!path.starts_with(kPrefix)) {
        return {};
    }
    auto remainder = path.substr(kPrefix.size());
    auto slash = remainder.find('/');
    if (slash == std::string::npos) {
        return {};
    }
    return remainder.substr(0, slash);
}

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

    SP::Window::CreateOptions window_options;
    window_options.name = "inputwidgets_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Loop";
    SP::UI::Declarative::MountOptions mount_options;
    mount_options.policy = SP::UI::Declarative::MountPolicy::WindowWidgets;
    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "inputwidgets_button",
                                                      std::move(button_args),
                                                      mount_options);
    REQUIRE(button);

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.wait_for_revision = false;
    readiness_options.wait_for_structure = false;
    readiness_options.wait_for_buckets = false;
    readiness_options.wait_for_runtime_metrics = true;
    readiness_options.force_scene_publish = true;
    auto readiness = DeclarativeTestUtils::ensure_scene_ready(space,
                                                              scene->path,
                                                              window->path,
                                                              window->view_name,
                                                              readiness_options);
    if (!readiness) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("input task readiness", readiness.error()));
    }
    REQUIRE(readiness);

    auto widget_root = std::string(button->getPath());
    auto queue_path = widget_root + "/ops/inbox/queue";
    auto actions_path = widget_root + "/ops/actions/inbox/queue";


    auto window_token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    auto app_component = app_component_from_window(window->path);
    auto window_metric_path = std::string("/system/widgets/runtime/input/windows/")
                              + window_token + "/metrics/widgets_processed_total";
    auto app_metric_path = std::string("/system/widgets/runtime/input/apps/")
                           + app_component + "/metrics/widgets_processed_total";
    auto window_baseline_metric = DeclarativeTestUtils::read_metric(space, window_metric_path);
    std::uint64_t window_baseline = window_baseline_metric ? *window_baseline_metric : 0;
    auto app_baseline_metric = DeclarativeTestUtils::read_metric(space, app_metric_path);
    std::uint64_t app_baseline = app_baseline_metric ? *app_baseline_metric : 0;

    SP::UI::Builders::Widgets::Bindings::WidgetOp op{};
    op.kind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate;
    op.widget_path = widget_root;
    op.value = 1.0f;
    (void)space.insert(queue_path, op);

    SP::UI::Declarative::ManualPumpOptions pump_options{};
    pump_options.include_app_widgets = true;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  pump_options);
    REQUIRE(pump_result);
    CHECK(pump_result->widgets_processed >= 1);

    auto window_after_metric = DeclarativeTestUtils::read_metric(space, window_metric_path);
    REQUIRE(window_after_metric);
    CHECK(*window_after_metric >= window_baseline + pump_result->widgets_processed);
    auto app_after_metric = DeclarativeTestUtils::read_metric(space, app_metric_path);
    REQUIRE(app_after_metric);
    CHECK(*app_after_metric >= app_baseline + pump_result->widgets_processed);

    auto action = DeclarativeTestUtils::take_with_retry<SP::UI::Declarative::Reducers::WidgetAction>(
        space,
        actions_path,
        std::chrono::milliseconds{50},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{500}, 4.0));
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
    SP::Window::CreateOptions window_options;
    window_options.name = "handler_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::Button::Args args{};
    args.label = "Invoke";
    auto handler_flag = std::make_shared<std::atomic<bool>>(false);
    args.on_press = [handler_flag](SP::UI::Declarative::ButtonContext&) {
        handler_flag->store(true, std::memory_order_release);
    };

    SP::UI::Declarative::MountOptions mount_options;
    mount_options.policy = SP::UI::Declarative::MountPolicy::WindowWidgets;
    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "handler_button",
                                                      std::move(args),
                                                      mount_options);
    REQUIRE(button);

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.wait_for_revision = false;
    readiness_options.wait_for_structure = false;
    readiness_options.wait_for_buckets = false;
    readiness_options.wait_for_runtime_metrics = true;
    readiness_options.force_scene_publish = true;
    auto readiness = DeclarativeTestUtils::ensure_scene_ready(space,
                                                              scene->path,
                                                              window->path,
                                                              window->view_name,
                                                              readiness_options);
    if (!readiness) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("handler readiness", readiness.error()));
    }
    REQUIRE(readiness);

    auto widget_path = std::string(button->getPath());
    auto queue_path = widget_path + "/ops/inbox/queue";
    auto events_inbox = widget_path + "/events/inbox/queue";
    auto press_events = widget_path + "/events/press/queue";


    auto window_token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    auto app_component = app_component_from_window(window->path);
    auto window_metric_path = std::string("/system/widgets/runtime/input/windows/")
                              + window_token + "/metrics/widgets_processed_total";
    auto app_metric_path = std::string("/system/widgets/runtime/input/apps/")
                           + app_component + "/metrics/widgets_processed_total";
    auto window_baseline_metric = DeclarativeTestUtils::read_metric(space, window_metric_path);
    std::uint64_t window_baseline = window_baseline_metric ? *window_baseline_metric : 0;
    auto app_baseline_metric = DeclarativeTestUtils::read_metric(space, app_metric_path);
    std::uint64_t app_baseline = app_baseline_metric ? *app_baseline_metric : 0;

    SP::UI::Builders::Widgets::Bindings::WidgetOp op{};
    op.kind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate;
    op.widget_path = widget_path;
    op.value = 1.0f;
    (void)space.insert(queue_path, op);

    SP::UI::Declarative::ManualPumpOptions handler_pump_options{};
    handler_pump_options.include_app_widgets = true;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  handler_pump_options);
    REQUIRE(pump_result);
    CHECK(pump_result->widgets_processed >= 1);

    auto window_after_metric = DeclarativeTestUtils::read_metric(space, window_metric_path);
    REQUIRE(window_after_metric);
    CHECK(*window_after_metric >= window_baseline + pump_result->widgets_processed);
    auto app_after_metric = DeclarativeTestUtils::read_metric(space, app_metric_path);
    REQUIRE(app_after_metric);
    CHECK(*app_after_metric >= app_baseline + pump_result->widgets_processed);

    auto handler_budget = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{1500}, 2.5);
    auto handler_deadline = std::chrono::steady_clock::now() + handler_budget;
    bool observed = false;
    while (std::chrono::steady_clock::now() < handler_deadline) {
        if (handler_flag->load(std::memory_order_acquire)) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(observed);

    auto inbox_event = DeclarativeTestUtils::take_with_retry<SP::UI::Declarative::Reducers::WidgetAction>(
        space,
        events_inbox,
        std::chrono::milliseconds{50},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{500}, 4.0));
    REQUIRE(inbox_event);
    CHECK(inbox_event->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate);

    auto press_event = DeclarativeTestUtils::take_with_retry<SP::UI::Declarative::Reducers::WidgetAction>(
        space,
        press_events,
        std::chrono::milliseconds{50},
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{500}, 4.0));
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

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.wait_for_revision = false;
    readiness_options.wait_for_structure = false;
    readiness_options.wait_for_buckets = false;
    readiness_options.wait_for_runtime_metrics = true;
    readiness_options.force_scene_publish = true;
    auto readiness = DeclarativeTestUtils::ensure_scene_ready(space,
                                                              scene->path,
                                                              window->path,
                                                              window->view_name,
                                                              readiness_options);
    if (!readiness) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("paint button readiness", readiness.error()));
    }
    REQUIRE(readiness);

    auto button_path = layout->getPath() + "/children/button_panel";
    *target_widget = button_path;
    *target_authoring = button_path + "/authoring/button/background";


    auto token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    auto events_root = std::string("/system/widgets/runtime/events/");
    auto pointer_queue = events_root + token + "/pointer/queue";
    auto button_queue = events_root + token + "/button/queue";

    auto pointer_metric_path = std::string(DeclarativeTestUtils::kWidgetEventsPointerMetric);
    auto button_metric_path = std::string(DeclarativeTestUtils::kWidgetEventsButtonMetric);
    auto ops_metric_path = std::string(DeclarativeTestUtils::kWidgetEventsOpsMetric);
    auto pointer_baseline = DeclarativeTestUtils::read_metric(space, pointer_metric_path);
    REQUIRE(pointer_baseline);
    auto button_baseline = DeclarativeTestUtils::read_metric(space, button_metric_path);
    REQUIRE(button_baseline);
    auto ops_baseline = DeclarativeTestUtils::read_metric(space, ops_metric_path);
    REQUIRE(ops_baseline);

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.pointer_id = 1;
    move.motion.absolute = true;
    move.motion.absolute_x = layout_width * 0.5f;
    move.motion.absolute_y = layout_height * 0.5f;
    (void)space.insert(pointer_queue, move);
    auto pointer_wait = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        pointer_metric_path,
        *pointer_baseline + 1,
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{1000}, 3.0));
    if (!pointer_wait) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("pointer metric", pointer_wait.error()));
    }
    REQUIRE(pointer_wait);

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

    auto button_wait = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        button_metric_path,
        *button_baseline + 2,
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{1500}, 3.0));
    if (!button_wait) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("button metric", button_wait.error()));
    }
    REQUIRE(button_wait);
    auto ops_wait = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        ops_metric_path,
        *ops_baseline + 1,
        DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{1500}, 3.0));
    if (!ops_wait) {
        FAIL_CHECK(DeclarativeTestUtils::format_error("widget ops metric", ops_wait.error()));
    }
    REQUIRE(ops_wait);

    auto press_wait_budget = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{2000}, 3.0);
    auto press_deadline = std::chrono::steady_clock::now() + press_wait_budget;
    bool observed = false;
    while (std::chrono::steady_clock::now() < press_deadline) {
        if (pressed->load(std::memory_order_acquire)) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(observed);
}
