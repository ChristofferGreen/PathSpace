#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <string>

namespace {

using namespace SP;
using namespace SP::UI::Declarative;

struct DeclarativeFixture {
    PathSpace space;
    DeclarativeFixture() {
        auto launch = SP::System::LaunchStandard(space);
        REQUIRE(launch.has_value());
        auto app = SP::App::Create(space, "test_app");
        REQUIRE(app.has_value());
        app_root = *app;
        SP::Window::CreateOptions window_opts{};
        window_opts.name = "main_window";
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

} // namespace
