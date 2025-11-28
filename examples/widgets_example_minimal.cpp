#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 600;
constexpr std::string_view kAppName = "widgets_example_minimal";

auto fail(SP::PathSpace& space,
          std::string_view message,
          std::optional<SP::Error> error = std::nullopt) -> int {
    std::cerr << "widgets_example_minimal: " << message;
    if (error) {
        std::cerr << ": " << SP::describeError(*error);
    }
    std::cerr << "\n";
    SP::System::ShutdownDeclarativeRuntime(space);
    return 1;
}

auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "widgets_example_minimal: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

} // namespace

int main() {
    SP::PathSpace space;

    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        return fail(space, "failed to launch declarative runtime", launch.error());
    }

    auto app = SP::App::Create(space,
                               kAppName,
                               {.title = "Declarative Widgets Minimal"});
    if (!app) {
        return fail(space, "failed to create app", app.error());
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    auto window = SP::Window::Create(space,
                                     app_root,
                                     "Declarative Widgets Minimal",
                                     kWindowWidth,
                                     kWindowHeight);
    if (!window) {
        return fail(space, "failed to create window", window.error());
    }

    SP::Scene::CreateOptions scene_options{};
    scene_options.name = "minimal_scene";
    scene_options.description = "Plan doc sample";
    scene_options.view = window->view_name;
    auto scene = SP::Scene::Create(space, app_root, window->path, scene_options);
    if (!scene) {
        return fail(space, "failed to create scene", scene.error());
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    if (!surface_rel) {
        return fail(space, "failed to read surface binding", surface_rel.error());
    }
    auto surface_abs = SP::App::resolve_app_relative(app_root_view, *surface_rel);
    if (!surface_abs) {
        return fail(space, "failed to resolve surface path", surface_abs.error());
    }
    auto bind_scene = SP::UI::Surface::SetScene(space,
                                                SP::UI::SurfacePath{surface_abs->getPath()},
                                                scene->path);
    if (!bind_scene) {
        return fail(space, "failed to bind surface scene", bind_scene.error());
    }

    using namespace SP::UI::Declarative;

    auto status_label = Label::Create(space,
                                      window_view,
                                      "status_label",
                                      "Tap the button or pick a greeting");
    if (!status_label) {
        return fail(space, "failed to create label", status_label.error());
    }

    Button::Args button_args{};
    button_args.label = "Say Hello!";
    button_args.on_press = [status_label_path = *status_label](ButtonContext& ctx) {
        log_error(Label::SetText(ctx.space,
                                 status_label_path,
                                 "Hello from PathSpace!"),
                  "Label::SetText");
        std::cout << "Hello button pressed" << std::endl;
    };
    auto hello_button = Button::Create(space,
                                       window_view,
                                       "hello_button",
                                       button_args);
    if (!hello_button) {
        return fail(space, "failed to create button", hello_button.error());
    }

    List::Args list_args{};
    list_args.items = {
        {.id = "hello", .label = "Hello"},
        {.id = "bonjour", .label = "Bonjour"},
        {.id = "konnichiwa", .label = "Konnichiwa"},
    };
    list_args.on_child_event = [status_label_path = *status_label](ListChildContext& ctx) {
        auto text = std::string{"Selected greeting: "} + ctx.child_id;
        log_error(Label::SetText(ctx.space, status_label_path, text), "Label::SetText");
    };
    list_args.children = {
        {"hello_fragment", Label::Fragment(Label::Args{.text = "Hello from ListArgs"})},
        {"bonus_button",
         Button::Fragment(Button::Args{
             .label = "Bonus",
             .on_press = [](ButtonContext&) {
                 std::cout << "Bonus button clicked!" << std::endl;
             },
         })},
    };
    auto greetings_list = List::Create(space,
                                       window_view,
                                       "greetings_list",
                                       list_args);
    if (!greetings_list) {
        return fail(space, "failed to create list", greetings_list.error());
    }

    auto list_view = SP::App::ConcretePathView{greetings_list->getPath()};
    auto hi_item = Label::Create(space, list_view, "hi_item", "Hi there");
    if (!hi_item) {
        return fail(space, "failed to add list child", hi_item.error());
    }

    auto run = SP::App::RunUI(space,
                              *scene,
                              *window,
                              {.window_width = kWindowWidth,
                               .window_height = kWindowHeight,
                               .window_title = "Declarative Widgets Minimal"});
    if (!run) {
        return fail(space, "App::RunUI failed", run.error());
    }

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
