#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetDetail.hpp>

#include <algorithm>
#include <string>
#include <span>

namespace {

using namespace SP;
using namespace SP::UI::Declarative;
namespace Builders = SP::UI::Builders;
namespace WidgetsNS = SP::UI::Builders::Widgets;
namespace DetailNS = SP::UI::Builders::Detail;

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
    SP::UI::Builders::WindowPath window_path;
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
        fx.space.read<SP::UI::Builders::Widgets::ButtonState, std::string>(button->getPath()
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
        fx.space.read<SP::UI::Builders::Widgets::SliderState, std::string>(
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

    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());

    auto style = fx.space.read<WidgetsNS::ButtonStyle, std::string>((*button).getPath()
                                                                    + "/meta/style");
    REQUIRE(style.has_value());
    auto state = fx.space.read<WidgetsNS::ButtonState, std::string>((*button).getPath()
                                                                    + "/state");
    REQUIRE(state.has_value());
    WidgetsNS::ButtonPreviewOptions preview{};
    preview.authoring_root = button->getPath();
    auto reference = WidgetsNS::BuildButtonPreview(*style, *state, preview);

    CHECK(bucket->drawable_ids == reference.drawable_ids);
    CHECK(bucket->command_payload == reference.command_payload);
    CHECK(bucket->command_kinds == reference.command_kinds);
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
    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());

    auto style = fx.space.read<WidgetsNS::SliderStyle, std::string>((*slider).getPath()
                                                                    + "/meta/style");
    REQUIRE(style.has_value());
    auto state = fx.space.read<WidgetsNS::SliderState, std::string>((*slider).getPath()
                                                                    + "/state");
    REQUIRE(state.has_value());
    auto range = fx.space.read<WidgetsNS::SliderRange, std::string>((*slider).getPath()
                                                                    + "/meta/range");
    REQUIRE(range.has_value());
    WidgetsNS::SliderPreviewOptions preview{};
    preview.authoring_root = slider->getPath();
    auto reference = WidgetsNS::BuildSliderPreview(*style, *range, *state, preview);

    CHECK(bucket->command_payload == reference.command_payload);
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
    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());

    auto style = fx.space.read<WidgetsNS::ListStyle, std::string>((*list).getPath()
                                                                  + "/meta/style");
    REQUIRE(style.has_value());
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
    auto reference = WidgetsNS::BuildListPreview(*style, item_span, *state, preview);

    CHECK(bucket->command_counts == reference.bucket.command_counts);
    CHECK(bucket->drawable_ids == reference.bucket.drawable_ids);
}

TEST_CASE("Declarative focus metadata mirrors window and widget state") {
    DeclarativeFixture fx;
    auto scene = SP::Scene::Create(fx.space, fx.app_root, fx.window_path);
    REQUIRE(scene.has_value());
    struct SceneCleanup {
        PathSpace& space;
        Builders::ScenePath path;
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

    auto config = Builders::Widgets::Focus::MakeConfig(SP::App::AppRootPathView{fx.app_root.getPath()});

    auto set_button = Builders::Widgets::Focus::Set(fx.space, config, *button);
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

    auto move_forward = Builders::Widgets::Focus::Move(
        fx.space,
        config,
        Builders::Widgets::Focus::Direction::Forward);
    REQUIRE(move_forward);
    REQUIRE(move_forward->has_value());
    CHECK_EQ(move_forward->value().widget.getPath(), slider->getPath());

    CHECK(read_focus_flag((*slider).getPath() + "/focus/current"));
    window_focus = fx.space.read<std::string, std::string>(focus_path);
    REQUIRE(window_focus.has_value());
    CHECK_EQ(*window_focus, slider->getPath());

    auto cleared = Builders::Widgets::Focus::Clear(fx.space, config);
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
    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());

    auto const& data = std::get<InputFieldDescriptor>(descriptor->data);
    auto reference = DetailNS::build_text_field_bucket(data.style,
                                                       data.state,
                                                       input->getPath(),
                                                       true);
    CHECK(bucket->drawable_ids == reference.drawable_ids);
    CHECK(bucket->command_payload == reference.command_payload);
}

TEST_CASE("WidgetDescriptor loads stack metadata even when bucket is empty") {
    DeclarativeFixture fx;
    Stack::Args args{};
    args.active_panel = "first";
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
    CHECK_EQ(data.panels.size(), 2U);
    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());
    CHECK(bucket->drawable_ids.empty());
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

    auto bucket = BuildWidgetBucket(*descriptor);
    REQUIRE(bucket.has_value());
    CHECK(bucket->drawable_ids.empty());
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

} // namespace
