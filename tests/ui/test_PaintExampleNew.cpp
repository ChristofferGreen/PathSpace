#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/examples/paint/PaintExampleNewUI.hpp>
#include "../../examples/declarative_example_shared.hpp"
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>

namespace {

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";

auto now_timestamp_ns() -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool pointer_tests_enabled() {
    return std::getenv("PATHSPACE_RUN_POINTER_DEVICE_TESTS") != nullptr;
}

struct TestHarness {
    SP::PathSpace space;
    RuntimeGuard guard;
    SP::Window::CreateResult window;
    SP::Scene::CreateResult scene;
    std::shared_ptr<std::atomic<bool>> pressed_flag;
    std::string button_path;
    float layout_width = 0.0f;
    float layout_height = 0.0f;

    TestHarness()
        : guard(space) {}
};

void init_harness(TestHarness& harness) {
    using namespace std::chrono_literals;

    auto ensured_devices = PathSpaceExamples::PaintExampleNew::EnsureInputDevices(harness.space);
    REQUIRE(ensured_devices);

    auto launch = SP::System::LaunchStandard(harness.space);
    REQUIRE(launch);

    auto app = SP::App::Create(harness.space, "paint_example_new_device_test");
    REQUIRE(app);

    SP::Window::CreateOptions window_opts;
    window_opts.title = "Paint Example Device Test";
    window_opts.width = 640;
    window_opts.height = 480;
    auto window = SP::Window::Create(harness.space, *app, window_opts);
    REQUIRE(window);
    harness.window = *window;
    auto disable_metal = PathSpaceExamples::force_window_software_renderer(harness.space,
                                                                           harness.window.path,
                                                                           harness.window.view_name);
    REQUIRE(disable_metal);

    auto scene = SP::Scene::Create(harness.space, *app, window->path);
    REQUIRE(scene);
    harness.scene = *scene;

    auto view_base = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto surface_rel = harness.space.read<std::string, std::string>(view_base + "/surface");
    REQUIRE(surface_rel);
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()}, *surface_rel);
    REQUIRE(surface_abs);
    auto surface_path = SP::UI::Builders::SurfacePath{surface_abs->getPath()};
    auto set_scene = SP::UI::Builders::Surface::SetScene(harness.space, surface_path, harness.scene.path);
    REQUIRE(set_scene);

    harness.pressed_flag = std::make_shared<std::atomic<bool>>(false);
    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Press Me";
    button_args.style.width = 240.0f;
    button_args.style.height = 64.0f;
    button_args.on_press = [flag = harness.pressed_flag](SP::UI::Declarative::ButtonContext&) {
        flag->store(true, std::memory_order_release);
    };

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto mounted = PathSpaceExamples::PaintExampleNew::MountButtonUI(harness.space,
                                                                     window_view,
                                                                     window_opts.width,
                                                                     window_opts.height,
                                                                     std::move(button_args));
    REQUIRE(mounted);
    harness.button_path = mounted->button_path;
    harness.layout_width = mounted->layout_width;
    harness.layout_height = mounted->layout_height;

    auto input_ready = PathSpaceExamples::PaintExampleNew::EnableWindowInput(harness.space,
                                                                             harness.window,
                                                                             "paint_example_new_device_test");
    REQUIRE(input_ready);

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.wait_for_runtime_metrics = true;
    readiness_options.scene_window_component_override = PathSpaceExamples::window_component_name(
        std::string(harness.window.path.getPath()));
    readiness_options.scene_view_override = harness.window.view_name;
    readiness_options.wait_for_buckets = false;
    readiness_options.wait_for_structure = false;
    readiness_options.force_scene_publish = true;
    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(harness.space,
                                                                       harness.scene.path,
                                                                       harness.window.path,
                                                                       harness.window.view_name,
                                                                       readiness_options);
    REQUIRE(readiness);
}

void send_pointer_event(TestHarness& harness, SP::PathIOMouse::Event event) {
    event.timestampNs = now_timestamp_ns();
    std::string queue = std::string(kPointerDevice) + "/events";
    (void)harness.space.insert(queue, event);
}

void send_pointer_click(TestHarness& harness, float x, float y) {
    SP::PathIOMouse::Event move{};
    move.type = SP::MouseEventType::AbsoluteMove;
    move.x = static_cast<int>(x);
    move.y = static_cast<int>(y);
    send_pointer_event(harness, move);

    SP::PathIOMouse::Event down{};
    down.type = SP::MouseEventType::ButtonDown;
    down.button = SP::MouseButton::Left;
    down.x = static_cast<int>(x);
    down.y = static_cast<int>(y);
    send_pointer_event(harness, down);

    SP::PathIOMouse::Event up = down;
    up.type = SP::MouseEventType::ButtonUp;
    send_pointer_event(harness, up);
}

void present_once(TestHarness& harness) {
    auto present = SP::UI::Builders::Window::Present(harness.space,
                                                    harness.window.path,
                                                    harness.window.view_name);
    REQUIRE(present);
}

} // namespace

TEST_CASE("paint_example_new enables pointer subscriptions") {
    TestHarness harness;
    init_harness(harness);

    auto pointer_subscriber_path =
        std::string(kPointerDevice) + "/config/push/subscribers/paint_example_new_device_test";
    auto keyboard_subscriber_path =
        std::string(kKeyboardDevice) + "/config/push/subscribers/paint_example_new_device_test";

    auto pointer_subscriber = harness.space.read<bool, std::string>(pointer_subscriber_path);
    REQUIRE(pointer_subscriber);
    CHECK(*pointer_subscriber);

    auto keyboard_subscriber = harness.space.read<bool, std::string>(keyboard_subscriber_path);
    REQUIRE(keyboard_subscriber);
    CHECK(*keyboard_subscriber);
}

TEST_CASE("paint_example_new button reacts to pointer device events") {
    using namespace std::chrono_literals;
    if (!pointer_tests_enabled()) {
        INFO("Set PATHSPACE_RUN_POINTER_DEVICE_TESTS=1 to exercise pointer dispatch");
        return;
    }
    TestHarness harness;
    init_harness(harness);

    send_pointer_click(harness, harness.layout_width * 0.5f, harness.layout_height * 0.5f);

    bool observed = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        present_once(harness);
        if (harness.pressed_flag->load(std::memory_order_acquire)) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    CHECK(observed);
}

TEST_CASE("paint_example_new pointer hover updates button state") {
    using namespace std::chrono_literals;
    if (!pointer_tests_enabled()) {
        INFO("Set PATHSPACE_RUN_POINTER_DEVICE_TESTS=1 to exercise pointer dispatch");
        return;
    }
    TestHarness harness;
    init_harness(harness);

    SP::PathIOMouse::Event move{};
    move.type = SP::MouseEventType::AbsoluteMove;
    move.x = static_cast<int>(harness.layout_width * 0.5f);
    move.y = static_cast<int>(harness.layout_height * 0.5f);
    send_pointer_event(harness, move);

    bool hovered = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        present_once(harness);
        auto state = harness.space.read<SP::UI::Builders::Widgets::ButtonState, std::string>(
            harness.button_path + "/state");
        if (state && state->hovered) {
            hovered = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    CHECK(hovered);
}
