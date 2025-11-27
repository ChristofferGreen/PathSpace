#include "declarative_example_shared.hpp"

#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <system_error>

using namespace PathSpaceExamples;

namespace {

struct Options {
    int width = 960;
    int height = 600;
};

auto parse_options(int argc, char** argv) -> Options {
    Options opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("widgets_example_minimal");

    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});

    (void)cli.parse(argc, argv);

    opts.width = std::max(640, opts.width);
    opts.height = std::max(480, opts.height);
    return opts;
}

auto fail(SP::PathSpace& space, std::string_view message) -> int {
    std::cerr << "widgets_example_minimal: " << message << "\n";
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

int main(int argc, char** argv) {
    auto opts = parse_options(argc, argv);

    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        return fail(space, "failed to launch declarative runtime");
    }

    auto app = SP::App::Create(space,
                               "widgets_example_minimal",
                               {.title = "Declarative Widgets Minimal"});
    if (!app) {
        return fail(space, "failed to create app");
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "minimal_window";
    window_opts.title = "PathSpace Minimal";
    window_opts.width = opts.width;
    window_opts.height = opts.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        return fail(space, "failed to create window");
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "minimal_scene";
    scene_opts.description = "Declarative minimal example";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        return fail(space, "failed to create scene");
    }

    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    app_root_view,
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        return fail(space, "failed to prepare presenter bootstrap");
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "widgets_example_minimal");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "widgets_example_minimal");
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
                                                           "status",
                                                           {.text = "Adjust the slider"});
    if (!status_label) {
        return fail(space, "failed to create label");
    }

    auto slider_value = std::make_shared<float>(25.0f);
    SP::UI::Declarative::Slider::Args slider_args{};
    slider_args.minimum = 0.0f;
    slider_args.maximum = 100.0f;
    slider_args.value = *slider_value;
    slider_args.on_change = [slider_value,
                             status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx) {
        *slider_value = ctx.value;
        std::ostringstream stream;
        stream << "Slider value = " << std::fixed << std::setprecision(1) << ctx.value;
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto slider = SP::UI::Declarative::Slider::Create(space,
                                                       window_view,
                                                       "primary_slider",
                                                       slider_args);
    if (!slider) {
        return fail(space, "failed to create slider");
    }

    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Reset";
    button_args.on_press = [slider_value,
                            slider_path = *slider,
                            status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
        *slider_value = 25.0f;
        log_error(SP::UI::Declarative::Slider::SetValue(ctx.space, slider_path, *slider_value),
                  "Slider::SetValue");
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      "Slider reset to 25"),
                  "Label::SetText");
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                       window_view,
                                                       "reset_button",
                                                       button_args);
    if (!button) {
        return fail(space, "failed to create button");
    }

    auto readiness = ensure_declarative_scene_ready(space,
                                                    scene->path,
                                                    window->path,
                                                    window->view_name);
    if (!readiness) {
        std::cerr << "widgets_example_minimal: scene readiness failed: "
                  << SP::describeError(readiness.error()) << "\n";
        return fail(space, "scene readiness failed");
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    PresentLoopHooks hooks{};
    run_present_loop(space,
                     window->path,
                     window->view_name,
                     *present_handles,
                     opts.width,
                     opts.height,
                     hooks);

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
