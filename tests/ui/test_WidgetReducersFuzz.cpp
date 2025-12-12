#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/WidgetMailbox.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace SP;
namespace Declarative = SP::UI::Declarative;
namespace WidgetsNS = SP::UI::Runtime::Widgets;
namespace WidgetBindings = SP::UI::Runtime::Widgets::Bindings;
namespace DeclarativeReducers = SP::UI::Declarative::Reducers;
namespace Mailbox = SP::UI::Declarative::Mailbox;

auto is_not_found_error(SP::Error::Code code) -> bool {
    return code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath;
}

struct DeclarativeFuzzFixture {
    PathSpace space;
    SP::App::AppRootPath app_root;
    SP::UI::WindowPath window_path;

    DeclarativeFuzzFixture() {
        auto launch = SP::System::LaunchStandard(space);
        REQUIRE(launch);
        auto app = SP::App::Create(space, "widget_fuzz_app");
        REQUIRE(app);
        app_root = *app;
        SP::Window::CreateOptions opts{};
        opts.name = "fuzz_window";
        opts.title = "Widget Reducers Fuzz";
        opts.visible = true;
        auto window = SP::Window::Create(space, app_root, opts);
        REQUIRE(window);
        window_path = window->path;
    }

    ~DeclarativeFuzzFixture() { SP::System::ShutdownDeclarativeRuntime(space); }

    auto parent_view() const -> SP::App::ConcretePathView {
        return SP::App::ConcretePathView{window_path.getPath()};
    }
};

struct Bounds {
    float width = 0.0f;
    float height = 0.0f;
};

struct WidgetQueues {
    std::string actions_queue;
    std::vector<std::string> mailbox_topics;
    std::uint64_t next_sequence = 1;
};

template <typename T>
auto overwrite_node(PathSpace& space, std::string const& path, T const& value) -> void {
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        (void)space.take<T, std::string>(path);
        auto retry = space.insert(path, value);
        REQUIRE(retry.errors.empty());
    }
}

auto mark_widget_dirty(PathSpace& space, std::string const& root_path) -> void {
    auto dirty_path = root_path + "/render/dirty";
    overwrite_node(space, dirty_path, true);
}

auto init_widget_mailbox(PathSpace& space,
                         SP::UI::Runtime::WidgetPath const& widget,
                         std::vector<std::string> topics) -> WidgetQueues {
    WidgetQueues queues;
    queues.actions_queue = DeclarativeReducers::DefaultActionsQueue(widget).getPath();
    queues.mailbox_topics = std::move(topics);

    auto subs_path = WidgetsNS::WidgetSpacePath(widget, "/capsule/mailbox/subscriptions");
    auto inserted = space.insert(subs_path, queues.mailbox_topics);
    REQUIRE(inserted.errors.empty());

    return queues;
}

auto random_in_range(std::mt19937& rng, float min_value, float max_value) -> float {
    std::uniform_real_distribution<float> dist(min_value, max_value);
    return dist(rng);
}

auto random_pointer(std::mt19937& rng, Bounds bounds) -> WidgetBindings::PointerInfo {
    auto width = std::max(bounds.width, 1.0f);
    auto height = std::max(bounds.height, 1.0f);
    auto x = random_in_range(rng, 0.0f, width);
    auto y = random_in_range(rng, 0.0f, height);
    return WidgetBindings::PointerInfo::Make(x, y).WithLocal(x, y).WithInside(true).WithPrimary(true);
}

struct ButtonContext {
    PathSpace* space = nullptr;
    SP::UI::Runtime::WidgetPath widget;
    std::string root_path;
    std::string state_path;
    WidgetsNS::ButtonState state{};
    Bounds bounds{};
    WidgetQueues queues{};
};

struct ToggleContext {
    PathSpace* space = nullptr;
    SP::UI::Runtime::WidgetPath widget;
    std::string root_path;
    std::string state_path;
    WidgetsNS::ToggleState state{};
    Bounds bounds{};
    WidgetQueues queues{};
};

struct SliderContext {
    PathSpace* space = nullptr;
    SP::UI::Runtime::WidgetPath widget;
    std::string root_path;
    std::string state_path;
    WidgetsNS::SliderState state{};
    WidgetsNS::SliderRange range{};
    Bounds bounds{};
    WidgetQueues queues{};
};

struct ListContext {
    PathSpace* space = nullptr;
    SP::UI::Runtime::WidgetPath widget;
    std::string root_path;
    std::string state_path;
    WidgetsNS::ListState state{};
    std::vector<WidgetsNS::ListItem> items;
    Bounds bounds{};
    WidgetQueues queues{};
};

auto enqueue_widget_op(PathSpace& space,
                       WidgetQueues& queues,
                       WidgetBindings::WidgetOpKind kind,
                       std::string_view widget_path,
                       std::string_view target_id,
                       WidgetBindings::PointerInfo pointer,
                       float analog_value) -> void {
    auto topic = Mailbox::TopicFor(kind);
    REQUIRE_FALSE(topic.empty());

    Declarative::WidgetMailboxEvent event{};
    event.topic = std::string{topic};
    event.kind = kind;
    event.widget_path = std::string(widget_path);
    event.target_id = std::string(target_id);
    event.pointer = pointer;
    event.value = analog_value;
    event.sequence = queues.next_sequence++;
    event.timestamp_ns = event.sequence * 100; // deterministic monotonic timestamp

    std::string queue_path{widget_path};
    queue_path.append("/capsule/mailbox/events/");
    queue_path.append(event.topic);
    queue_path.append("/queue");

    auto inserted = space.insert(queue_path, event);
    REQUIRE(inserted.errors.empty());
}

auto drive_button(ButtonContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 4);
    auto op = static_cast<WidgetBindings::WidgetOpKind>(op_dist(rng));

    auto desired = ctx.state;
    auto pointer = random_pointer(rng, ctx.bounds);

    switch (op) {
    case WidgetBindings::WidgetOpKind::HoverEnter:
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::HoverExit:
        desired.hovered = false;
        desired.pressed = false;
        pointer.WithInside(false);
        pointer.scene_x = -1.0f;
        pointer.scene_y = -1.0f;
        pointer.local_x = -1.0f;
        pointer.local_y = -1.0f;
        break;
    case WidgetBindings::WidgetOpKind::Press:
        desired.hovered = true;
        desired.pressed = true;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::Release:
        desired.hovered = true;
        desired.pressed = false;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::Activate:
        desired.hovered = true;
        desired.pressed = false;
        pointer.WithInside(true);
        break;
    default:
        op = WidgetBindings::WidgetOpKind::HoverEnter;
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    }

    overwrite_node(*ctx.space, ctx.state_path, desired);
    mark_widget_dirty(*ctx.space, ctx.root_path);
    ctx.state = desired;

    float analog = ctx.state.pressed ? 1.0f : 0.0f;
    if (op == WidgetBindings::WidgetOpKind::Activate) {
        analog = 1.0f;
    }

    enqueue_widget_op(*ctx.space,
                      ctx.queues,
                      op,
                      ctx.root_path,
                      "widget/button",
                      pointer,
                      analog);
}

auto drive_toggle(ToggleContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 4);
    auto op = static_cast<WidgetBindings::WidgetOpKind>(op_dist(rng));

    auto desired = ctx.state;
    auto pointer = random_pointer(rng, ctx.bounds);

    switch (op) {
    case WidgetBindings::WidgetOpKind::HoverEnter:
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::HoverExit:
        desired.hovered = false;
        pointer.WithInside(false);
        pointer.scene_x = -2.0f;
        pointer.scene_y = -2.0f;
        pointer.local_x = -2.0f;
        pointer.local_y = -2.0f;
        break;
    case WidgetBindings::WidgetOpKind::Press:
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::Release:
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::Toggle:
        desired.hovered = true;
        desired.checked = !ctx.state.checked;
        pointer.WithInside(true);
        break;
    default:
        op = WidgetBindings::WidgetOpKind::HoverEnter;
        desired.hovered = true;
        pointer.WithInside(true);
        break;
    }

    overwrite_node(*ctx.space, ctx.state_path, desired);
    mark_widget_dirty(*ctx.space, ctx.root_path);
    ctx.state = desired;

    float analog = ctx.state.checked ? 1.0f : 0.0f;
    enqueue_widget_op(*ctx.space,
                      ctx.queues,
                      op,
                      ctx.root_path,
                      "widget/toggle",
                      pointer,
                      analog);
}

auto make_slider_value(std::mt19937& rng, WidgetsNS::SliderRange const& range) -> float {
    auto min_value = std::min(range.minimum, range.maximum);
    auto max_value = std::max(range.minimum, range.maximum);
    return random_in_range(rng, min_value, max_value);
}

auto drive_slider(SliderContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 2);
    auto op_index = op_dist(rng);

    WidgetBindings::WidgetOpKind op = WidgetBindings::WidgetOpKind::SliderBegin;
    switch (op_index) {
    case 0: op = WidgetBindings::WidgetOpKind::SliderBegin; break;
    case 1: op = WidgetBindings::WidgetOpKind::SliderUpdate; break;
    case 2: op = WidgetBindings::WidgetOpKind::SliderCommit; break;
    default: break;
    }

    auto desired = ctx.state;
    desired.value = make_slider_value(rng, ctx.range);
    desired.hovered = true;
    desired.dragging = (op != WidgetBindings::WidgetOpKind::SliderCommit);

    auto pointer = random_pointer(rng, ctx.bounds);

    overwrite_node(*ctx.space, ctx.state_path, desired);
    mark_widget_dirty(*ctx.space, ctx.root_path);
    ctx.state = desired;

    enqueue_widget_op(*ctx.space,
                      ctx.queues,
                      op,
                      ctx.root_path,
                      "widget/slider/thumb",
                      pointer,
                      ctx.state.value);
    if (op == WidgetBindings::WidgetOpKind::SliderCommit) {
        ctx.state.dragging = false;
    }
}

auto random_list_index(std::mt19937& rng, std::size_t count, bool allow_negative) -> std::int32_t {
    if (count == 0) {
        return -1;
    }
    std::uniform_int_distribution<int> index_dist(0, static_cast<int>(count - 1));
    if (allow_negative) {
        std::uniform_int_distribution<int> choice(0, 4);
        if (choice(rng) == 0) {
            return -1;
        }
    }
    return index_dist(rng);
}

auto list_target_id(std::int32_t index) -> std::string {
    if (index >= 0) {
        return "widget/list/item/" + std::to_string(index);
    }
    return "widget/list";
}

auto drive_list(ListContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 3);
    auto op_index = op_dist(rng);

    WidgetBindings::WidgetOpKind op = WidgetBindings::WidgetOpKind::ListHover;
    switch (op_index) {
    case 0: op = WidgetBindings::WidgetOpKind::ListHover; break;
    case 1: op = WidgetBindings::WidgetOpKind::ListSelect; break;
    case 2: op = WidgetBindings::WidgetOpKind::ListActivate; break;
    case 3: op = WidgetBindings::WidgetOpKind::ListScroll; break;
    default: break;
    }

    auto desired = ctx.state;
    std::int32_t item_index = -1;
    float scroll_delta = 0.0f;

    auto pointer = random_pointer(rng, ctx.bounds);

    switch (op) {
    case WidgetBindings::WidgetOpKind::ListHover:
        item_index = random_list_index(rng, ctx.items.size(), true);
        desired.hovered_index = item_index;
        pointer.WithInside(item_index >= 0);
        if (item_index < 0) {
            pointer.scene_x = -3.0f;
            pointer.scene_y = -3.0f;
        }
        break;
    case WidgetBindings::WidgetOpKind::ListSelect:
        item_index = random_list_index(rng, ctx.items.size(), false);
        desired.selected_index = item_index;
        desired.hovered_index = item_index;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::ListActivate:
        item_index = random_list_index(rng, ctx.items.size(), false);
        desired.hovered_index = item_index;
        pointer.WithInside(true);
        break;
    case WidgetBindings::WidgetOpKind::ListScroll: {
        std::uniform_real_distribution<float> scroll_dist(-3.5f, 3.5f);
        scroll_delta = scroll_dist(rng);
        desired.scroll_offset = ctx.state.scroll_offset + scroll_delta;
        pointer.WithInside(true);
        break;
    }
    default:
        break;
    }

    overwrite_node(*ctx.space, ctx.state_path, desired);
    mark_widget_dirty(*ctx.space, ctx.root_path);
    ctx.state = desired;

    float analog = (op == WidgetBindings::WidgetOpKind::ListScroll && std::isfinite(scroll_delta))
                       ? scroll_delta
                       : static_cast<float>(item_index);

    enqueue_widget_op(*ctx.space,
                      ctx.queues,
                      op,
                      ctx.root_path,
                      list_target_id(item_index),
                      pointer,
                      analog);
}

template <typename Context>
auto reduce_actions(PathSpace& space, Context const& ctx)
    -> DeclarativeReducers::ProcessActionsResult {
    auto processed = DeclarativeReducers::ProcessPendingActions(space, ctx.widget, 2048);
    REQUIRE(processed);
    REQUIRE_FALSE(processed->actions.empty());
    return *std::move(processed);
}

auto verify_button_actions(PathSpace& space, ButtonContext const& ctx) -> void {
    auto result = reduce_actions(space, ctx);
    auto const& actions = result.actions;
    auto actions_queue = result.actions_queue.getPath();

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.root_path);
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
        CHECK((action.analog_value == doctest::Approx(0.0f)
               || action.analog_value == doctest::Approx(1.0f)));
    }

    for (auto const& expected : actions) {
        auto stored = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
    }

    auto empty = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_toggle_actions(PathSpace& space, ToggleContext const& ctx) -> void {
    auto result = reduce_actions(space, ctx);
    auto const& actions = result.actions;
    auto actions_queue = result.actions_queue.getPath();

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.root_path);
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
        CHECK((action.analog_value == doctest::Approx(0.0f)
               || action.analog_value == doctest::Approx(1.0f)));
    }

    for (auto const& expected : actions) {
        auto stored = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
    }

    auto empty = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_slider_actions(PathSpace& space, SliderContext const& ctx) -> void {
    auto result = reduce_actions(space, ctx);
    auto const& actions = result.actions;
    auto actions_queue = result.actions_queue.getPath();

    auto min_value = std::min(ctx.range.minimum, ctx.range.maximum);
    auto max_value = std::max(ctx.range.minimum, ctx.range.maximum);

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.root_path);
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.analog_value >= doctest::Approx(min_value));
        CHECK(action.analog_value <= doctest::Approx(max_value));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
    }

    for (auto const& expected : actions) {
        auto stored = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
    }

    auto empty = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_list_actions(PathSpace& space, ListContext const& ctx) -> void {
    auto result = reduce_actions(space, ctx);
    auto const& actions = result.actions;
    auto actions_queue = result.actions_queue.getPath();

    std::uint64_t last_sequence = 0;
    auto const& widget_path = ctx.root_path;
    auto const count = static_cast<std::int32_t>(ctx.items.size());

    for (auto const& action : actions) {
        CHECK(action.widget_path == widget_path);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;

        switch (action.kind) {
        case WidgetBindings::WidgetOpKind::ListHover:
        case WidgetBindings::WidgetOpKind::ListSelect:
        case WidgetBindings::WidgetOpKind::ListActivate:
            CHECK(action.discrete_index >= -1);
            if (action.discrete_index >= 0) {
                CHECK(action.discrete_index < count);
                CHECK(action.analog_value == doctest::Approx(static_cast<float>(action.discrete_index)));
            }
            break;
        case WidgetBindings::WidgetOpKind::ListScroll:
            CHECK(action.discrete_index == -1);
            CHECK(std::isfinite(action.analog_value));
            break;
        default:
            CHECK(action.discrete_index == -1);
            break;
        }
    }

    for (auto const& expected : actions) {
        auto stored = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
        CHECK(stored->discrete_index == expected.discrete_index);
    }

    auto empty = space.take<DeclarativeReducers::WidgetAction, std::string>(actions_queue);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

} // namespace

TEST_CASE("Widget reducers fuzz harness maintains invariants") {
    DeclarativeFuzzFixture fx;

    auto parent = fx.parent_view();

    auto button = Declarative::Button::Create(fx.space,
                                              parent,
                                              "primary_button",
                                              Declarative::Button::Args{.label = "Fuzz Button"});
    REQUIRE(button);
    auto toggle = Declarative::Toggle::Create(fx.space, parent, "primary_toggle");
    REQUIRE(toggle);

    Declarative::Slider::Args slider_args{};
    slider_args.minimum = -1.0f;
    slider_args.maximum = 1.0f;
    slider_args.value = 0.0f;
    auto slider = Declarative::Slider::Create(fx.space, parent, "primary_slider", slider_args);
    REQUIRE(slider);

    Declarative::List::Args list_args{};
    list_args.items = {
        {"alpha", "Alpha"},
        {"beta", "Beta"},
        {"gamma", "Gamma"},
        {"delta", "Delta"},
    };
    auto list = Declarative::List::Create(fx.space, parent, "primary_list", list_args);
    REQUIRE(list);

    auto button_root = WidgetsNS::WidgetSpacePath(button->getPath(), "");
    auto toggle_root = WidgetsNS::WidgetSpacePath(toggle->getPath(), "");
    auto slider_root = WidgetsNS::WidgetSpacePath(slider->getPath(), "");
    auto list_root = WidgetsNS::WidgetSpacePath(list->getPath(), "");

    auto button_state_path = WidgetsNS::WidgetSpacePath(button->getPath(), "/state");
    auto button_state = fx.space.read<WidgetsNS::ButtonState, std::string>(button_state_path);
    REQUIRE(button_state);
    auto toggle_state_path = WidgetsNS::WidgetSpacePath(toggle->getPath(), "/state");
    auto toggle_state = fx.space.read<WidgetsNS::ToggleState, std::string>(toggle_state_path);
    REQUIRE(toggle_state);
    auto slider_state_path = WidgetsNS::WidgetSpacePath(slider->getPath(), "/state");
    auto slider_state = fx.space.read<WidgetsNS::SliderState, std::string>(slider_state_path);
    REQUIRE(slider_state);
    WidgetsNS::SliderRange slider_range{
        .minimum = slider_args.minimum,
        .maximum = slider_args.maximum,
        .step = slider_args.step,
    };
    auto list_state_path = WidgetsNS::WidgetSpacePath(list->getPath(), "/state");
    auto list_state = fx.space.read<WidgetsNS::ListState, std::string>(list_state_path);
    REQUIRE(list_state);

    std::vector<std::string> button_topics{
        "hover_enter", "hover_exit", "press", "release", "activate"};
    std::vector<std::string> toggle_topics{
        "hover_enter", "hover_exit", "press", "release", "toggle"};
    std::vector<std::string> slider_topics{
        "slider_begin", "slider_update", "slider_commit"};
    std::vector<std::string> list_topics{
        "list_hover", "list_select", "list_activate", "list_scroll"};

    ButtonContext button_ctx{
        .space = &fx.space,
        .widget = *button,
        .root_path = button_root,
        .state_path = button_state_path,
        .state = *button_state,
        .bounds = Bounds{256.0f, 128.0f},
        .queues = init_widget_mailbox(fx.space, *button, button_topics),
    };

    ToggleContext toggle_ctx{
        .space = &fx.space,
        .widget = *toggle,
        .root_path = toggle_root,
        .state_path = toggle_state_path,
        .state = *toggle_state,
        .bounds = Bounds{192.0f, 96.0f},
        .queues = init_widget_mailbox(fx.space, *toggle, toggle_topics),
    };

    SliderContext slider_ctx{
        .space = &fx.space,
        .widget = *slider,
        .root_path = slider_root,
        .state_path = slider_state_path,
        .state = *slider_state,
        .range = slider_range,
        .bounds = Bounds{320.0f, 96.0f},
        .queues = init_widget_mailbox(fx.space, *slider, slider_topics),
    };

    auto list_height = list_args.style.border_thickness * 2.0f
                       + list_args.style.item_height * static_cast<float>(std::max<std::size_t>(list_args.items.size(), 1u));
    ListContext list_ctx{
        .space = &fx.space,
        .widget = *list,
        .root_path = list_root,
        .state_path = list_state_path,
        .state = *list_state,
        .items = list_args.items,
        .bounds = Bounds{list_args.style.width, list_height},
        .queues = init_widget_mailbox(fx.space, *list, list_topics),
    };

    std::mt19937 rng{1337};
    std::uniform_int_distribution<int> widget_dist(0, 3);

    constexpr int kIterations = 200;
    for (int i = 0; i < kIterations; ++i) {
        switch (widget_dist(rng)) {
        case 0: drive_button(button_ctx, rng); break;
        case 1: drive_toggle(toggle_ctx, rng); break;
        case 2: drive_slider(slider_ctx, rng); break;
        case 3: drive_list(list_ctx, rng); break;
        default: break;
        }
    }

    verify_button_actions(fx.space, button_ctx);
    verify_toggle_actions(fx.space, toggle_ctx);
    verify_slider_actions(fx.space, slider_ctx);
    verify_list_actions(fx.space, list_ctx);
}
