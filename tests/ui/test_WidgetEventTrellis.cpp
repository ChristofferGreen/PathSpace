#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>
#include <pathspace/ui/declarative/WidgetMailbox.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include "DeclarativeTestUtils.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace SP;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;
using WidgetMailboxEvent = SP::UI::Declarative::WidgetMailboxEvent;
using WidgetAction = SP::UI::Declarative::Reducers::WidgetAction;
namespace WidgetBindings = SP::UI::Runtime::Widgets::Bindings;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

inline auto widget_space(std::string const& root, std::string_view relative) -> std::string {
    return SP::UI::Runtime::Widgets::WidgetSpacePath(root, relative);
}

inline void ensure_widget_space(PathSpace& space, std::string const& widget_root) {
    auto root_insert = space.insert(widget_root, std::make_unique<PathSpace>());
    if (!root_insert.errors.empty()) {
        auto cleared = space.take<std::unique_ptr<PathSpace>>(widget_root);
        if (!cleared) {
            auto const code = cleared.error().code;
            bool allowed = (code == SP::Error::Code::NoSuchPath)
                || (code == SP::Error::Code::NoObjectFound);
            REQUIRE(allowed);
        }
        root_insert = space.insert(widget_root, std::make_unique<PathSpace>());
        REQUIRE(root_insert.errors.empty());
    }
    auto reset = SP::UI::Declarative::Detail::reset_widget_space(space, widget_root);
    REQUIRE(reset.has_value());
}

inline void set_mailbox_subscriptions(PathSpace& space,
                                      std::string const& widget_root,
                                      std::string const& kind) {
    std::vector<std::string> topics;
    if (kind == "button" || kind == "toggle") {
        topics = {"hover_enter", "hover_exit", "press", "release", "activate", "toggle"};
    } else if (kind == "label") {
        topics = {"hover_enter", "hover_exit", "activate"};
    } else if (kind == "slider") {
        topics = {"hover_enter", "hover_exit", "slider_begin", "slider_update", "slider_commit"};
    } else if (kind == "list") {
        topics = {"list_hover", "list_select", "list_activate", "list_scroll"};
    } else if (kind == "tree") {
        topics = {"tree_hover",
                  "tree_select",
                  "tree_toggle",
                  "tree_expand",
                  "tree_collapse",
                  "tree_request_load",
                  "tree_scroll"};
    } else if (kind == "input_field" || kind == "text_area") {
        topics = {"text_hover",
                  "text_focus",
                  "text_input",
                  "text_delete",
                  "text_move_cursor",
                  "text_set_selection",
                  "text_composition_start",
                  "text_composition_update",
                  "text_composition_commit",
                  "text_composition_cancel",
                  "text_clipboard_copy",
                  "text_clipboard_cut",
                  "text_clipboard_paste",
                  "text_scroll",
                  "text_submit"};
    } else if (kind == "stack") {
        topics = {"stack_select"};
    } else if (kind == "paint_surface") {
        topics = {"paint_stroke_begin", "paint_stroke_update", "paint_stroke_commit"};
    }

    if (!topics.empty()) {
        (void)space.insert(widget_space(widget_root, "/capsule/mailbox/subscriptions"), topics);
    }
}

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

struct EnvGuard {
    EnvGuard(std::string key, std::string value) : key_(std::move(key)) {
        auto existing = std::getenv(key_.c_str());
        if (existing) {
            previous_ = existing;
        }
        setenv(key_.c_str(), value.c_str(), 1);
    }

    ~EnvGuard() {
        if (previous_) {
            setenv(key_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(key_.c_str());
        }
    }

private:
    std::string key_;
    std::optional<std::string> previous_{};
};

struct TrellisGuard {
    explicit TrellisGuard(SP::PathSpace& s) : space(s) {}
    ~TrellisGuard() { SP::UI::Declarative::ShutdownWidgetEventTrellis(space); }

    SP::PathSpace& space;
};

TEST_CASE("WidgetEventTrellis routes pointer and button events to WidgetOps") {
    PathSpace space;
    EnvGuard debug_tree{"PATHSPACE_UI_DEBUG_TREE", "1"};

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
    const std::string mailbox_hover_queue = widget_space(widget_path, "/capsule/mailbox/events/hover_enter/queue");
    const std::string mailbox_press_queue = widget_space(widget_path, "/capsule/mailbox/events/press/queue");
    const std::string mailbox_release_queue = widget_space(widget_path, "/capsule/mailbox/events/release/queue");
    const std::string mailbox_activate_queue = widget_space(widget_path, "/capsule/mailbox/events/activate/queue");
    ensure_widget_space(space, widget_path);

    BuilderWidgets::ButtonState button_state{};
    (void)space.insert(widget_space(widget_path, "/state"), button_state);
    (void)space.insert(widget_space(widget_path, "/render/dirty"), false);
    (void)space.insert(widget_space(widget_path, "/meta/kind"), std::string{"button"});
    set_mailbox_subscriptions(space, widget_path, "button");

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_space(widget_path, "/authoring/button/background");
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);
    TrellisGuard trellis_guard{space};
    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 120.0f;
    move.motion.absolute_y = 48.0f;
    (void)space.insert(pointer_queue, move);

    auto hover_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_hover_queue,
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(hover_event);
    CHECK_EQ(hover_event->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::HoverEnter);

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

    auto press_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_press_queue,
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(press_event);
    CHECK_EQ(press_event->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Press);

    auto release_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_release_queue,
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(release_event);
    CHECK_EQ(release_event->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Release);

    auto activate_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_activate_queue,
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(activate_event);
    CHECK_EQ(activate_event->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate);

    using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;
    auto ops_inbox = space.take<WidgetOp, std::string>(widget_space(widget_path, "/ops/inbox/queue"));
    if (ops_inbox) {
        FAIL_CHECK("WidgetOps inbox should remain empty when capsule mailboxes are routed");
    } else {
        auto code = ops_inbox.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}

TEST_CASE("WidgetEventTrellis routes mailbox events when capsules are enabled") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
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
    ensure_widget_space(space, widget_path);

    BuilderWidgets::ButtonState button_state{};
    (void)space.insert(widget_space(widget_path, "/state"), button_state);
    (void)space.insert(widget_space(widget_path, "/render/dirty"), false);
    (void)space.insert(widget_space(widget_path, "/meta/kind"), std::string{"button"});
    (void)space.insert(widget_space(widget_path, "/capsule/mailbox/subscriptions"),
                       std::vector<std::string>{
                           "hover_enter",
                           "hover_exit",
                           "press",
                           "release",
                           "activate",
                       });

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_space(widget_path, "/authoring/button/background");
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);

    TrellisGuard trellis_guard{space};

    std::this_thread::sleep_for(100ms);

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 96.0f;
    move.motion.absolute_y = 40.0f;
    (void)space.insert(pointer_queue, move);

    auto mailbox_hover = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/hover_enter/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_hover);
    CHECK_EQ(mailbox_hover->topic, "hover_enter");

    SP::IO::ButtonEvent press{};
    press.source = SP::IO::ButtonSource::Mouse;
    press.device_path = "/system/devices/in/pointer/default";
    press.button_code = 1;
    press.button_id = 1;
    press.state.pressed = true;
    (void)space.insert(button_queue, press);

    auto mailbox_press = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/press/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_press);
    CHECK_EQ(mailbox_press->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Press);

    auto mailbox_total_path = widget_space(widget_path, "/capsule/mailbox/metrics/events_total");
    auto waited = DeclarativeTestUtils::wait_for_metric_at_least(
        space,
        mailbox_total_path,
        std::uint64_t{2},
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(waited);
}

TEST_CASE("WidgetEventTrellis routes label capsule mailboxes") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
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

    const std::string widget_path = window_path + "/widgets/test_label";
    const std::string pointer_queue = "/system/widgets/runtime/events/" + token + "/pointer/queue";
    const std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
    ensure_widget_space(space, widget_path);

    (void)space.insert(widget_space(widget_path, "/meta/kind"), std::string{"label"});
    (void)space.insert(widget_space(widget_path, "/capsule/mailbox/subscriptions"),
                       std::vector<std::string>{
                           "hover_enter",
                           "hover_exit",
                           "press",
                           "release",
                           "activate",
                       });

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_space(widget_path, "/authoring/label");
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);

    TrellisGuard trellis_guard{space};

    std::this_thread::sleep_for(100ms);

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 48.0f;
    move.motion.absolute_y = 24.0f;
    (void)space.insert(pointer_queue, move);

    auto mailbox_hover = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/hover_enter/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_hover);
    CHECK_EQ(mailbox_hover->topic, "hover_enter");

    SP::IO::ButtonEvent press{};
    press.source = SP::IO::ButtonSource::Mouse;
    press.device_path = "/system/devices/in/pointer/default";
    press.button_code = 1;
    press.button_id = 1;
    press.state.pressed = true;
    (void)space.insert(button_queue, press);

    auto mailbox_press = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/press/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_press);
    CHECK_EQ(mailbox_press->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Press);

    SP::IO::ButtonEvent release = press;
    release.state.pressed = false;
    (void)space.insert(button_queue, release);

    auto mailbox_release = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/release/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_release);

    auto mailbox_activate = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(widget_path, "/capsule/mailbox/events/activate/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(mailbox_activate);
    CHECK_EQ(mailbox_activate->kind, SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate);

    auto op_result = space.take<WidgetOp, std::string>(widget_space(widget_path, "/ops/inbox/queue"));
    if (op_result) {
        FAIL_CHECK("WidgetOp queue should be empty for label mailbox flow");
    } else {
        auto code = op_result.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }
}

TEST_CASE("Capsule mailbox metrics count dispatched ops") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
    PathSpace space;
    EnvGuard debug_tree{"PATHSPACE_UI_DEBUG_TREE", "1"};

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "capsule_mailbox_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "capsule_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "capsule_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "capsule_mailbox_button",
                                                      SP::UI::Declarative::Button::Args{.label = "Press"});
    REQUIRE(button);

    auto token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    std::string runtime_base = "/system/widgets/runtime/windows/" + token;

    (void)space.insert(runtime_base + "/window", window->path.getPath());
    (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/button/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/text/devices",
                       std::vector<std::string>{});

    (void)space.insert(window->path.getPath() + "/views/" + window->view_name + "/scene",
                       std::string(scene->path.getPath()));

    const std::string widget_path = button->getPath();
    const std::string pointer_queue = "/system/widgets/runtime/events/" + token + "/pointer/queue";
    const std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
    const std::string mailbox_press_queue = widget_space(widget_path, "/capsule/mailbox/events/press/queue");

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_space(widget_path, "/authoring/button/background");
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);

    TrellisGuard trellis_guard{space};

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 64.0f;
    move.motion.absolute_y = 32.0f;
    (void)space.insert(pointer_queue, move);

    SP::IO::ButtonEvent press{};
    press.source = SP::IO::ButtonSource::Mouse;
    press.device_path = "/system/devices/in/pointer/default";
    press.button_code = 1;
    press.button_id = 1;
    press.state.pressed = true;
    (void)space.insert(button_queue, press);

    auto press_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_press_queue,
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(press_event);

    SP::UI::Declarative::Detail::record_capsule_mailbox_event(
        space,
        widget_path,
        press_event->kind,
        press_event->target_id,
        press_event->timestamp_ns,
        press_event->sequence);

    auto mailbox_path = widget_space(widget_path, "/capsule/mailbox/metrics/events_total");
    auto mailbox_total = space.read<std::uint64_t, std::string>(mailbox_path);
    if (!mailbox_total) {
        auto inserted = space.insert(mailbox_path, std::uint64_t{0});
        REQUIRE(inserted.errors.empty());
        mailbox_total = space.read<std::uint64_t, std::string>(mailbox_path);
    }
    REQUIRE(mailbox_total);

    auto deadline = std::chrono::steady_clock::now()
        + DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{500});
    while (*mailbox_total < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        mailbox_total = space.read<std::uint64_t, std::string>(mailbox_path);
        if (!mailbox_total) {
            break;
        }
    }
    REQUIRE(mailbox_total);
    CHECK_GE(*mailbox_total, std::uint64_t{1});

    auto last_kind = space.read<std::string, std::string>(
        widget_space(widget_path, "/capsule/mailbox/metrics/last_event/kind"));
    REQUIRE(last_kind);
    REQUIRE_FALSE(last_kind->empty());
    CHECK((*last_kind == "press" || *last_kind == "hover_enter"));

    auto ops_inbox = space.take<WidgetOp, std::string>(widget_space(widget_path, "/ops/inbox/queue"));
    if (ops_inbox) {
        FAIL_CHECK("WidgetOps inbox should stay empty when mailboxes are routed");
    } else {
        auto code = ops_inbox.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    auto press_count = space.read<std::uint64_t, std::string>(
        widget_space(widget_path, "/capsule/mailbox/events/press/total"));
    if (press_count) {
        CHECK_GE(*press_count, std::uint64_t{1});
    }

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    REQUIRE(SP::Scene::Shutdown(space, scene->path));
}

TEST_CASE("Mailbox reducer dispatches capsule widgets without ops inbox") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
    EnvGuard capsule_only{"PATHSPACE_WIDGET_CAPSULES_ONLY", "0"};
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "mailbox_reducer_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "mailbox_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "mailbox_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto button_args = SP::UI::Declarative::Button::Args{
        .label = "Press",
        .on_press = [](SP::UI::Declarative::ButtonContext& ctx) {
            auto counter_path = widget_space(ctx.widget.getPath(), "/handler/presses");
            auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(counter_path, next);
        },
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                      window_view,
                                                      "mailbox_button",
                                                      std::move(button_args));
    REQUIRE(button);

    auto button_path = button->getPath();

    auto toggle_args = SP::UI::Declarative::Toggle::Args{
        .on_toggle = [](SP::UI::Declarative::ToggleContext& ctx) {
            auto counter_path = widget_space(ctx.widget.getPath(), "/handler/toggles");
            auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(counter_path, next);
        },
    };
    auto toggle = SP::UI::Declarative::Toggle::Create(space,
                                                      window_view,
                                                      "mailbox_toggle",
                                                      std::move(toggle_args));
    REQUIRE(toggle);

    auto toggle_path = toggle->getPath();

    auto slider_args = SP::UI::Declarative::Slider::Args{
        .minimum = 0.0f,
        .maximum = 1.0f,
        .value = 0.25f,
        .on_change = [](SP::UI::Declarative::SliderContext& ctx) {
            auto counter_path = widget_space(ctx.widget.getPath(), "/handler/changes");
            auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(counter_path, next);
        },
    };
    auto slider = SP::UI::Declarative::Slider::Create(space,
                                                      window_view,
                                                      "mailbox_slider",
                                                      slider_args);
    REQUIRE(slider);

    auto slider_path = slider->getPath();

    auto tree_args = SP::UI::Declarative::Tree::Args{};
    tree_args.nodes = {
        SP::UI::Runtime::Widgets::TreeNode{"root", "", "Root", true, true, true},
        SP::UI::Runtime::Widgets::TreeNode{"child", "root", "Child", true, false, true},
    };
    tree_args.on_node_event = [](SP::UI::Declarative::TreeNodeContext& ctx) {
        auto counter_path = widget_space(ctx.widget.getPath(), "/handler/node_events");
        auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
        auto next = current ? (*current + 1) : std::uint64_t{1};
        (void)ctx.space.insert(counter_path, next);
    };
    auto tree = SP::UI::Declarative::Tree::Create(space,
                                                  window_view,
                                                  "mailbox_tree",
                                                  std::move(tree_args));
    REQUIRE(tree);

    auto tree_path = tree->getPath();

    auto label_args = SP::UI::Declarative::Label::Args{
        .text = "Label",
        .on_activate = [](SP::UI::Declarative::LabelContext& ctx) {
            auto counter_path = widget_space(ctx.widget.getPath(), "/handler/activations");
            auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(counter_path, next);
        },
    };
    auto label = SP::UI::Declarative::Label::Create(space,
                                                    window_view,
                                                    "mailbox_label",
                                                    std::move(label_args));
    REQUIRE(label);

    auto label_path = label->getPath();
    auto label_counter = widget_space(label_path, "/handler/activations");
    auto slider_counter = widget_space(slider_path, "/handler/changes");
    auto tree_counter = widget_space(tree_path, "/handler/node_events");

    auto label_binding = space.read<SP::UI::Declarative::HandlerBinding, std::string>(
        widget_space(label_path, "/events/activate/handler"));
    REQUIRE(label_binding);
    INFO("label handler kind=" << static_cast<int>(label_binding->kind));
    CHECK(label_binding->kind == SP::UI::Declarative::HandlerKind::LabelActivate);

    WidgetMailboxEvent press{};
    press.topic = "press";
    press.kind = WidgetBindings::WidgetOpKind::Press;
    press.widget_path = button_path;
    press.target_id = "button/background";
    press.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    press.pointer.WithInside(true);
    press.pointer.WithPrimary(true);
    press.value = 1.0f;
    press.sequence = 1;
    press.timestamp_ns = 1;
    (void)space.insert(widget_space(button_path, "/capsule/mailbox/events/press/queue"), press);

    WidgetMailboxEvent toggle_event{};
    toggle_event.topic = "toggle";
    toggle_event.kind = WidgetBindings::WidgetOpKind::Toggle;
    toggle_event.widget_path = toggle_path;
    toggle_event.target_id = "toggle/track";
    toggle_event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    toggle_event.pointer.WithInside(true);
    toggle_event.pointer.WithPrimary(true);
    toggle_event.value = 1.0f;
    toggle_event.sequence = 2;
    toggle_event.timestamp_ns = 2;
    (void)space.insert(widget_space(toggle_path, "/capsule/mailbox/events/toggle/queue"), toggle_event);

    WidgetMailboxEvent slider_event{};
    slider_event.topic = "slider_update";
    slider_event.kind = WidgetBindings::WidgetOpKind::SliderUpdate;
    slider_event.widget_path = slider_path;
    slider_event.target_id = "slider/thumb";
    slider_event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    slider_event.pointer.WithInside(true);
    slider_event.pointer.WithPrimary(true);
    slider_event.value = 0.75f;
    slider_event.sequence = 3;
    slider_event.timestamp_ns = 3;
    (void)space.insert(widget_space(slider_path, "/capsule/mailbox/events/slider_update/queue"), slider_event);

    WidgetMailboxEvent tree_event{};
    tree_event.topic = "tree_select";
    tree_event.kind = WidgetBindings::WidgetOpKind::TreeSelect;
    tree_event.widget_path = tree_path;
    tree_event.target_id = "child";
    tree_event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    tree_event.pointer.WithInside(true);
    tree_event.pointer.WithPrimary(true);
    tree_event.value = 1.0f;
    tree_event.sequence = 4;
    tree_event.timestamp_ns = 4;
    (void)space.insert(widget_space(tree_path, "/capsule/mailbox/events/tree_select/queue"), tree_event);

    WidgetMailboxEvent label_event{};
    label_event.topic = "activate";
    label_event.kind = WidgetBindings::WidgetOpKind::Activate;
    label_event.widget_path = label_path;
    label_event.target_id = "label";
    label_event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    label_event.pointer.WithInside(true);
    label_event.pointer.WithPrimary(true);
    label_event.value = 1.0f;
    label_event.sequence = 5;
    label_event.timestamp_ns = 5;
    (void)space.insert(widget_space(label_path, "/capsule/mailbox/events/activate/queue"), label_event);

    SP::UI::Declarative::ManualPumpOptions pump_options{};
    pump_options.max_actions_per_widget = 8;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  pump_options);
    REQUIRE(pump_result);
    CHECK_GE(pump_result->actions_published, 5U);

    auto button_action = space.take<WidgetAction, std::string>(
        widget_space(button_path, "/ops/actions/inbox/queue"));
    REQUIRE(button_action);
    CHECK_EQ(button_action->widget_path, button_path);

    auto toggle_action = space.take<WidgetAction, std::string>(
        widget_space(toggle_path, "/ops/actions/inbox/queue"));
    REQUIRE(toggle_action);
    CHECK_EQ(toggle_action->widget_path, toggle_path);

    auto slider_action = space.take<WidgetAction, std::string>(
        widget_space(slider_path, "/ops/actions/inbox/queue"));
    REQUIRE(slider_action);
    CHECK_EQ(slider_action->widget_path, slider_path);

    auto tree_action = space.take<WidgetAction, std::string>(
        widget_space(tree_path, "/ops/actions/inbox/queue"));
    REQUIRE(tree_action);
    CHECK_EQ(tree_action->widget_path, tree_path);
    CHECK_EQ(tree_action->target_id, std::string{"child"});

    auto label_action = space.take<WidgetAction, std::string>(
        widget_space(label_path, "/ops/actions/inbox/queue"));
    REQUIRE(label_action);
    CHECK_EQ(label_action->widget_path, label_path);
    INFO("label action kind=" << static_cast<int>(label_action->kind)
         << " target=" << label_action->target_id);

    auto label_count = space.read<std::uint64_t, std::string>(label_counter);
    REQUIRE(label_count);
    CHECK_EQ(*label_count, std::uint64_t{1});

    auto slider_count = space.read<std::uint64_t, std::string>(slider_counter);
    REQUIRE(slider_count);
    CHECK_EQ(*slider_count, std::uint64_t{1});

    auto tree_count = space.read<std::uint64_t, std::string>(tree_counter);
    REQUIRE(tree_count);
    CHECK_EQ(*tree_count, std::uint64_t{1});

    auto error_msg = space.take<std::string, std::string>(
        "/system/widgets/runtime/events/log/errors/queue");
    while (error_msg) {
        INFO("widget error: " << *error_msg);
        error_msg = space.take<std::string, std::string>(
            "/system/widgets/runtime/events/log/errors/queue");
    }

    auto op_result = space.take<WidgetOp, std::string>(widget_space(button_path, "/ops/inbox/queue"));
    if (op_result) {
        FAIL_CHECK("WidgetOp queue should be empty when using mailbox reducer");
    } else {
        auto code = op_result.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    auto label_op = space.take<WidgetOp, std::string>(widget_space(label_path, "/ops/inbox/queue"));
    if (label_op) {
        FAIL_CHECK("WidgetOp queue should be empty for label mailbox reducer");
    } else {
        auto code = label_op.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    auto slider_op = space.take<WidgetOp, std::string>(widget_space(slider_path, "/ops/inbox/queue"));
    if (slider_op) {
        FAIL_CHECK("WidgetOp queue should be empty for slider mailbox reducer");
    } else {
        auto code = slider_op.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    auto tree_op = space.take<WidgetOp, std::string>(widget_space(tree_path, "/ops/inbox/queue"));
    if (tree_op) {
        FAIL_CHECK("WidgetOp queue should be empty for tree mailbox reducer");
    } else {
        auto code = tree_op.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }
}

TEST_CASE("Input capsule mailbox dispatch routes to handlers") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
    EnvGuard capsule_only{"PATHSPACE_WIDGET_CAPSULES_ONLY", "0"};
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "input_mailbox_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "input_mailbox_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "input_mailbox_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto input_args = SP::UI::Declarative::InputField::Args{
        .text = "hi",
        .placeholder = "type",
        .on_change = [](SP::UI::Declarative::InputFieldContext& ctx) {
            auto counter_path = widget_space(ctx.widget.getPath(), "/handler/changes");
            auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(counter_path, next);
        },
    };

    auto input = SP::UI::Declarative::InputField::Create(space,
                                                         window_view,
                                                         "mailbox_input",
                                                         input_args);
    REQUIRE(input);

    auto input_path = input->getPath();

    SP::UI::Declarative::WidgetMailboxEvent event{};
    event.topic = "text_input";
    event.kind = WidgetBindings::WidgetOpKind::TextInput;
    event.widget_path = input_path;
    event.target_id = "input_field/text";
    event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    event.pointer.WithInside(true);
    event.pointer.WithPrimary(true);
    event.value = 65.0f;
    event.sequence = 1;
    event.timestamp_ns = 1;
    (void)space.insert(widget_space(input_path, "/capsule/mailbox/events/text_input/queue"), event);

    SP::UI::Declarative::ManualPumpOptions pump_options{};
    pump_options.max_actions_per_widget = 4;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  pump_options);
    REQUIRE(pump_result);

    auto action = space.take<WidgetAction, std::string>(
        widget_space(input_path, "/ops/actions/inbox/queue"));
    REQUIRE(action);
    CHECK_EQ(action->kind, WidgetBindings::WidgetOpKind::TextInput);

    auto counter = space.read<std::uint64_t, std::string>(
        widget_space(input_path, "/handler/changes"));
    REQUIRE(counter);
    CHECK_EQ(*counter, std::uint64_t{1});

    auto op_inbox = space.take<WidgetOp, std::string>(widget_space(input_path, "/ops/inbox/queue"));
    if (op_inbox) {
        FAIL_CHECK("WidgetOp queue should be empty for input mailbox reducer");
    } else {
        auto code = op_inbox.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }
}

TEST_CASE("Stack mailbox dispatches via capsule queue") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
    EnvGuard capsule_only{"PATHSPACE_WIDGET_CAPSULES_ONLY", "0"};
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "stack_mailbox_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "stack_mailbox_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "stack_mailbox_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::Stack::Args args{};
    args.active_panel = "alpha";
    args.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "alpha",
        .fragment = SP::UI::Declarative::Label::Fragment(
            SP::UI::Declarative::Label::Args{.text = "Alpha"}),
    });
    args.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "beta",
        .fragment = SP::UI::Declarative::Label::Fragment(
            SP::UI::Declarative::Label::Args{.text = "Beta"}),
    });
    args.on_select = [](SP::UI::Declarative::StackPanelContext& ctx) {
        auto counter_path = widget_space(ctx.widget.getPath(), "/handler/selects");
        auto current = ctx.space.read<std::uint64_t, std::string>(counter_path);
        auto next = current ? (*current + 1) : std::uint64_t{1};
        (void)ctx.space.insert(counter_path, next);
    };

    auto stack = SP::UI::Declarative::Stack::Create(space,
                                                    window_view,
                                                    "mailbox_stack",
                                                    std::move(args));
    REQUIRE(stack);

    auto stack_path = stack->getPath();

    WidgetMailboxEvent select_event{};
    select_event.topic = "stack_select";
    select_event.kind = WidgetBindings::WidgetOpKind::StackSelect;
    select_event.widget_path = stack_path;
    select_event.target_id = "stack/panel/beta";
    select_event.pointer = WidgetBindings::PointerInfo::Make(0.0f, 0.0f);
    select_event.pointer.WithInside(true);
    select_event.pointer.WithPrimary(true);
    select_event.value = 1.0f;
    select_event.sequence = 1;
    select_event.timestamp_ns = 1;
    (void)space.insert(widget_space(stack_path, "/capsule/mailbox/events/stack_select/queue"), select_event);

    SP::UI::Declarative::ManualPumpOptions pump_options{};
    pump_options.max_actions_per_widget = 4;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  pump_options);
    REQUIRE(pump_result);

    auto action = space.take<WidgetAction, std::string>(
        widget_space(stack_path, "/ops/actions/inbox/queue"));
    REQUIRE(action);
    CHECK_EQ(action->kind, WidgetBindings::WidgetOpKind::StackSelect);

    auto counter = space.read<std::uint64_t, std::string>(
        widget_space(stack_path, "/handler/selects"));
    REQUIRE(counter);
    CHECK_EQ(*counter, std::uint64_t{1});

    auto op_inbox = space.take<WidgetOp, std::string>(widget_space(stack_path, "/ops/inbox/queue"));
    if (op_inbox) {
        FAIL_CHECK("WidgetOp queue should be empty for stack mailbox reducer");
    } else {
        auto code = op_inbox.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }
}

TEST_CASE("WidgetEventTrellis dispatches capsule mailboxes without legacy ops") {
    EnvGuard capsules_flag{"PATHSPACE_WIDGET_CAPSULES", "1"};
    EnvGuard capsule_only{"PATHSPACE_WIDGET_CAPSULES_ONLY", "0"};
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "capsule_trellis_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "capsule_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "capsule_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto button_args = SP::UI::Declarative::Button::Args{
        .label = "Press",
        .on_press = [](SP::UI::Declarative::ButtonContext& ctx) {
            auto handler_path = widget_space(ctx.widget.getPath(), "/handler/presses");
            auto current = ctx.space.read<std::uint64_t, std::string>(handler_path);
            auto next = current ? (*current + 1) : std::uint64_t{1};
            (void)ctx.space.insert(handler_path, next);
        },
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                       window_view,
                                                       "capsule_button",
                                                       std::move(button_args));
    REQUIRE(button);

    auto button_path = button->getPath();
    auto handler_path = widget_space(button_path, "/handler/presses");
    auto handler_init = space.insert(handler_path, std::uint64_t{0});
    REQUIRE(handler_init.errors.empty());

    auto token = SP::Runtime::MakeRuntimeWindowToken(window->path.getPath());
    std::string runtime_base = "/system/widgets/runtime/windows/" + token;

    (void)space.insert(runtime_base + "/window", window->path.getPath());
    (void)space.insert(runtime_base + "/subscriptions/pointer/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/button/devices",
                       std::vector<std::string>{"/system/devices/in/pointer/default"});
    (void)space.insert(runtime_base + "/subscriptions/text/devices",
                       std::vector<std::string>{});

    (void)space.insert(window->path.getPath() + "/views/" + window->view_name + "/scene",
                       std::string(scene->path.getPath()));

    const std::string pointer_queue = "/system/widgets/runtime/events/" + token + "/pointer/queue";
    const std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [button_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = "button/background";
            result.position.scene_x = scene_x;
            result.position.scene_y = scene_y;
            return result;
        };

    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);

    TrellisGuard trellis_guard{space};

    SP::IO::PointerEvent move{};
    move.device_path = "/system/devices/in/pointer/default";
    move.motion.absolute = true;
    move.motion.absolute_x = 96.0f;
    move.motion.absolute_y = 48.0f;
    (void)space.insert(pointer_queue, move);

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

    std::this_thread::sleep_for(250ms);

    auto pointer_total = space.read<std::uint64_t, std::string>(
        std::string{options.metrics_root} + "/pointer_events_total");
    auto button_total = space.read<std::uint64_t, std::string>(
        std::string{options.metrics_root} + "/button_events_total");
    INFO("pointer_events_total=" << (pointer_total ? *pointer_total : 0));
    INFO("button_events_total=" << (button_total ? *button_total : 0));

    auto mailbox_press = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        widget_space(button_path, "/capsule/mailbox/events/press/queue"),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(2000ms));
    REQUIRE(mailbox_press);
    (void)space.insert(widget_space(button_path, "/capsule/mailbox/events/press/queue"), *mailbox_press);

    SP::UI::Declarative::ManualPumpOptions pump_options{};
    pump_options.max_actions_per_widget = 8;
    auto pump_result = SP::UI::Declarative::PumpWindowWidgetsOnce(space,
                                                                  window->path,
                                                                  window->view_name,
                                                                  pump_options);
    REQUIRE(pump_result);
    CHECK_GE(pump_result->actions_published, 1U);

    auto button_action = space.take<WidgetAction, std::string>(
        widget_space(button_path, "/ops/actions/inbox/queue"));
    REQUIRE(button_action);
    CHECK_EQ(button_action->widget_path, button_path);

    auto op_result = space.take<WidgetOp, std::string>(widget_space(button_path, "/ops/inbox/queue"));
    if (op_result) {
        FAIL_CHECK("WidgetOp queue should remain empty for capsule mailbox flow");
    } else {
        auto code = op_result.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }
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
    auto mailbox_queue_for = [&](SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) {
        auto topic = SP::UI::Declarative::Mailbox::TopicFor(kind);
        return widget_space(widget_path, std::string{"/capsule/mailbox/events/"} + std::string{topic}
                                            + "/queue");
    };
    ensure_widget_space(space, widget_path);

    BuilderWidgets::ButtonState state{};
    (void)space.insert(widget_space(widget_path, "/state"), state);
    (void)space.insert(widget_space(widget_path, "/render/dirty"), false);
    (void)space.insert(widget_space(widget_path, "/meta/kind"), std::string{"button"});
    set_mailbox_subscriptions(space, widget_path, "button");

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
    REQUIRE(started);
    REQUIRE(*started);
    TrellisGuard trellis_guard{space};
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

    auto press_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Press),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(press_event);
    CHECK(press_event->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Press);

    auto release_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Release),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(release_event);
    CHECK(release_event->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Release);

    auto activate_event = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
        space,
        mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate),
        DeclarativeTestUtils::scaled_timeout(200ms),
        DeclarativeTestUtils::scaled_timeout(1500ms));
    REQUIRE(activate_event);
    CHECK(activate_event->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate);

    using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;
    auto ops_inbox = space.take<WidgetOp, std::string>(widget_space(widget_path, "/ops/inbox/queue"));
    if (ops_inbox) {
        FAIL_CHECK("WidgetOps inbox should stay empty when mailboxes handle keyboard events");
    } else {
        auto code = ops_inbox.error().code;
        CHECK((code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath));
    }

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}

TEST_CASE("WidgetEventTrellis handles keyboard navigation for declarative widgets") {
    using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;

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
        ensure_widget_space(space, slider_path);

        BuilderWidgets::SliderState slider_state{};
        slider_state.value = 0.5f;
        (void)space.insert(widget_space(slider_path, "/state"), slider_state);
        BuilderWidgets::SliderStyle slider_style{};
        (void)space.insert(widget_space(slider_path, "/meta/style"), slider_style);
        BuilderWidgets::SliderRange slider_range{};
        slider_range.minimum = 0.0f;
        slider_range.maximum = 1.0f;
        slider_range.step = 0.1f;
        (void)space.insert(widget_space(slider_path, "/meta/range"), slider_range);
        (void)space.insert(widget_space(slider_path, "/meta/kind"), std::string{"slider"});
        (void)space.insert(widget_space(slider_path, "/render/dirty"), false);
        set_mailbox_subscriptions(space, slider_path, "slider");

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);
        TrellisGuard trellis_guard{space};
        SP::IO::ButtonEvent right{};
        right.source = SP::IO::ButtonSource::Keyboard;
        right.device_path = "/system/devices/in/keyboard/default";
        right.button_code = 0x7C;
        right.button_id = 0x7C;
        right.state.pressed = true;
        std::string button_queue = "/system/widgets/runtime/events/" + token + "/button/queue";
        (void)space.insert(button_queue, right);

        auto mailbox_queue_for = [&](SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) {
            auto topic = SP::UI::Declarative::Mailbox::TopicFor(kind);
            return widget_space(slider_path, std::string{"/capsule/mailbox/events/"} + std::string{topic}
                                                   + "/queue");
        };

        auto update = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::SliderUpdate),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(update);
        CHECK(update->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::SliderUpdate);

        auto commit = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::SliderCommit),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(commit);
        CHECK(commit->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::SliderCommit);

        auto new_state = space.read<BuilderWidgets::SliderState, std::string>(widget_space(slider_path, "/state"));
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
        ensure_widget_space(space, list_path);

        BuilderWidgets::ListState list_state{};
        (void)space.insert(widget_space(list_path, "/state"), list_state);
        BuilderWidgets::ListStyle list_style{};
        list_style.width = 200.0f;
        list_style.item_height = 24.0f;
        (void)space.insert(widget_space(list_path, "/meta/style"), list_style);
        std::vector<BuilderWidgets::ListItem> items{{"first", "First", true},
                                                    {"second", "Second", true},
                                                    {"third", "Third", true}};
        (void)space.insert(widget_space(list_path, "/meta/items"), items);
        (void)space.insert(widget_space(list_path, "/meta/kind"), std::string{"list"});
        (void)space.insert(widget_space(list_path, "/render/dirty"), false);
        set_mailbox_subscriptions(space, list_path, "list");

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);
        TrellisGuard trellis_guard{space};

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

        auto mailbox_queue_for = [&](SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) {
            auto topic = SP::UI::Declarative::Mailbox::TopicFor(kind);
            return widget_space(list_path, std::string{"/capsule/mailbox/events/"} + std::string{topic}
                                                + "/queue");
        };

        auto hover = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListHover),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(hover);
        CHECK(hover->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListHover);

        auto select = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListSelect),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(select);
        CHECK(select->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListSelect);
        CHECK(select->value == 1.0f);

        send_key(0x24); // return
        auto activate = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListActivate),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(activate);
        CHECK(activate->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::ListActivate);

        auto new_state = space.read<BuilderWidgets::ListState, std::string>(widget_space(list_path, "/state"));
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
        ensure_widget_space(space, tree_path);

        auto mailbox_queue_for = [&](SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) {
            auto topic = SP::UI::Declarative::Mailbox::TopicFor(kind);
            return widget_space(tree_path, std::string{"/capsule/mailbox/events/"}
                                             + std::string{topic} + "/queue");
        };

        BuilderWidgets::TreeState tree_state{};
        tree_state.selected_id = "root";
        (void)space.insert(widget_space(tree_path, "/state"), tree_state);
        BuilderWidgets::TreeStyle tree_style{};
        (void)space.insert(widget_space(tree_path, "/meta/style"), tree_style);
        std::vector<BuilderWidgets::TreeNode> nodes{{"root", "", "Root", true, true, true},
                                                    {"child", "root", "Child", true, true, true}};
        (void)space.insert(widget_space(tree_path, "/meta/nodes"), nodes);
        (void)space.insert(widget_space(tree_path, "/meta/kind"), std::string{"tree"});
        (void)space.insert(widget_space(tree_path, "/render/dirty"), false);
        set_mailbox_subscriptions(space, tree_path, "tree");

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);
        TrellisGuard trellis_guard{space};

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
        auto toggle = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeToggle),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(toggle);
        CHECK(toggle->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeToggle);

        // Move to child
        send_key(0x7C);
        auto hover = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeHover),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(hover);
        CHECK(hover->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeHover);
        auto select = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeSelect),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(select);
        CHECK(select->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TreeSelect);

        auto updated_state = space.read<BuilderWidgets::TreeState, std::string>(widget_space(tree_path, "/state"));
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
        ensure_widget_space(space, input_path);

        BuilderWidgets::TextFieldState text_state{};
        text_state.text = "Hello";
        text_state.cursor = 5;
        text_state.selection_start = 5;
        text_state.selection_end = 5;
        (void)space.insert(widget_space(input_path, "/state"), text_state);
        BuilderWidgets::TextFieldStyle text_style{};
        (void)space.insert(widget_space(input_path, "/meta/style"), text_style);
        (void)space.insert(widget_space(input_path, "/meta/kind"), std::string{"input_field"});
        (void)space.insert(widget_space(input_path, "/render/dirty"), false);
        set_mailbox_subscriptions(space, input_path, "input_field");

        SP::UI::Declarative::WidgetEventTrellisOptions options;
        options.refresh_interval = 1ms;
        options.idle_sleep = 1ms;
        auto started = SP::UI::Declarative::CreateWidgetEventTrellis(space, options);
        REQUIRE(started);
        REQUIRE(*started);
        TrellisGuard trellis_guard{space};

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

        auto mailbox_queue_for = [&](SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) {
            auto topic = SP::UI::Declarative::Mailbox::TopicFor(kind);
            return widget_space(input_path, std::string{"/capsule/mailbox/events/"} + std::string{topic}
                                               + "/queue");
        };

        send_key(0x33); // delete backward
        auto delete_op = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextDelete),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(delete_op);
        CHECK(delete_op->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextDelete);

        send_key(0x7B); // move cursor left
        auto move = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextMoveCursor),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(move);
        CHECK(move->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextMoveCursor);

        send_key(0x24); // submit
        auto submit = DeclarativeTestUtils::take_with_retry<WidgetMailboxEvent>(
            space,
            mailbox_queue_for(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextSubmit),
            DeclarativeTestUtils::scaled_timeout(200ms),
            DeclarativeTestUtils::scaled_timeout(1500ms));
        REQUIRE(submit);
        CHECK(submit->kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::TextSubmit);

        auto new_state = space.read<BuilderWidgets::TextFieldState, std::string>(widget_space(input_path, "/state"));
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
    std::array<std::string, 3> mailbox_queues{
        widget_space(widget_path, "/capsule/mailbox/events/paint_stroke_begin/queue"),
        widget_space(widget_path, "/capsule/mailbox/events/paint_stroke_update/queue"),
        widget_space(widget_path, "/capsule/mailbox/events/paint_stroke_commit/queue"),
    };

    SP::UI::Declarative::WidgetEventTrellisOptions options;
    options.refresh_interval = 1ms;
    options.idle_sleep = 1ms;
    options.hit_test_override =
        [widget_path](PathSpace&,
                      std::string const&,
                      float scene_x,
                      float scene_y) -> SP::Expected<SP::UI::Runtime::Scene::HitTestResult> {
            SP::UI::Runtime::Scene::HitTestResult result{};
            result.hit = true;
            result.target.authoring_node_id = widget_space(widget_path, "/authoring/paint_surface/canvas");
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
    TrellisGuard trellis_guard{space};

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

    auto tight_timeout = DeclarativeTestUtils::read_env_timeout_override();
    if (!DeclarativeTestUtils::full_fuzz_enabled()
        && tight_timeout
        && tight_timeout->count() <= std::chrono::milliseconds{1000}.count()) {
        INFO("Skipping paint stroke fuzz when PATHSPACE_TEST_TIMEOUT<=1s");
        return;
    }

    std::mt19937 rng{1337};
    std::uniform_real_distribution<float> dist_x(4.0f, static_cast<float>(args.buffer_width) - 4.0f);
    std::uniform_real_distribution<float> dist_y(4.0f, static_cast<float>(args.buffer_height) - 4.0f);
    std::uniform_int_distribution<int> dist_updates(1, 4);
    std::size_t commit_count = 0;
    auto take_timeout = DeclarativeTestUtils::scaled_timeout(std::chrono::milliseconds{50}, 0.5);
    auto stroke_iterations = DeclarativeTestUtils::scaled_iterations(8);

    auto drain_widget_ops = [&]() {
        while (true) {
            std::optional<WidgetMailboxEvent> event;
            for (auto const& queue : mailbox_queues) {
                auto taken = space.take<WidgetMailboxEvent, std::string>(
                    queue,
                    SP::Out{} & SP::Block{take_timeout});
                if (taken) {
                    event = std::move(*taken);
                    break;
                }
                auto const& error = taken.error();
                if (error.code != SP::Error::Code::NoObjectFound
                    && error.code != SP::Error::Code::NoSuchPath
                    && error.code != SP::Error::Code::Timeout) {
                    FAIL_CHECK("Widget mailbox read failed: " << static_cast<int>(error.code));
                    return;
                }
            }

            if (!event) {
                break;
            }

            WidgetAction action{};
            action.widget_path = event->widget_path;
            action.target_id = event->target_id;
            action.kind = event->kind;
            action.pointer = event->pointer;
            action.sequence = event->sequence;
            action.timestamp_ns = event->timestamp_ns;
            auto handled = PaintRuntime::HandleAction(space, action);
            REQUIRE(handled);
            if (action.kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::PaintStrokeCommit) {
                ++commit_count;
            }
        }
    };

    for (int stroke = 0; stroke < stroke_iterations; ++stroke) {
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

    CHECK(commit_count == static_cast<std::size_t>(stroke_iterations));

    auto records = PaintRuntime::LoadStrokeRecords(space, widget_path);
    REQUIRE(records);
    CHECK(records->size() == commit_count);
    for (auto const& stroke : *records) {
        CHECK_FALSE(stroke.points.empty());
    }

    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
}
