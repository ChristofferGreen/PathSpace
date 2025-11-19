#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <chrono>
#include <random>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace SP;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
using WidgetOp = SP::UI::Builders::Widgets::Bindings::WidgetOp;
using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

TEST_CASE("WidgetEventTrellis routes pointer and button events to WidgetOps") {
    PathSpace space;

    std::string window_path = "/system/applications/test_app/windows/main";
    auto token = SP::Runtime::MakeRuntimeWindowToken(window_path);
    std::string runtime_base = "/system/widgets/runtime/windows/" + token;

    (void)space.insert(runtime_base + "/window", window_path);
    (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/button/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/text/devices",
                       std::vector<std::string>{});

    (void)space.insert(window_path + "/views/main/scene", "scenes/main_scene");

    const std::string widget_path = window_path + "/widgets/test_button";
    const std::string pointer_queue = "/system/widgets/runtime/events/" + token + "/pointer/queue";
    const std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
    const std::string widget_ops_queue = widget_path + "/ops/inbox/queue";

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Builders::Scene::HitTestResult> {
            SP::UI::Builders::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_path + "/authoring/button/background";
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 120.0f;
    move.motion.absolute_y = 48.0f;
    (void)space.insert(pointer_queue, move);

    using WidgetOp = SP::UI::Builders::Widgets::Bindings::WidgetOp;
    auto hover = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(hover);
    CHECK(hover->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::HoverEnter);

    SP::IO::ButtonEvent press{};
    press.source = SP::IO::ButtonSource::Mouse;
    press.device_path = "/system/devices/in/pointer/default";
    press.button_code = 1;
    press.button_id = 1;
    press.state.pressed = true;
    (void)space.insert(button_queue, press);

    SP::IO::ButtonEvent release = press;
    release.state.pressed = false;
    (void)space.insert(button_queue, release);

    auto next_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(next_op);
    CHECK(next_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Press);

    auto release_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(release_op);
    CHECK(release_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Release);

    auto activate_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(activate_op);
    CHECK(activate_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate);

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}

TEST_CASE("WidgetEventTrellis handles keyboard button activation") {
    PathSpace space;
    std::string app_root = "/system/applications/test_app";
    std::string window_path = app_root + "/windows/main";
    auto token = SP::Runtime::MakeRuntimeWindowToken(window_path);
    std::string runtime_base = "/system/widgets/runtime/windows/" + token;

    (void)space.insert(runtime_base + "/window", window_path);
    (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                       std::vector<std::string>{});
    (void)space.insert(runtime_base + "/subscriptions/button/devices",
                       std::vector<std::string>{"/system/devices/in/keyboard/default"});
    (void)space.insert(runtime_base + "/subscriptions/text/devices",
                       std::vector<std::string>{});

    (void)space.insert(window_path + "/views/main/scene", "scenes/main_scene");
    (void)space.insert(app_root + "/widgets/focus/current",
                       window_path + "/widgets/test_button");
    (void)space.insert(std::string("scenes/main_scene/structure/window/main/focus/current"),
                       window_path + "/widgets/test_button");

    const std::string widget_path = window_path + "/widgets/test_button";
    const std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
    const std::string widget_ops_queue = widget_path + "/ops/inbox/queue";

    BuilderWidgets::ButtonState state{};
    (void)space.insert(widget_path + "/state", state);
    (void)space.insert(widget_path + "/render/dirty", false);
    (void)space.insert(widget_path + "/meta/kind", std::string{"button"});

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    SP::IO::ButtonEvent press{};
    press.source = SP::IO::ButtonSource::Keyboard;
    press.device_path = "/system/devices/in/keyboard/default";
    press.button_code = 0x31;
    press.button_id = 0;
    press.state.pressed = true;
    (void)space.insert(button_queue, press);

    SP::IO::ButtonEvent release = press;
    release.state.pressed = false;
    (void)space.insert(button_queue, release);

    using WidgetOp = SP::UI::Builders::Widgets::Bindings::WidgetOp;
    auto press_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(press_op);
    CHECK(press_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Press);

    auto release_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(release_op);
    CHECK(release_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Release);

    auto activate_op = space.take<WidgetOp, std::string>(
        widget_ops_queue,
        SP::Out{} & SP::Block{200ms});
    REQUIRE(activate_op);
    CHECK(activate_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::Activate);

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}

TEST_CASE("WidgetEventTrellis handles keyboard navigation for declarative widgets") {
    using WidgetOp = SP::UI::Builders::Widgets::Bindings::WidgetOp;

    auto setup_window = [](PathSpace& space,
                           std::string const& app_root,
                           std::string const& window_name,
                           std::vector<std::string> const& button_devices) {
        std::string window_path = app_root + "/windows/" + window_name;
        auto token = SP::Runtime::MakeRuntimeWindowToken(window_path);
        std::string runtime_base = "/system/widgets/runtime/windows/" + token;
        (void)space.insert(runtime_base + "/window", window_path);
        (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                           std::vector<std::string>{});
        (void)space.insert(runtime_base + "/subscriptions/button/devices", button_devices);
        (void)space.insert(runtime_base + "/subscriptions/text/devices",
                           std::vector<std::string>{});
        (void)space.insert(window_path + "/views/main/scene", "scenes/" + window_name + "_scene");
        return std::make_pair(window_path, token);
    };

    SUBCASE("slider arrow keys adjust value") {
        PathSpace space;
        std::string app_root = "/system/applications/test_app";
        auto [window_path, token] = setup_window(space,
                                                app_root,
                                                "slider",
                                                {"/system/devices/in/keyboard/default"});
        std::string scene_path = app_root + "/scenes/slider_scene";
        std::string slider_path = window_path + "/widgets/test_slider";
        (void)space.insert(scene_path + "/structure/window/slider/focus/current", slider_path);
        (void)space.insert(app_root + "/widgets/focus/current", slider_path);

        BuilderWidgets::SliderState slider_state{};
        slider_state.value = 0.5f;
        (void)space.insert(slider_path + "/state", slider_state);
        BuilderWidgets::SliderStyle slider_style{};
        (void)space.insert(slider_path + "/meta/style", slider_style);
        BuilderWidgets::SliderRange slider_range{};
        slider_range.minimum = 0.0f;
        slider_range.maximum = 1.0f;
        slider_range.step = 0.1f;
        (void)space.insert(slider_path + "/meta/range", slider_range);
        (void)space.insert(slider_path + "/meta/kind", std::string{"slider"});
        (void)space.insert(slider_path + "/render/dirty", false);

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);

        SP::IO::ButtonEvent right{};
        right.source = SP::IO::ButtonSource::Keyboard;
        right.device_path = "/system/devices/in/keyboard/default";
        right.button_code = 0x7C;
        right.button_id = 0x7C;
        right.state.pressed = true;
        std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
        (void)space.insert(button_queue, right);

        std::string widget_ops_queue = slider_path + "/ops/inbox/queue";
        auto update = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(update);
        CHECK(update->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::SliderUpdate);
        auto commit = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(commit);
        CHECK(commit->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::SliderCommit);

        auto new_state = space.read<BuilderWidgets::SliderState, std::string>(slider_path + "/state");
        REQUIRE(new_state);
        CHECK(doctest::Approx(new_state->value) == 0.6f);

        SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    }

    SUBCASE("list arrows move selection and Enter activates") {
        PathSpace space;
        std::string app_root = "/system/applications/test_app";
        auto [window_path, token] = setup_window(space,
                                                app_root,
                                                "list",
                                                {"/system/devices/in/keyboard/default"});
        std::string scene_path = app_root + "/scenes/list_scene";
        std::string list_path = window_path + "/widgets/test_list";
        (void)space.insert(scene_path + "/structure/window/list/focus/current", list_path);
        (void)space.insert(app_root + "/widgets/focus/current", list_path);

        BuilderWidgets::ListState list_state{};
        (void)space.insert(list_path + "/state", list_state);
        BuilderWidgets::ListStyle list_style{};
        list_style.width = 200.0f;
        list_style.item_height = 24.0f;
        (void)space.insert(list_path + "/meta/style", list_style);
        std::vector<BuilderWidgets::ListItem> items{{"first", "First", true},
                                                    {"second", "Second", true},
                                                    {"third", "Third", true}};
        (void)space.insert(list_path + "/meta/items", items);
        (void)space.insert(list_path + "/meta/kind", std::string{"list"});
        (void)space.insert(list_path + "/render/dirty", false);

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);

        std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
        auto send_key = [&](std::uint32_t code) {
            SP::IO::ButtonEvent key{};
            key.source = SP::IO::ButtonSource::Keyboard;
            key.device_path = "/system/devices/in/keyboard/default";
            key.button_code = code;
            key.button_id = static_cast<int>(code);
            key.state.pressed = true;
            (void)space.insert(button_queue, key);
        };

        send_key(0x7D); // down arrow

        std::string widget_ops_queue = list_path + "/ops/inbox/queue";
        auto hover = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(hover);
        CHECK(hover->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::ListHover);
        auto select = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(select);
        CHECK(select->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::ListSelect);
        CHECK(select->value == 1.0f);

        send_key(0x24); // return
        auto activate = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(activate);
        CHECK(activate->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::ListActivate);

        auto new_state = space.read<BuilderWidgets::ListState, std::string>(list_path + "/state");
        REQUIRE(new_state);
        CHECK(new_state->selected_index == 1);

        SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    }

    SUBCASE("tree arrows expand, collapse, and move selection") {
        PathSpace space;
        std::string app_root = "/system/applications/test_app";
        auto [window_path, token] = setup_window(space,
                                                app_root,
                                                "tree",
                                                {"/system/devices/in/keyboard/default"});
        std::string scene_path = app_root + "/scenes/tree_scene";
        std::string tree_path = window_path + "/widgets/test_tree";
        (void)space.insert(scene_path + "/structure/window/tree/focus/current", tree_path);
        (void)space.insert(app_root + "/widgets/focus/current", tree_path);

        BuilderWidgets::TreeState tree_state{};
        tree_state.selected_id = "root";
        (void)space.insert(tree_path + "/state", tree_state);
        BuilderWidgets::TreeStyle tree_style{};
        (void)space.insert(tree_path + "/meta/style", tree_style);
        std::vector<BuilderWidgets::TreeNode> nodes{{"root", "", "Root", true, true, true},
                                                    {"child", "root", "Child", true, true, true}};
        (void)space.insert(tree_path + "/meta/nodes", nodes);
        (void)space.insert(tree_path + "/meta/kind", std::string{"tree"});
        (void)space.insert(tree_path + "/render/dirty", false);

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);

        std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
        auto send_key = [&](std::uint32_t code) {
            SP::IO::ButtonEvent key{};
            key.source = SP::IO::ButtonSource::Keyboard;
            key.device_path = "/system/devices/in/keyboard/default";
            key.button_code = code;
            key.button_id = static_cast<int>(code);
            key.state.pressed = true;
            (void)space.insert(button_queue, key);
        };

        // Expand root
        send_key(0x7C); // right arrow
        auto toggle = space.take<WidgetOp, std::string>(
            tree_path + "/ops/inbox/queue",
            SP::Out{} & SP::Block{200ms});
        REQUIRE(toggle);
        CHECK(toggle->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TreeToggle);

        // Move to child
        send_key(0x7C);
        auto hover = space.take<WidgetOp, std::string>(
            tree_path + "/ops/inbox/queue",
            SP::Out{} & SP::Block{200ms});
        REQUIRE(hover);
        CHECK(hover->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TreeHover);
        auto select = space.take<WidgetOp, std::string>(
            tree_path + "/ops/inbox/queue",
            SP::Out{} & SP::Block{200ms});
        REQUIRE(select);
        CHECK(select->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TreeSelect);

        auto updated_state = space.read<BuilderWidgets::TreeState, std::string>(tree_path + "/state");
        REQUIRE(updated_state);
        CHECK(updated_state->selected_id == "child");

        SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    }

    SUBCASE("text input handles delete and submit") {
        PathSpace space;
        std::string app_root = "/system/applications/test_app";
        auto [window_path, token] = setup_window(space,
                                                app_root,
                                                "text",
                                                {"/system/devices/in/keyboard/default"});
        std::string scene_path = app_root + "/scenes/text_scene";
        std::string input_path = window_path + "/widgets/test_input";
        (void)space.insert(scene_path + "/structure/window/text/focus/current", input_path);
        (void)space.insert(app_root + "/widgets/focus/current", input_path);

        BuilderWidgets::TextFieldState text_state{};
        text_state.text = "Hello";
        text_state.cursor = 5;
        text_state.selection_start = 5;
        text_state.selection_end = 5;
        (void)space.insert(input_path + "/state", text_state);
        BuilderWidgets::TextFieldStyle text_style{};
        (void)space.insert(input_path + "/meta/style", text_style);
        (void)space.insert(input_path + "/meta/kind", std::string{"input_field"});
        (void)space.insert(input_path + "/render/dirty", false);

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);

        std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
        auto send_key = [&](std::uint32_t code) {
            SP::IO::ButtonEvent key{};
            key.source = SP::IO::ButtonSource::Keyboard;
            key.device_path = "/system/devices/in/keyboard/default";
            key.button_code = code;
            key.button_id = static_cast<int>(code);
            key.state.pressed = true;
            (void)space.insert(button_queue, key);
        };

        send_key(0x33); // delete backward
        std::string widget_ops_queue = input_path + "/ops/inbox/queue";
        auto delete_op = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(delete_op);
        CHECK(delete_op->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TextDelete);

        send_key(0x7B); // move cursor left
        auto move = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(move);
        CHECK(move->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TextMoveCursor);

        send_key(0x24); // submit
        auto submit = space.take<WidgetOp, std::string>(
            widget_ops_queue,
            SP::Out{} & SP::Block{200ms});
        REQUIRE(submit);
        CHECK(submit->kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::TextSubmit);

        auto new_state = space.read<BuilderWidgets::TextFieldState, std::string>(input_path + "/state");
        REQUIRE(new_state);
        CHECK(new_state->text == "Hell");

        SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    }
}

TEST_CASE("WidgetEventTrellis fuzzes declarative paint stroke ops") {
    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    launch_options.start_widget_event_trellis = false;
    launch_options.start_paint_gpu_uploader = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "trellis_paint_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "paint_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    std::string runtime_base = "/system/widgets/runtime/windows/" + token;
    (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                       std::vector<std::string>{std::string{kPointerDevice}});
    (void)space.insert(runtime_base + "/subscriptions/button/devices",
                       std::vector<std::string>{std::string{kPointerDevice}});
    (void)space.insert(runtime_base + "/subscriptions/text/devices",
                       std::vector<std::string>{});

    std::string view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto view = SP::App::ConcretePathView{view_path};

    SP::UI::Declarative::PaintSurface::Args args{};
    args.buffer_width = 160;
    args.buffer_height = 120;
    auto paint_widget = SP::UI::Declarative::PaintSurface::Create(space, view, "trellis_canvas", args);
    REQUIRE(paint_widget);
    auto widget_path = paint_widget->getPath();

    auto pointer_queue = "/system/widgets/runtime/events/" + token + "/pointer/queue";
    auto button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
    auto widget_ops_queue = widget_path + "/ops/inbox/queue";

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Builders::Scene::HitTestResult> {
            SP::UI::Builders::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_path + "/authoring/paint_surface/canvas";
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            result.position.local_x = scene_x;
            result.position.local_y = scene_y;
            result.position.has_local = true;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    auto send_pointer = [&](float x, float y) {
        SP::IO::PointerEvent evt{};
        evt.device_path = std::string{kPointerDevice};
        evt.motion.absolute = true;
        evt.motion.absolute_x = x;
        evt.motion.absolute_y = y;
        (void)space.insert(pointer_queue, evt);
    };

    auto send_button = [&](bool pressed) {
        SP::IO::ButtonEvent evt{};
        evt.source = SP::IO::ButtonSource::Mouse;
        evt.device_path = std::string{kPointerDevice};
        evt.button_code = 1;
        evt.button_id = 1;
        evt.state.pressed = pressed;
        (void)space.insert(button_queue, evt);
    };

    std::mt19937 rng{1337};
    std::uniform_real_distribution<float> dist_x(4.0f, static_cast<float>(args.buffer_width) - 4.0f);
    std::uniform_real_distribution<float> dist_y(4.0f, static_cast<float>(args.buffer_height) - 4.0f);
    std::uniform_int_distribution<int> dist_updates(1, 4);
    std::size_t commit_count = 0;

    auto drain_widget_ops = [&]() {
        while (true) {
            auto op = space.take<WidgetOp, std::string>(
                widget_ops_queue,
                SP::Out{} & SP::Block{50ms});
            if (!op) {
                auto const& error = op.error();
                if (error.code == SP::Error::Code::NoObjectFound
                    || error.code == SP::Error::Code::NoSuchPath
                    || error.code == SP::Error::Code::Timeout) {
                    break;
                }
                FAIL_CHECK("WidgetOp read failed: " << static_cast<int>(error.code));
                break;
            }
            WidgetAction action{};
            action.widget_path = op->widget_path;
            action.target_id = op->target_id;
            action.kind = op->kind;
            action.pointer = op->pointer;
            auto handled = PaintRuntime::HandleAction(space, action);
            REQUIRE(handled);
            if (action.kind == SP::UI::Builders::Widgets::Bindings::WidgetOpKind::PaintStrokeCommit) {
                ++commit_count;
            }
        }
    };

    constexpr int kStrokeIterations = 8;
    for (int stroke = 0; stroke < kStrokeIterations; ++stroke) {
        send_pointer(dist_x(rng), dist_y(rng));
        send_button(true);
        drain_widget_ops();

        int updates = dist_updates(rng);
        for (int step = 0; step < updates; ++step) {
            send_pointer(dist_x(rng), dist_y(rng));
            drain_widget_ops();
        }

        send_button(false);
        drain_widget_ops();
    }

    drain_widget_ops();

    auto records = PaintRuntime::LoadStrokeRecords(space, widget_path);
    REQUIRE(records);
    CHECK(records->size() == commit_count);
    for (auto const& stroke : *records) {
        CHECK_FALSE(stroke.points.empty());
    }

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}
