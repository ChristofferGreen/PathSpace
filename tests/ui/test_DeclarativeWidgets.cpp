#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetDetail.hpp>

#include <algorithm>
#include <string>
#include <span>

using namespace std::chrono_literals;

namespace {

using namespace SP;
using namespace SP::UI::Declarative;
namespace Runtime = SP::UI::Runtime;
namespace WidgetsNS = SP::UI::Runtime::Widgets;
namespace DetailNS = SP::UI::Declarative::Detail;
namespace ThemeConfig = SP::UI::Declarative::ThemeConfig;

auto LoadActiveThemeName(PathSpace& space, SP::App::AppRootPathView app_root) -> std::string {
    if (auto active = ThemeConfig::LoadActive(space, app_root)) {
        if (!active->empty()) {
            return *active;
        }
    } else {
        auto const& error = active.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            REQUIRE_MESSAGE(false, error.message.value_or("LoadActive failed"));
        }
    }
    auto system_theme = ThemeConfig::LoadSystemActive(space);
    REQUIRE(system_theme.has_value());
    return *system_theme;
}

auto LoadActiveTheme(PathSpace& space, SP::App::AppRootPathView app_root) -> WidgetsNS::WidgetTheme {
    auto name = LoadActiveThemeName(space, app_root);
    auto selection = WidgetsNS::LoadTheme(space, app_root, name);
    REQUIRE(selection.has_value());
    return selection->theme;
}

struct DeclarativeFixture {
    PathSpace space;
    DeclarativeFixture() {
        auto launch = SP::System::LaunchStandard(space);
        REQUIRE(launch.has_value());
        auto app = SP::App::Create(space, "test_app");
        REQUIRE(app.has_value());
        app_root = *app;
        SP::Window::CreateOptions window_opts{};
        window_opts.name = window_name;
        window_opts.title = "Main";
        auto window_result =
            SP::Window::Create(space, app_root, window_opts);
        REQUIRE(window_result.has_value());
        window_path = window_result->path;
    }

    ~DeclarativeFixture() {
        SP::System::ShutdownDeclarativeRuntime(space);
    }

    auto parent_view() const -> SP::App::ConcretePathView {
        return SP::App::ConcretePathView{window_path.getPath()};
    }

    SP::App::AppRootPath app_root;
    SP::UI::WindowPath window_path;
    std::string window_name = "main_window";
};

TEST_CASE("Declarative Button mounts under window widgets") {
    DeclarativeFixture fx;

    auto button = Button::Create(fx.space,
                                 fx.parent_view(),
                                 "hello_button",
                                 Button::Args{.label = "Hello"});
    REQUIRE(button.has_value());

    auto state =
        fx.space.read<SP::UI::Runtime::Widgets::ButtonState, std::string>(button->getPath()
                                                                           + "/state");
    REQUIRE(state.has_value());
    CHECK(state->enabled);
    auto label =
        fx.space.read<std::string, std::string>(button->getPath() + "/meta/label");
    REQUIRE(label.has_value());
    CHECK_EQ(*label, "Hello");

    REQUIRE(Button::SetLabel(fx.space, *button, "Updated").has_value());
    auto updated =
        fx.space.read<std::string, std::string>(button->getPath() + "/meta/label");
    REQUIRE(updated.has_value());
    CHECK_EQ(*updated, "Updated");
}

TEST_CASE("Declarative List mounts child fragments") {
    DeclarativeFixture fx;

    List::Args args{};
    args.items.push_back({"alpha", "Alpha"});
    args.children.push_back({"label_child", Label::Fragment(Label::Args{.text = "Nested"})});

    auto list =
        List::Create(fx.space, fx.parent_view(), "list_widget", std::move(args));
    REQUIRE(list.has_value());

    auto child_text = fx.space.read<std::string, std::string>(
        list->getPath() + "/children/label_child/state/text");
    REQUIRE(child_text.has_value());
    CHECK_EQ(*child_text, "Nested");
}

TEST_CASE("Slider clamps value and SetValue updates render flag") {
    DeclarativeFixture fx;

    Slider::Args args{};
    args.minimum = 0.0f;
    args.maximum = 10.0f;
    args.value = 5.0f;
    auto slider =
        Slider::Create(fx.space, fx.parent_view(), "volume_slider", args);
    REQUIRE(slider.has_value());

    REQUIRE(Slider::SetValue(fx.space, *slider, 42.0f).has_value());
    auto state =
        fx.space.read<SP::UI::Runtime::Widgets::SliderState, std::string>(
            slider->getPath() + "/state");
    REQUIRE(state.has_value());
    CHECK_EQ(state->value, doctest::Approx(10.0f));

    auto dirty =
        fx.space.read<bool, std::string>(slider->getPath() + "/render/dirty");
    REQUIRE(dirty.has_value());
    CHECK(*dirty);
}

TEST_CASE("WidgetDescriptor reproduces button bucket") {
    DeclarativeFixture fx;
    auto button = Button::Create(fx.space,
                                 fx.parent_view(),
                                 "descriptor_button",
                                 Button::Args{.label = "Descriptor"});
    REQUIRE(button.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *button);
    REQUIRE(descriptor.has_value());

    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());

    auto theme = LoadActiveTheme(fx.space, SP::App::AppRootPathView{fx.app_root.getPath()});
    auto style = theme.button;
    auto state = fx.space.read<WidgetsNS::ButtonState, std::string>((*button).getPath()
                                                                    + "/state");
    REQUIRE(state.has_value());
    WidgetsNS::ButtonPreviewOptions preview{};
    preview.authoring_root = button->getPath();
    preview.label = "Descriptor";
    auto reference = WidgetsNS::BuildButtonPreview(style, *state, preview);

    auto const& descriptor_button = std::get<ButtonDescriptor>(descriptor->data);
    CHECK(descriptor_button.style.background_color[0] == doctest::Approx(style.background_color[0]));

    REQUIRE_EQ(bucket->command_payload.size(), reference.command_payload.size());
    for (std::size_t i = 0; i < bucket->command_payload.size(); ++i) {
        CHECK(bucket->command_payload[i]
              == doctest::Approx(reference.command_payload[i]).epsilon(1e-5f));
    }
    CHECK(bucket->drawable_ids == reference.drawable_ids);
    CHECK(bucket->command_kinds == reference.command_kinds);
}

TEST_CASE("Button styles record explicit override intent") {
    DeclarativeFixture fx;

    SUBCASE("Default style has no overrides") {
        Button::Args args{};
        args.label = "Default";
        auto widget =
            Button::Create(fx.space, fx.parent_view(), "default_button", args);
        REQUIRE(widget.has_value());
        auto style =
            fx.space.read<WidgetsNS::ButtonStyle, std::string>(widget->getPath()
                                                               + "/meta/style");
        REQUIRE(style.has_value());
        CHECK_EQ(style->overrides, 0);
    }

    SUBCASE("Custom colors set the override mask") {
        Button::Args args{};
        args.label = "Custom";
        args.style.background_color = {0.05f, 0.2f, 0.6f, 1.0f};
        auto widget =
            Button::Create(fx.space, fx.parent_view(), "custom_button", args);
        REQUIRE(widget.has_value());
        auto style =
            fx.space.read<WidgetsNS::ButtonStyle, std::string>(widget->getPath()
                                                               + "/meta/style");
        REQUIRE(style.has_value());
        CHECK(HasStyleOverride(style->overrides,
                               WidgetsNS::ButtonStyleOverrideField::BackgroundColor));
        CHECK_FALSE(HasStyleOverride(style->overrides,
                                     WidgetsNS::ButtonStyleOverrideField::TextColor));
    }
}

TEST_CASE("Button preserves explicit overrides after theme defaults") {
    DeclarativeFixture fx;

    Button::Args args{};
    args.label = "ThemeAware";
    args.style.background_color = {0.21f, 0.46f, 0.72f, 1.0f};
    args.style.text_color = {0.95f, 0.92f, 0.35f, 1.0f};

    auto widget =
        Button::Create(fx.space, fx.parent_view(), "theme_button", args);
    REQUIRE(widget.has_value());

    auto style =
        fx.space.read<WidgetsNS::ButtonStyle, std::string>(widget->getPath()
                                                           + "/meta/style");
    REQUIRE(style.has_value());

    CHECK(style->background_color[0]
          == doctest::Approx(args.style.background_color[0]));
    CHECK(style->background_color[1]
          == doctest::Approx(args.style.background_color[1]));
    CHECK(style->text_color[0]
          == doctest::Approx(args.style.text_color[0]));
    CHECK(style->text_color[1]
          == doctest::Approx(args.style.text_color[1]));
    CHECK(HasStyleOverride(style->overrides,
                           WidgetsNS::ButtonStyleOverrideField::BackgroundColor));
    CHECK(HasStyleOverride(style->overrides,
                           WidgetsNS::ButtonStyleOverrideField::TextColor));

    auto theme = LoadActiveTheme(fx.space, SP::App::AppRootPathView{fx.app_root.getPath()});
    CHECK(style->typography.font_size
          == doctest::Approx(theme.button.typography.font_size));
}

TEST_CASE("WidgetDescriptor reproduces slider bucket") {
    DeclarativeFixture fx;
    Slider::Args args{};
    args.minimum = 0.0f;
    args.maximum = 2.0f;
    args.value = 1.0f;
    auto slider = Slider::Create(fx.space, fx.parent_view(), "descriptor_slider", args);
    REQUIRE(slider.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *slider);
    REQUIRE(descriptor.has_value());
    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());

    auto theme = LoadActiveTheme(fx.space, SP::App::AppRootPathView{fx.app_root.getPath()});
    auto style = theme.slider;
    auto state = fx.space.read<WidgetsNS::SliderState, std::string>((*slider).getPath()
                                                                    + "/state");
    REQUIRE(state.has_value());
    auto range = fx.space.read<WidgetsNS::SliderRange, std::string>((*slider).getPath()
                                                                    + "/meta/range");
    REQUIRE(range.has_value());
    WidgetsNS::SliderPreviewOptions preview{};
    preview.authoring_root = slider->getPath();
    auto reference = WidgetsNS::BuildSliderPreview(style, *range, *state, preview);

    auto const& descriptor_slider = std::get<SliderDescriptor>(descriptor->data);
    CHECK(descriptor_slider.style.fill_color[0] == doctest::Approx(style.fill_color[0]));

    REQUIRE_EQ(bucket->command_payload.size(), reference.command_payload.size());
    for (std::size_t i = 0; i < bucket->command_payload.size(); ++i) {
        CHECK(bucket->command_payload[i]
              == doctest::Approx(reference.command_payload[i]).epsilon(1e-5f));
    }
    CHECK(bucket->drawable_ids == reference.drawable_ids);
}

TEST_CASE("WidgetDescriptor reproduces list bucket") {
    DeclarativeFixture fx;
    List::Args args{};
    args.items.push_back({.id = "alpha", .label = "Alpha"});
    args.items.push_back({.id = "beta", .label = "Beta"});
    auto list = List::Create(fx.space, fx.parent_view(), "descriptor_list", args);
    REQUIRE(list.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *list);
    REQUIRE(descriptor.has_value());
    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());

    auto theme = LoadActiveTheme(fx.space, SP::App::AppRootPathView{fx.app_root.getPath()});
    auto style = theme.list;
    auto state = fx.space.read<WidgetsNS::ListState, std::string>((*list).getPath()
                                                                  + "/state");
    REQUIRE(state.has_value());
    auto items =
        fx.space.read<std::vector<WidgetsNS::ListItem>, std::string>((*list).getPath()
                                                                     + "/meta/items");
    REQUIRE(items.has_value());
    WidgetsNS::ListPreviewOptions preview{};
    preview.authoring_root = list->getPath();
    auto item_span = std::span<WidgetsNS::ListItem const>{items->data(), items->size()};
    auto reference = WidgetsNS::BuildListPreview(style, item_span, *state, preview);

    auto const& descriptor_list = std::get<ListDescriptor>(descriptor->data);
    CHECK(descriptor_list.style.item_selected_color[0] == doctest::Approx(style.item_selected_color[0]));

    if (bucket->command_counts != reference.bucket.command_counts) {
        INFO("list counts mismatch bucket=" << bucket->command_counts.size()
                                            << " ref=" << reference.bucket.command_counts.size());
    }
    if (bucket->drawable_ids != reference.bucket.drawable_ids) {
        INFO("list drawable ids mismatch bucket=" << bucket->drawable_ids.size()
                                                  << " ref=" << reference.bucket.drawable_ids.size());
    }
    CHECK(bucket->command_counts == reference.bucket.command_counts);
    CHECK(bucket->drawable_ids == reference.bucket.drawable_ids);
}

TEST_CASE("Declarative focus metadata mirrors window and widget state") {
    DeclarativeFixture fx;
    auto scene = SP::Scene::Create(fx.space, fx.app_root, fx.window_path);
    REQUIRE(scene.has_value());
    struct SceneCleanup {
        PathSpace& space;
        SP::UI::ScenePath path;
        ~SceneCleanup() {
            (void)SP::Scene::Shutdown(space, path);
        }
    } scene_cleanup{fx.space, scene->path};

    auto button = Button::Create(fx.space,
                                 fx.parent_view(),
                                 "focus_button",
                                 Button::Args{.label = "Primary"});
    REQUIRE(button.has_value());

    Slider::Args slider_args{};
    slider_args.minimum = 0.0f;
    slider_args.maximum = 10.0f;
    slider_args.value = 5.0f;
    auto slider = Slider::Create(fx.space, fx.parent_view(), "focus_slider", slider_args);
    REQUIRE(slider.has_value());

    auto config = Runtime::Widgets::Focus::MakeConfig(SP::App::AppRootPathView{fx.app_root.getPath()});

    auto set_button = Runtime::Widgets::Focus::Set(fx.space, config, *button);
    if (!set_button) {
        FAIL_CHECK(set_button.error().message.value_or("focus set failed"));
    }
    REQUIRE(set_button.has_value());
    CHECK(set_button->changed);

    auto button_order = fx.space.read<std::uint32_t, std::string>((*button).getPath()
                                                                  + "/focus/order");
    REQUIRE(button_order.has_value());
    auto slider_order = fx.space.read<std::uint32_t, std::string>((*slider).getPath()
                                                                  + "/focus/order");
    REQUIRE(slider_order.has_value());
    CHECK_NE(*button_order, *slider_order);

    auto read_focus_flag = [&](std::string const& path) {
        auto value = fx.space.read<bool, std::string>(path);
        if (!value) {
            auto code = value.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
                return false;
            }
            FAIL_CHECK(value.error().message.value_or("focus flag read failed"));
            return false;
        }
        return *value;
    };

    CHECK(read_focus_flag((*button).getPath() + "/focus/current"));
    CHECK_FALSE(read_focus_flag((*slider).getPath() + "/focus/current"));

    auto focus_path = std::string(scene->path.getPath())
                     + "/structure/window/" + fx.window_name + "/focus/current";
    auto window_focus = fx.space.read<std::string, std::string>(focus_path);
    REQUIRE(window_focus.has_value());
    CHECK_EQ(*window_focus, button->getPath());

    auto move_forward = Runtime::Widgets::Focus::Move(
        fx.space,
        config,
        Runtime::Widgets::Focus::Direction::Forward);
    REQUIRE(move_forward);
    REQUIRE(move_forward->has_value());
    CHECK_EQ(move_forward->value().widget.getPath(), slider->getPath());

    CHECK(read_focus_flag((*slider).getPath() + "/focus/current"));
    window_focus = fx.space.read<std::string, std::string>(focus_path);
    REQUIRE(window_focus.has_value());
    CHECK_EQ(*window_focus, slider->getPath());

    auto cleared = Runtime::Widgets::Focus::Clear(fx.space, config);
    REQUIRE(cleared);
    CHECK(*cleared);
    CHECK_FALSE(read_focus_flag((*slider).getPath() + "/focus/current"));
    window_focus = fx.space.read<std::string, std::string>(focus_path);
    REQUIRE(window_focus.has_value());
    CHECK(window_focus->empty());

}

TEST_CASE("WidgetDescriptor reproduces input field bucket with theme defaults") {
    DeclarativeFixture fx;
    InputField::Args args{};
    args.text = "Hello declarative";
    args.placeholder = "Type here";
    auto input = InputField::Create(fx.space, fx.parent_view(), "descriptor_input", args);
    REQUIRE(input.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *input);
    REQUIRE(descriptor.has_value());
    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());

    auto const& data = std::get<InputFieldDescriptor>(descriptor->data);
    auto reference = DetailNS::build_text_field_bucket(data.style,
                                                       data.state,
                                                       input->getPath(),
                                                       true);
    CHECK(bucket->drawable_ids == reference.drawable_ids);
    CHECK(bucket->command_payload == reference.command_payload);
}

TEST_CASE("WidgetDescriptor publishes stack layout metadata and preview bucket") {
    DeclarativeFixture fx;
    Stack::Args args{};
    args.active_panel = "first";
    args.style.axis = WidgetsNS::StackAxis::Vertical;
    args.style.spacing = 4.0f;
    args.panels.push_back(Stack::Panel{
        .id = "first",
        .fragment = Label::Fragment(Label::Args{.text = "Panel A"}),
    });
    args.panels.push_back(Stack::Panel{
        .id = "second",
        .fragment = Label::Fragment(Label::Args{.text = "Panel B"}),
    });
    auto stack = Stack::Create(fx.space, fx.parent_view(), "descriptor_stack", std::move(args));
    REQUIRE(stack.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *stack);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<StackDescriptor>(descriptor->data);
    CHECK_EQ(data.active_panel, "first");
    REQUIRE_EQ(data.panels.size(), 2U);
    CHECK(data.panels.front().visible);
    CHECK_FALSE(data.panels.back().visible);
    CHECK_EQ(data.style.axis, WidgetsNS::StackAxis::Vertical);
    CHECK_EQ(data.layout.children.size(), 2U);
    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());
    CHECK_FALSE(bucket->drawable_ids.empty());
}

TEST_CASE("Stack::SetActivePanel rewrites visibility metadata") {
    DeclarativeFixture fx;
    Stack::Args args{};
    args.active_panel = "alpha";
    args.panels.push_back(Stack::Panel{
        .id = "alpha",
        .fragment = Label::Fragment(Label::Args{.text = "Alpha"}),
    });
    args.panels.push_back(Stack::Panel{
        .id = "beta",
        .fragment = Label::Fragment(Label::Args{.text = "Beta"}),
    });
    auto stack = Stack::Create(fx.space, fx.parent_view(), "visibility_stack", std::move(args));
    REQUIRE(stack.has_value());

    auto alpha_visible = fx.space.read<bool, std::string>(stack->getPath() + "/panels/alpha/visible");
    auto beta_visible = fx.space.read<bool, std::string>(stack->getPath() + "/panels/beta/visible");
    REQUIRE(alpha_visible.has_value());
    REQUIRE(beta_visible.has_value());
    CHECK(*alpha_visible);
    CHECK_FALSE(*beta_visible);

    auto switched = Stack::SetActivePanel(fx.space, *stack, "beta");
    REQUIRE(switched.has_value());
    alpha_visible = fx.space.read<bool, std::string>(stack->getPath() + "/panels/alpha/visible");
    beta_visible = fx.space.read<bool, std::string>(stack->getPath() + "/panels/beta/visible");
    REQUIRE(alpha_visible.has_value());
    REQUIRE(beta_visible.has_value());
    CHECK_FALSE(*alpha_visible);
    CHECK(*beta_visible);
}

TEST_CASE("PaintSurface descriptor captures brush metadata") {
    DeclarativeFixture fx;
    PaintSurface::Args args{};
    args.brush_size = 12.0f;
    args.brush_color = {1.0f, 0.25f, 0.1f, 1.0f};
    auto paint = PaintSurface::Create(fx.space, fx.parent_view(), "descriptor_paint", args);
    REQUIRE(paint.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *paint);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<PaintSurfaceDescriptor>(descriptor->data);
    CHECK_EQ(data.gpu_enabled, false);
    CHECK_EQ(data.brush_size, doctest::Approx(12.0f));
    CHECK_EQ(data.brush_color[0], doctest::Approx(1.0f));
    CHECK_EQ(data.brush_color[1], doctest::Approx(0.25f));
    CHECK_EQ(data.brush_color[2], doctest::Approx(0.1f));
    CHECK_EQ(data.brush_color[3], doctest::Approx(1.0f));

    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());
    CHECK_FALSE(bucket->drawable_ids.empty());
}

TEST_CASE("PaintSurface bucket includes buffer background before strokes") {
    DeclarativeFixture fx;
    PaintSurface::Args args{};
    auto paint = PaintSurface::Create(fx.space, fx.parent_view(), "background_paint", args);
    REQUIRE(paint.has_value());

    auto descriptor = LoadWidgetDescriptor(fx.space, *paint);
    REQUIRE(descriptor.has_value());
    auto bucket = BuildWidgetBucket(fx.space, *descriptor);
    REQUIRE(bucket.has_value());
    REQUIRE_FALSE(bucket->drawable_ids.empty());
    REQUIRE_FALSE(bucket->command_kinds.empty());
    CHECK(bucket->command_kinds.front()
          == static_cast<std::uint32_t>(SP::UI::Scene::DrawCommandKind::RoundedRect));
}

TEST_CASE("PaintSurfaceRuntime marks GPU state and dirty hints") {
    DeclarativeFixture fx;
    PaintSurface::Args args{};
    args.gpu_enabled = true;
    auto paint = PaintSurface::Create(fx.space, fx.parent_view(), "gpu_paint", args);
    REQUIRE(paint.has_value());

    SP::UI::Declarative::Reducers::WidgetAction action{};
    action.widget_path = paint->getPath();
    action.kind = SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::PaintStrokeBegin;
    action.target_id = "paint_surface/stroke/1";
    auto pointer = SP::UI::Runtime::Widgets::Bindings::PointerInfo{};
    pointer.WithLocal(48.0f, 24.0f);
    action.pointer = pointer;

    auto handled = PaintRuntime::HandleAction(fx.space, action);
    REQUIRE(handled.has_value());
    CHECK(handled.value());

    auto state_path = std::string(paint->getPath()) + "/render/gpu/state";
    auto gpu_state = fx.space.read<std::string, std::string>(state_path);
    REQUIRE(gpu_state.has_value());
    CHECK_EQ(*gpu_state, "DirtyPartial");

    auto pending_path = std::string(paint->getPath()) + "/render/buffer/pendingDirty";
    auto pending = DetailNS::read_optional<std::vector<SP::UI::Runtime::DirtyRectHint>>(fx.space, pending_path);
    REQUIRE(pending.has_value());
    REQUIRE(pending->has_value());
    CHECK_FALSE((*pending)->empty());
}

TEST_CASE("Widgets::Move relocates widget and preserves handlers") {
    DeclarativeFixture fx;

    auto list_a = List::Create(fx.space, fx.parent_view(), "list_a", List::Args{});
    REQUIRE(list_a.has_value());
    auto list_b = List::Create(fx.space, fx.parent_view(), "list_b", List::Args{});
    REQUIRE(list_b.has_value());

    Label::Args label_args{};
    label_args.text = "Alpha";
    label_args.on_activate = [](LabelContext&) {};

    auto child = Label::Create(fx.space,
                               SP::App::ConcretePathView{list_a->getPath()},
                               "child_one",
                               label_args);
    REQUIRE(child.has_value());

    auto original_binding = fx.space.read<HandlerBinding, std::string>(child->getPath() + "/events/activate/handler");
    REQUIRE(original_binding.has_value());

    auto moved = Move(fx.space,
                      *child,
                      SP::App::ConcretePathView{list_b->getPath()},
                      "moved_child");
    REQUIRE_MESSAGE(moved.has_value(), SP::describeError(moved.error()));

    auto new_path = moved->getPath();
    auto text = fx.space.read<std::string, std::string>(new_path + "/state/text");
    REQUIRE(text.has_value());
    CHECK_EQ(*text, "Alpha");

    auto binding = fx.space.read<HandlerBinding, std::string>(new_path + "/events/activate/handler");
    REQUIRE(binding.has_value());
    CHECK(binding->registry_key != original_binding->registry_key);

    auto dirty = fx.space.read<bool, std::string>(new_path + "/render/dirty");
    REQUIRE(dirty.has_value());
    CHECK(*dirty);

    auto old_children = fx.space.listChildren(SP::ConcretePathStringView{list_a->getPath() + "/children"});
    CHECK(std::find(old_children.begin(), old_children.end(), "child_one") == old_children.end());
}

TEST_CASE("Widgets::Move rejects duplicate destinations") {
    DeclarativeFixture fx;
    auto first = Button::Create(fx.space, fx.parent_view(), "first_button", Button::Args{.label = "First"});
    REQUIRE(first.has_value());
    auto second = Button::Create(fx.space, fx.parent_view(), "second_button", Button::Args{.label = "Second"});
    REQUIRE(second.has_value());

    auto result = Move(fx.space,
                       *first,
                       fx.parent_view(),
                       "second_button");
    REQUIRE_FALSE(result.has_value());
    CHECK_EQ(result.error().code, Error::Code::InvalidPath);
}

TEST_CASE("Widget fragments register handlers during mount") {
    DeclarativeFixture fx;
    Button::Args args{};
    bool invoked = false;
    args.on_press = [&](ButtonContext&) { invoked = true; };
    auto fragment = Button::Fragment(std::move(args));
    auto mounted = Widgets::Mount(fx.space, fx.parent_view(), "fragment_button", fragment);
    REQUIRE(mounted.has_value());

    auto binding = fx.space.read<HandlerBinding, std::string>(mounted->getPath() + "/events/press/handler");
    REQUIRE(binding.has_value());
    auto handler = Handlers::Read(fx.space, *mounted, "press");
    REQUIRE(handler);
    REQUIRE(handler->has_value());
    auto button_handler = std::get_if<ButtonHandler>(&handler->value());
    REQUIRE(button_handler != nullptr);

    ButtonContext ctx{fx.space, *mounted};
    (*button_handler)(ctx);
    CHECK(invoked);
}

TEST_CASE("Handler helpers replace, wrap, and restore callbacks") {
    DeclarativeFixture fx;
    bool base_called = false;
    Button::Args args{};
    args.on_press = [&](ButtonContext&) { base_called = true; };
    auto button = Button::Create(fx.space, fx.parent_view(), "handler_button", std::move(args));
    REQUIRE(button.has_value());

    bool override_called = false;
    HandlerVariant override_handler = ButtonHandler{[&](ButtonContext&) { override_called = true; }};
    auto replace_token = Handlers::Replace(fx.space,
                                           *button,
                                           "press",
                                           HandlerKind::ButtonPress,
                                           std::move(override_handler));
    REQUIRE(replace_token.has_value());

    auto binding = fx.space.read<HandlerBinding, std::string>(button->getPath() + "/events/press/handler");
    REQUIRE(binding.has_value());
    auto handler = Handlers::Read(fx.space, *button, "press");
    REQUIRE(handler);
    REQUIRE(handler->has_value());
    ButtonContext ctx{fx.space, *button};
    std::get<ButtonHandler>(handler->value())(ctx);
    CHECK(override_called);
    CHECK_FALSE(base_called);

    REQUIRE(Handlers::Restore(fx.space, *replace_token));

    auto restored = fx.space.read<HandlerBinding, std::string>(button->getPath() + "/events/press/handler");
    REQUIRE(restored.has_value());
    auto restored_handler = Handlers::Read(fx.space, *button, "press");
    REQUIRE(restored_handler);
    REQUIRE(restored_handler->has_value());
    std::get<ButtonHandler>(restored_handler->value())(ctx);
    CHECK(base_called);

    auto label = Label::Create(fx.space, fx.parent_view(), "handler_label", Label::Args{.text = "Plain"});
    REQUIRE(label.has_value());

    bool wrapped_called = false;
    auto wrap_token = Handlers::Wrap(
        fx.space,
        *label,
        "activate",
        HandlerKind::LabelActivate,
        [&](HandlerVariant const& existing) {
            CHECK(std::holds_alternative<std::monostate>(existing));
            return HandlerVariant{LabelHandler{[&](LabelContext&) { wrapped_called = true; }}};
        });
    REQUIRE(wrap_token.has_value());

    auto label_binding = fx.space.read<HandlerBinding, std::string>(label->getPath() + "/events/activate/handler");
    REQUIRE(label_binding.has_value());
    auto label_handler = Handlers::Read(fx.space, *label, "activate");
    REQUIRE(label_handler);
    REQUIRE(label_handler->has_value());
    LabelContext label_ctx{fx.space, *label};
    std::get<LabelHandler>(label_handler->value())(label_ctx);
    CHECK(wrapped_called);

    REQUIRE(Handlers::Restore(fx.space, *wrap_token));
    auto missing = fx.space.read<HandlerBinding, std::string>(label->getPath() + "/events/activate/handler");
    REQUIRE_FALSE(missing.has_value());
    auto code = missing.error().code;
    bool expected_code = (code == Error::Code::NoObjectFound)
                         || (code == Error::Code::NoSuchPath);
    CHECK(expected_code);
}

TEST_CASE("Theme resolver uses inherited theme when child theme omits value") {
    DeclarativeFixture fx;
    SP::App::AppRootPathView app_root_view{fx.app_root.getPath()};

    auto parent_theme = WidgetsNS::MakeDefaultWidgetTheme();
    parent_theme.button.width = 512.0f;

    auto parent_paths = ThemeConfig::Ensure(fx.space,
                                            app_root_view,
                                            "parent_theme",
                                            parent_theme);
    REQUIRE(parent_paths.has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     parent_paths->value.getPath(),
                                     parent_theme)
                .has_value());

    auto inherits_path = SP::App::resolve_app_relative(app_root_view,
                                                       "config/theme/child_theme/style/inherits");
    REQUIRE(inherits_path.has_value());
    auto sanitized_parent = ThemeConfig::SanitizeName("parent_theme");
    REQUIRE(DetailNS::replace_single(fx.space,
                                     inherits_path->getPath(),
                                     sanitized_parent)
                .has_value());

    auto button = Button::Create(fx.space,
                                 fx.parent_view(),
                                 "theme_child_button",
                                 Button::Args{.label = "Child"});
    REQUIRE(button.has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     button->getPath() + "/style/theme",
                                     std::string{"child_theme"})
                .has_value());

    auto widget_theme_override = fx.space.read<std::string, std::string>(
        button->getPath() + "/style/theme");
    INFO("widget theme override present=" << widget_theme_override.has_value()
         << " value=" << (widget_theme_override ? *widget_theme_override : std::string{"<missing>"}));
    auto child_theme_value = fx.space.read<WidgetsNS::WidgetTheme, std::string>(
        std::string(fx.app_root.getPath()) + "/config/theme/child_theme/value");
    INFO("child_theme value present=" << child_theme_value.has_value());
    auto inherits_value = fx.space.read<std::string, std::string>(
        std::string(fx.app_root.getPath()) + "/config/theme/child_theme/style/inherits");
    INFO("inherits present=" << inherits_value.has_value()
         << " value=" << (inherits_value ? *inherits_value : std::string{"<missing>"}));

    auto descriptor = LoadWidgetDescriptor(fx.space, *button);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<ButtonDescriptor>(descriptor->data);
    CHECK(data.style.width == doctest::Approx(parent_theme.button.width));
}

TEST_CASE("Theme resolver detects inheritance cycles") {
    DeclarativeFixture fx;
    SP::App::AppRootPathView app_root_view{fx.app_root.getPath()};

    auto theme_a = WidgetsNS::MakeDefaultWidgetTheme();
    auto paths_a = ThemeConfig::Ensure(fx.space, app_root_view, "cycle_a", theme_a);
    REQUIRE(paths_a.has_value());
    auto theme_b = WidgetsNS::MakeDefaultWidgetTheme();
    auto paths_b = ThemeConfig::Ensure(fx.space, app_root_view, "cycle_b", theme_b);
    REQUIRE(paths_b.has_value());

    auto inherits_a = SP::App::resolve_app_relative(app_root_view,
                                                    "config/theme/cycle_a/style/inherits");
    auto inherits_b = SP::App::resolve_app_relative(app_root_view,
                                                    "config/theme/cycle_b/style/inherits");
    REQUIRE(inherits_a.has_value());
    REQUIRE(inherits_b.has_value());

    auto name_a = ThemeConfig::SanitizeName("cycle_a");
    auto name_b = ThemeConfig::SanitizeName("cycle_b");

    REQUIRE(DetailNS::replace_single(fx.space,
                                     inherits_a->getPath(),
                                     name_b)
                .has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     inherits_b->getPath(),
                                     name_a)
                .has_value());

    auto resolved = ThemeConfig::Resolve(app_root_view, name_a);
    REQUIRE(resolved.has_value());
    auto loaded = ThemeConfig::Load(fx.space, *resolved);
    REQUIRE_FALSE(loaded.has_value());
    CHECK_EQ(loaded.error().code, Error::Code::InvalidType);
}

} // namespace
