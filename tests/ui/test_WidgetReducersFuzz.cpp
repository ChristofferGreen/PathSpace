#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace SP;
using namespace SP::UI::Builders;
namespace Widgets = SP::UI::Builders::Widgets;
namespace WidgetBindings = Widgets::Bindings;
namespace WidgetReducers = Widgets::Reducers;
using SP::UI::Builders::AutoRenderRequestEvent;

auto is_not_found_error(SP::Error::Code code) -> bool {
    return code == SP::Error::Code::NoObjectFound || code == SP::Error::Code::NoSuchPath;
}

struct TargetContext {
    std::string path;
    SurfaceDesc desc{};
};

struct ButtonContext {
    WidgetBindings::ButtonBinding binding{};
    Widgets::ButtonState state{};
    TargetContext target{};

    auto ops_queue_path() const -> std::string { return binding.options.ops_queue.getPath(); }
    auto actions_queue_path() const -> std::string {
        return WidgetReducers::DefaultActionsQueue(binding.widget.root).getPath();
    }
    auto render_queue_path() const -> std::string { return target.path + "/events/renderRequested/queue"; }
    auto dirty_hints_path() const -> std::string { return target.path + "/hints/dirtyRects"; }
};

struct ToggleContext {
    WidgetBindings::ToggleBinding binding{};
    Widgets::ToggleState state{};
    TargetContext target{};

    auto ops_queue_path() const -> std::string { return binding.options.ops_queue.getPath(); }
    auto actions_queue_path() const -> std::string {
        return WidgetReducers::DefaultActionsQueue(binding.widget.root).getPath();
    }
    auto render_queue_path() const -> std::string { return target.path + "/events/renderRequested/queue"; }
    auto dirty_hints_path() const -> std::string { return target.path + "/hints/dirtyRects"; }
};

struct SliderContext {
    WidgetBindings::SliderBinding binding{};
    Widgets::SliderState state{};
    Widgets::SliderRange range{};
    TargetContext target{};

    auto ops_queue_path() const -> std::string { return binding.options.ops_queue.getPath(); }
    auto actions_queue_path() const -> std::string {
        return WidgetReducers::DefaultActionsQueue(binding.widget.root).getPath();
    }
    auto render_queue_path() const -> std::string { return target.path + "/events/renderRequested/queue"; }
    auto dirty_hints_path() const -> std::string { return target.path + "/hints/dirtyRects"; }
};

struct ListContext {
    WidgetBindings::ListBinding binding{};
    Widgets::ListState state{};
    std::vector<Widgets::ListItem> items{};
    TargetContext target{};

    auto ops_queue_path() const -> std::string { return binding.options.ops_queue.getPath(); }
    auto actions_queue_path() const -> std::string {
        return WidgetReducers::DefaultActionsQueue(binding.widget.root).getPath();
    }
    auto render_queue_path() const -> std::string { return target.path + "/events/renderRequested/queue"; }
    auto dirty_hints_path() const -> std::string { return target.path + "/hints/dirtyRects"; }
};

auto random_in_range(std::mt19937& rng, float min_value, float max_value) -> float {
    std::uniform_real_distribution<float> dist(min_value, max_value);
    return dist(rng);
}

auto random_surface_point(std::mt19937& rng, SurfaceDesc const& desc) -> WidgetBindings::PointerInfo {
    auto width = static_cast<float>(std::max(1, desc.size_px.width));
    auto height = static_cast<float>(std::max(1, desc.size_px.height));
    WidgetBindings::PointerInfo pointer{};
    pointer.scene_x = random_in_range(rng, 0.0f, width);
    pointer.scene_y = random_in_range(rng, 0.0f, height);
    pointer.inside = true;
    pointer.primary = true;
    return pointer;
}

auto validate_dirty_rects(PathSpace& space,
                          std::string const& path,
                          SurfaceDesc const& desc,
                          bool expect_non_empty) -> void {
    auto hints = space.read<std::vector<DirtyRectHint>, std::string>(path);
    if (!hints) {
        CHECK_FALSE(expect_non_empty);
        if (hints.error().code != SP::Error::Code::NoObjectFound
            && hints.error().code != SP::Error::Code::NoSuchPath) {
            INFO("Dirty rect read unexpected error code = " << static_cast<int>(hints.error().code));
            CHECK(false);
        }
        return;
    }

    if (expect_non_empty) {
        REQUIRE_FALSE(hints->empty());
    }

    auto width = static_cast<float>(std::max(0, desc.size_px.width));
    auto height = static_cast<float>(std::max(0, desc.size_px.height));

    for (auto const& hint : *hints) {
        CHECK(hint.min_x >= 0.0f);
        CHECK(hint.min_y >= 0.0f);
        CHECK(hint.max_x >= hint.min_x);
        CHECK(hint.max_y >= hint.min_y);
        CHECK(hint.max_x <= width);
        CHECK(hint.max_y <= height);
    }
}

auto verify_auto_render(PathSpace& space,
                        std::string const& queue_path,
                        bool expect_event,
                        std::string_view reason) -> void {
    auto event = space.take<AutoRenderRequestEvent, std::string>(queue_path);
    if (expect_event) {
        REQUIRE(event);
        CHECK(event->reason == reason);
        auto extra = space.take<AutoRenderRequestEvent, std::string>(queue_path);
        CHECK_FALSE(extra);
        if (!extra) {
            CHECK(is_not_found_error(extra.error().code));
        }
    } else {
        CHECK_FALSE(event);
        if (!event) {
            CHECK(is_not_found_error(event.error().code));
        }
    }
}

auto drive_button(PathSpace& space, ButtonContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 4);
    auto op = static_cast<WidgetBindings::WidgetOpKind>(op_dist(rng));

    auto desired = ctx.state;
    WidgetBindings::PointerInfo pointer = random_surface_point(rng, ctx.target.desc);

    switch (op) {
    case WidgetBindings::WidgetOpKind::HoverEnter:
        desired.hovered = true;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::HoverExit:
        desired.hovered = false;
        desired.pressed = false;
        pointer.inside = false;
        pointer.scene_x = -1.0f;
        pointer.scene_y = -1.0f;
        break;
    case WidgetBindings::WidgetOpKind::Press:
        desired.hovered = true;
        desired.pressed = true;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::Release:
        desired.hovered = true;
        desired.pressed = false;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::Activate:
        desired.hovered = true;
        desired.pressed = false;
        pointer.inside = true;
        break;
    default:
        op = WidgetBindings::WidgetOpKind::HoverEnter;
        desired.hovered = true;
        pointer.inside = true;
        break;
    }

    auto dispatched = WidgetBindings::DispatchButton(space, ctx.binding, desired, op, pointer);
    REQUIRE(dispatched);

    auto stored = space.read<Widgets::ButtonState, std::string>(ctx.binding.widget.state.getPath());
    REQUIRE(stored);
    ctx.state = *stored;

    verify_auto_render(space, ctx.render_queue_path(), *dispatched, "widget/button");
    validate_dirty_rects(space, ctx.dirty_hints_path(), ctx.target.desc, *dispatched);
}

auto drive_toggle(PathSpace& space, ToggleContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 4);
    auto op = static_cast<WidgetBindings::WidgetOpKind>(op_dist(rng));

    auto desired = ctx.state;
    WidgetBindings::PointerInfo pointer = random_surface_point(rng, ctx.target.desc);

    switch (op) {
    case WidgetBindings::WidgetOpKind::HoverEnter:
        desired.hovered = true;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::HoverExit:
        desired.hovered = false;
        pointer.inside = false;
        pointer.scene_x = -2.0f;
        pointer.scene_y = -2.0f;
        break;
    case WidgetBindings::WidgetOpKind::Press:
        desired.hovered = true;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::Release:
        desired.hovered = true;
        pointer.inside = true;
        break;
    case WidgetBindings::WidgetOpKind::Toggle:
        desired.hovered = true;
        desired.checked = !ctx.state.checked;
        pointer.inside = true;
        break;
    default:
        op = WidgetBindings::WidgetOpKind::HoverEnter;
        desired.hovered = true;
        pointer.inside = true;
        break;
    }

    auto dispatched = WidgetBindings::DispatchToggle(space, ctx.binding, desired, op, pointer);
    REQUIRE(dispatched);

    auto stored = space.read<Widgets::ToggleState, std::string>(ctx.binding.widget.state.getPath());
    REQUIRE(stored);
    ctx.state = *stored;

    verify_auto_render(space, ctx.render_queue_path(), *dispatched, "widget/toggle");
    validate_dirty_rects(space, ctx.dirty_hints_path(), ctx.target.desc, *dispatched);
}

auto make_slider_value(std::mt19937& rng, Widgets::SliderRange const& range) -> float {
    auto min_value = std::min(range.minimum, range.maximum);
    auto max_value = std::max(range.minimum, range.maximum);
    return random_in_range(rng, min_value, max_value);
}

auto drive_slider(PathSpace& space, SliderContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 2);
    auto op_index = op_dist(rng);

    WidgetBindings::WidgetOpKind op_kind = WidgetBindings::WidgetOpKind::SliderBegin;
    switch (op_index) {
    case 0: op_kind = WidgetBindings::WidgetOpKind::SliderBegin; break;
    case 1: op_kind = WidgetBindings::WidgetOpKind::SliderUpdate; break;
    case 2: op_kind = WidgetBindings::WidgetOpKind::SliderCommit; break;
    default: break;
    }

    auto desired = ctx.state;
    desired.value = make_slider_value(rng, ctx.range);
    desired.hovered = true;

    if (op_kind == WidgetBindings::WidgetOpKind::SliderBegin) {
        desired.dragging = true;
    } else if (op_kind == WidgetBindings::WidgetOpKind::SliderUpdate) {
        desired.dragging = true;
    } else {
        desired.dragging = false;
    }

    WidgetBindings::PointerInfo pointer = random_surface_point(rng, ctx.target.desc);

    auto dispatched = WidgetBindings::DispatchSlider(space, ctx.binding, desired, op_kind, pointer);
    REQUIRE(dispatched);

    auto stored = space.read<Widgets::SliderState, std::string>(ctx.binding.widget.state.getPath());
    REQUIRE(stored);
    ctx.state = *stored;

    verify_auto_render(space, ctx.render_queue_path(), *dispatched, "widget/slider");
    validate_dirty_rects(space, ctx.dirty_hints_path(), ctx.target.desc, *dispatched);
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

auto drive_list(PathSpace& space, ListContext& ctx, std::mt19937& rng) -> void {
    std::uniform_int_distribution<int> op_dist(0, 3);
    auto op_index = op_dist(rng);

    WidgetBindings::WidgetOpKind op_kind = WidgetBindings::WidgetOpKind::ListHover;
    switch (op_index) {
    case 0: op_kind = WidgetBindings::WidgetOpKind::ListHover; break;
    case 1: op_kind = WidgetBindings::WidgetOpKind::ListSelect; break;
    case 2: op_kind = WidgetBindings::WidgetOpKind::ListActivate; break;
    case 3: op_kind = WidgetBindings::WidgetOpKind::ListScroll; break;
    default: break;
    }

    auto desired = ctx.state;
    std::int32_t item_index = -1;
    float scroll_delta = 0.0f;

    WidgetBindings::PointerInfo pointer = random_surface_point(rng, ctx.target.desc);
    pointer.inside = true;

    std::uniform_real_distribution<float> scroll_dist(-3.5f, 3.5f);

    switch (op_kind) {
    case WidgetBindings::WidgetOpKind::ListHover:
        item_index = random_list_index(rng, ctx.items.size(), true);
        pointer.inside = item_index >= 0;
        break;
    case WidgetBindings::WidgetOpKind::ListSelect:
        item_index = random_list_index(rng, ctx.items.size(), false);
        desired.selected_index = item_index;
        desired.hovered_index = item_index;
        break;
    case WidgetBindings::WidgetOpKind::ListActivate:
        item_index = random_list_index(rng, ctx.items.size(), false);
        desired.hovered_index = item_index;
        break;
    case WidgetBindings::WidgetOpKind::ListScroll:
        item_index = -1;
        scroll_delta = scroll_dist(rng);
        desired.scroll_offset = ctx.state.scroll_offset + scroll_delta;
        break;
    default:
        break;
    }

    auto dispatched = WidgetBindings::DispatchList(space,
                                                   ctx.binding,
                                                   desired,
                                                   op_kind,
                                                   pointer,
                                                   item_index,
                                                   scroll_delta);
    REQUIRE(dispatched);

    auto stored = space.read<Widgets::ListState, std::string>(ctx.binding.widget.state.getPath());
    REQUIRE(stored);
    ctx.state = *stored;

    verify_auto_render(space, ctx.render_queue_path(), *dispatched, "widget/list");
    validate_dirty_rects(space, ctx.dirty_hints_path(), ctx.target.desc, *dispatched);
}

template <typename Context>
auto reduce_actions(PathSpace& space, Context const& ctx) -> std::vector<WidgetReducers::WidgetAction> {
    auto ops_queue_path = ctx.ops_queue_path();
    auto ops_queue = SP::ConcretePathStringView{ops_queue_path};
    auto actions = WidgetReducers::ReducePending(space, ops_queue, 2048);
    REQUIRE(actions);
    REQUIRE_FALSE(actions->empty());
    return *std::move(actions);
}

auto verify_button_actions(PathSpace& space, ButtonContext const& ctx) -> void {
    auto actions = reduce_actions(space, ctx);

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.binding.widget.root.getPath());
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
        CHECK((action.analog_value == doctest::Approx(0.0f)
               || action.analog_value == doctest::Approx(1.0f)));
    }

    auto actions_queue = WidgetReducers::DefaultActionsQueue(ctx.binding.widget.root);
    auto actions_queue_path = std::string(actions_queue.getPath());
    auto actions_queue_view = SP::ConcretePathStringView{actions_queue_path};
    auto publish = WidgetReducers::PublishActions(space,
                                                  actions_queue_view,
                                                  std::span(actions.data(), actions.size()));
    REQUIRE(publish);

    for (auto const& expected : actions) {
        auto stored = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->discrete_index == expected.discrete_index);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
    }

    auto empty = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_toggle_actions(PathSpace& space, ToggleContext const& ctx) -> void {
    auto actions = reduce_actions(space, ctx);

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.binding.widget.root.getPath());
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
        CHECK((action.analog_value == doctest::Approx(0.0f)
               || action.analog_value == doctest::Approx(1.0f)));
    }

    auto actions_queue = WidgetReducers::DefaultActionsQueue(ctx.binding.widget.root);
    auto actions_queue_path = std::string(actions_queue.getPath());
    auto actions_queue_view = SP::ConcretePathStringView{actions_queue_path};
    auto publish = WidgetReducers::PublishActions(space,
                                                  actions_queue_view,
                                                  std::span(actions.data(), actions.size()));
    REQUIRE(publish);

    for (auto const& expected : actions) {
        auto stored = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
        CHECK(stored->sequence == expected.sequence);
    }

    auto empty = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_slider_actions(PathSpace& space, SliderContext const& ctx) -> void {
    auto actions = reduce_actions(space, ctx);

    auto min_value = std::min(ctx.range.minimum, ctx.range.maximum);
    auto max_value = std::max(ctx.range.minimum, ctx.range.maximum);

    std::uint64_t last_sequence = 0;
    for (auto const& action : actions) {
        CHECK(action.widget_path == ctx.binding.widget.root.getPath());
        CHECK(action.discrete_index == -1);
        CHECK(std::isfinite(action.pointer.scene_x));
        CHECK(std::isfinite(action.pointer.scene_y));
        CHECK(action.analog_value >= doctest::Approx(min_value));
        CHECK(action.analog_value <= doctest::Approx(max_value));
        CHECK(action.sequence > last_sequence);
        last_sequence = action.sequence;
    }

    auto actions_queue = WidgetReducers::DefaultActionsQueue(ctx.binding.widget.root);
    auto actions_queue_path = std::string(actions_queue.getPath());
    auto actions_queue_view = SP::ConcretePathStringView{actions_queue_path};
    auto publish = WidgetReducers::PublishActions(space,
                                                  actions_queue_view,
                                                  std::span(actions.data(), actions.size()));
    REQUIRE(publish);

    for (auto const& expected : actions) {
        auto stored = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
    }

    auto empty = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

auto verify_list_actions(PathSpace& space, ListContext const& ctx) -> void {
    auto actions = reduce_actions(space, ctx);

    std::uint64_t last_sequence = 0;
    auto const widget_path = ctx.binding.widget.root.getPath();
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

    auto actions_queue = WidgetReducers::DefaultActionsQueue(ctx.binding.widget.root);
    auto actions_queue_path = std::string(actions_queue.getPath());
    auto actions_queue_view = SP::ConcretePathStringView{actions_queue_path};
    auto publish = WidgetReducers::PublishActions(space,
                                                  actions_queue_view,
                                                  std::span(actions.data(), actions.size()));
    REQUIRE(publish);

    for (auto const& expected : actions) {
        auto stored = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
        REQUIRE(stored);
        CHECK(stored->kind == expected.kind);
        CHECK(stored->sequence == expected.sequence);
        CHECK(stored->widget_path == expected.widget_path);
        CHECK(stored->analog_value == doctest::Approx(expected.analog_value));
        CHECK(stored->discrete_index == expected.discrete_index);
    }

    auto empty = space.take<WidgetReducers::WidgetAction, std::string>(actions_queue_path);
    CHECK_FALSE(empty);
    if (!empty) {
        CHECK(is_not_found_error(empty.error().code));
    }
}

} // namespace

TEST_CASE("Widget reducers fuzz harness maintains invariants") {
    PathSpace space;
    AppRootPath app_root{"/system/applications/widget_fuzz"};
    auto root_view = SP::App::AppRootPathView{app_root.getPath()};

    RendererParams renderer_params{
        .name = "fuzz_renderer",
        .kind = RendererKind::Software2D,
        .description = "Renderer for fuzz harness",
    };
    auto renderer = Renderer::Create(space, root_view, renderer_params);
    REQUIRE(renderer);

    auto make_target = [&](std::string const& surface_name, SurfaceDesc desc) -> TargetContext {
        SurfaceParams surface_params{
            .name = surface_name,
            .desc = desc,
            .renderer = "renderers/" + renderer_params.name,
        };
        auto surface = Surface::Create(space, root_view, surface_params);
        REQUIRE(surface);

        auto target = Renderer::ResolveTargetBase(space,
                                                  root_view,
                                                  *renderer,
                                                  ("targets/surfaces/" + surface_name).c_str());
        REQUIRE(target);

        return TargetContext{target->getPath(), desc};
    };

    TargetContext button_target = make_target("fuzz_button_surface",
                                              SurfaceDesc{
                                                  .size_px = {256, 128},
                                                  .progressive_tile_size_px = 16,
                                              });
    TargetContext toggle_target = make_target("fuzz_toggle_surface",
                                              SurfaceDesc{
                                                  .size_px = {192, 96},
                                                  .progressive_tile_size_px = 16,
                                              });
    TargetContext slider_target = make_target("fuzz_slider_surface",
                                              SurfaceDesc{
                                                  .size_px = {320, 96},
                                                  .progressive_tile_size_px = 16,
                                              });
    TargetContext list_target = make_target("fuzz_list_surface",
                                            SurfaceDesc{
                                                .size_px = {240, 240},
                                                .progressive_tile_size_px = 16,
                                            });

    Widgets::ButtonParams button_params{
        .name = "primary_button",
        .label = "Fuzz Button",
    };
    auto button_paths = Widgets::CreateButton(space, root_view, button_params);
    REQUIRE(button_paths);

    Widgets::ToggleParams toggle_params{.name = "primary_toggle"};
    auto toggle_paths = Widgets::CreateToggle(space, root_view, toggle_params);
    REQUIRE(toggle_paths);

    Widgets::SliderParams slider_params{
        .name = "primary_slider",
        .minimum = -1.0f,
        .maximum = 1.0f,
        .value = 0.0f,
        .step = 0.0f,
    };
    auto slider_paths = Widgets::CreateSlider(space, root_view, slider_params);
    REQUIRE(slider_paths);

    Widgets::ListParams list_params{
        .name = "primary_list",
        .items = {
            Widgets::ListItem{.id = "alpha", .label = "Alpha"},
            Widgets::ListItem{.id = "beta", .label = "Beta"},
            Widgets::ListItem{.id = "gamma", .label = "Gamma"},
            Widgets::ListItem{.id = "delta", .label = "Delta"},
        },
    };
    auto list_paths = Widgets::CreateList(space, root_view, list_params);
    REQUIRE(list_paths);

    auto button_binding = WidgetBindings::CreateButtonBinding(space,
                                                              root_view,
                                                              *button_paths,
                                                              SP::ConcretePathStringView{button_target.path});
    REQUIRE(button_binding);
    auto toggle_binding = WidgetBindings::CreateToggleBinding(space,
                                                              root_view,
                                                              *toggle_paths,
                                                              SP::ConcretePathStringView{toggle_target.path});
    REQUIRE(toggle_binding);
    auto slider_binding = WidgetBindings::CreateSliderBinding(space,
                                                              root_view,
                                                              *slider_paths,
                                                              SP::ConcretePathStringView{slider_target.path});
    REQUIRE(slider_binding);
    auto list_binding = WidgetBindings::CreateListBinding(space,
                                                          root_view,
                                                          *list_paths,
                                                          SP::ConcretePathStringView{list_target.path});
    REQUIRE(list_binding);

    auto read_button_state = space.read<Widgets::ButtonState, std::string>(button_paths->state.getPath());
    REQUIRE(read_button_state);
    auto read_toggle_state = space.read<Widgets::ToggleState, std::string>(toggle_paths->state.getPath());
    REQUIRE(read_toggle_state);
    auto read_slider_state = space.read<Widgets::SliderState, std::string>(slider_paths->state.getPath());
    REQUIRE(read_slider_state);
    auto read_slider_range = space.read<Widgets::SliderRange, std::string>(slider_paths->range.getPath());
    REQUIRE(read_slider_range);
    auto read_list_state = space.read<Widgets::ListState, std::string>(list_paths->state.getPath());
    REQUIRE(read_list_state);
    auto read_list_items = space.read<std::vector<Widgets::ListItem>, std::string>(list_paths->items.getPath());
    REQUIRE(read_list_items);

    ButtonContext button_ctx{*button_binding, *read_button_state, button_target};
    ToggleContext toggle_ctx{*toggle_binding, *read_toggle_state, toggle_target};
    SliderContext slider_ctx{*slider_binding, *read_slider_state, *read_slider_range, slider_target};
    ListContext list_ctx{*list_binding, *read_list_state, *read_list_items, list_target};

    std::mt19937 rng{1337};
    std::uniform_int_distribution<int> widget_dist(0, 3);

    constexpr int kIterations = 200;
    for (int i = 0; i < kIterations; ++i) {
        switch (widget_dist(rng)) {
        case 0: drive_button(space, button_ctx, rng); break;
        case 1: drive_toggle(space, toggle_ctx, rng); break;
        case 2: drive_slider(space, slider_ctx, rng); break;
        case 3: drive_list(space, list_ctx, rng); break;
        default: break;
        }
    }

    verify_button_actions(space, button_ctx);
    verify_toggle_actions(space, toggle_ctx);
    verify_slider_actions(space, slider_ctx);
    verify_list_actions(space, list_ctx);
}
