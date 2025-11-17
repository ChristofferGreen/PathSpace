#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace SP;

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
