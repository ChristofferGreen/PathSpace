#include "declarative_example_shared.hpp"

#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>

#include <algorithm>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace PathSpaceExamples;
namespace ScreenshotCli = SP::UI::Screenshot;

namespace {

struct CommandLineOptions {
    int width = 800;
    int height = 520;
    ScreenshotCli::DeclarativeScreenshotCliOptions screenshot;
};

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("declarative_hello_example");
    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});
    ScreenshotCli::RegisterDeclarativeScreenshotCliOptions(cli, opts.screenshot);

    (void)cli.parse(argc, argv);

    opts.width = std::max(640, opts.width);
    opts.height = std::max(480, opts.height);
    ScreenshotCli::ApplyDeclarativeScreenshotEnvOverrides(opts.screenshot);
    return opts;
}

auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "declarative_hello_example: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "declarative_hello_example: failed to launch runtime\n";
        return 1;
    }

    auto app = SP::App::Create(space, "declarative_hello", {.title = "Declarative Hello"});
    if (!app) {
        std::cerr << "declarative_hello_example: failed to create app\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "hello_window";
    window_opts.title = "Declarative Hello";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        std::cerr << "declarative_hello_example: failed to create window\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "hello_scene";
    scene_opts.description = "Hello button/list scene";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        std::cerr << "declarative_hello_example: failed to create scene\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    app_root_view,
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        std::cerr << "declarative_hello_example: failed to prepare presenter\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "declarative_hello_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "declarative_hello_example");
    auto pointer_devices = std::vector<std::string>{std::string{kPointerDevice}};
    auto keyboard_devices = std::vector<std::string>{std::string{kKeyboardDevice}};
    subscribe_window_devices(space,
                             window->path,
                             std::span<const std::string>(pointer_devices),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices));

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto status_label = SP::UI::Declarative::Label::Create(space,
                                                           window_view,
                                                           "hello_label",
                                                           {.text = "Tap the button or pick a greeting"});
    if (!status_label) {
        std::cerr << "declarative_hello_example: failed to create label\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Say Hello";
    button_args.on_press = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Hello from Declarative Widgets!"),
                  "Label::SetText");
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                       window_view,
                                                       "hello_button",
                                                       button_args);
    if (!button) {
        std::cerr << "declarative_hello_example: failed to create button\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    std::vector<SP::UI::Declarative::List::ListItem> greetings{
        {.id = "hola", .label = "Hola"},
        {.id = "bonjour", .label = "Bonjour"},
        {.id = "konnichiwa", .label = "Konnichiwa"},
    };
    SP::UI::Declarative::List::Args list_args{};
    list_args.items = greetings;
    list_args.on_child_event = [status_label_path = *status_label](SP::UI::Declarative::ListChildContext& ctx) {
        std::string text = "Selected greeting: " + ctx.child_id;
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      text),
                  "Label::SetText");
    };
    auto list = SP::UI::Declarative::List::Create(space,
                                                   window_view,
                                                   "greeting_list",
                                                   list_args);
    if (!list) {
        std::cerr << "declarative_hello_example: failed to create list\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto readiness = ensure_declarative_scene_ready(space,
                                                    scene->path,
                                                    window->path,
                                                    window->view_name);
    if (!readiness) {
        std::cerr << "declarative_hello_example: scene readiness failed: "
                  << SP::describeError(readiness.error()) << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    if (ScreenshotCli::DeclarativeScreenshotRequested(options.screenshot)) {
        auto pose = [&]() -> SP::Expected<void> {
            if (!status_label) {
                return SP::Expected<void>{};
            }
            return SP::UI::Declarative::Label::SetText(space,
                                                       *status_label,
                                                       "Screenshot capture ready");
        };
        auto capture = ScreenshotCli::CaptureDeclarativeScreenshotIfRequested(space,
                                                                              scene->path,
                                                                              window->path,
                                                                              window->view_name,
                                                                              options.width,
                                                                              options.height,
                                                                              "declarative_hello_example",
                                                                              options.screenshot,
                                                                              pose);
        if (!capture) {
            std::cerr << "declarative_hello_example: screenshot capture failed: "
                      << SP::describeError(capture.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    PresentLoopHooks hooks{};
    run_present_loop(space,
                     window->path,
                     window->view_name,
                     *present_handles,
                     options.width,
                     options.height,
                     hooks);

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
