#include "declarative_example_shared.hpp"

#include <pathspace/examples/paint/PaintControls.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>

#include <algorithm>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <cstdlib>
#include <system_error>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace PathSpaceExamples;
namespace ScreenshotCli = SP::UI::Screenshot;
namespace PaintControlsNS = SP::Examples::PaintControls;
using PaintControlsNS::BrushState;

namespace {

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool headless = false;
    ScreenshotCli::DeclarativeScreenshotCliOptions screenshot;
};

auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("widgets_example");

    cli.add_flag("--headless", {.on_set = [&] { opts.headless = true; }});
    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});
    ScreenshotCli::RegisterDeclarativeScreenshotCliOptions(cli, opts.screenshot);

    (void)cli.parse(argc, argv);

    opts.width = std::max(640, opts.width);
    opts.height = std::max(480, opts.height);
    ScreenshotCli::ApplyDeclarativeScreenshotEnvOverrides(opts.screenshot);
    if (ScreenshotCli::DeclarativeScreenshotRequested(opts.screenshot)) {
        opts.headless = true;
    }
    return opts;
}

auto make_device_list(std::string const& device_path) -> std::vector<std::string> {
    return {device_path};
}

auto log_error(SP::Expected<void> const& status, std::string const& context) -> void {
    if (status) {
        return;
    }
    auto const& error = status.error();
    std::cerr << "widgets_example: " << context << " failed";
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
        std::cerr << "widgets_example: failed to launch declarative runtime\n";
        return 1;
    }

    auto app = SP::App::Create(space,
                               "widgets_example",
                               {.title = "Declarative Widgets Gallery"});
    if (!app) {
        std::cerr << "widgets_example: failed to create app\n";
        return 1;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};
    auto theme_selection = SP::UI::Runtime::Widgets::LoadTheme(space, app_root_view, "");
    if (!theme_selection) {
        std::cerr << "widgets_example: failed to load theme\n";
        return 1;
    }
    auto active_theme = theme_selection->theme;

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "gallery_window";
    window_opts.title = "PathSpace Declarative Widgets";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        std::cerr << "widgets_example: failed to create window\n";
        return 1;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "gallery_scene";
    scene_opts.description = "Declarative widgets gallery";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        std::cerr << "widgets_example: failed to create scene\n";
        return 1;
    }

    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    app_root_view,
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        std::cerr << "widgets_example: failed to prepare presenter bootstrap\n";
        return 1;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    ensure_device_push_config(space, std::string{kPointerDevice}, "widgets_example");
    ensure_device_push_config(space, std::string{kKeyboardDevice}, "widgets_example");
    auto pointer_devices = make_device_list(std::string{kPointerDevice});
    auto keyboard_devices = make_device_list(std::string{kKeyboardDevice});
    subscribe_window_devices(space,
                             window->path,
                             std::span<const std::string>(pointer_devices),
                             std::span<const std::string>{},
                             std::span<const std::string>(keyboard_devices));

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto status_label = SP::UI::Declarative::Label::Create(space,
                                                           window_view,
                                                           "status_label",
                                                           {.text = "Ready"});
    if (!status_label) {
        std::cerr << "widgets_example: failed to create status label\n";
        return 1;
    }

    auto button_counter = std::make_shared<int>(0);
    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Press Me";
    button_args.on_press = [button_counter,
                            status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx) {
        ++(*button_counter);
        std::ostringstream stream;
        stream << "Button pressed " << *button_counter << " time(s)";
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto button = SP::UI::Declarative::Button::Create(space,
                                                       window_view,
                                                       "primary_button",
                                                       button_args);
    if (!button) {
        std::cerr << "widgets_example: failed to create button\n";
        return 1;
    }

    SP::UI::Declarative::Toggle::Args toggle_args{};
    toggle_args.on_toggle = [status_label_path = *status_label](SP::UI::Declarative::ToggleContext& ctx) {
        auto message = ctx.widget.getPath();
        std::ostringstream stream;
        stream << "Toggle state changed for " << message;
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto toggle = SP::UI::Declarative::Toggle::Create(space,
                                                       window_view,
                                                       "demo_toggle",
                                                       toggle_args);
    if (!toggle) {
        std::cerr << "widgets_example: failed to create toggle\n";
        return 1;
    }

    SP::UI::Declarative::Slider::Args slider_args{};
    slider_args.minimum = 0.0f;
    slider_args.maximum = 100.0f;
    slider_args.value = 35.0f;
    slider_args.on_change = [status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx) {
        std::ostringstream stream;
        stream << "Slider value = " << std::fixed << std::setprecision(1) << ctx.value;
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto slider = SP::UI::Declarative::Slider::Create(space,
                                                       window_view,
                                                       "gallery_slider",
                                                       slider_args);
    if (!slider) {
        std::cerr << "widgets_example: failed to create slider\n";
        return 1;
    }

    std::vector<SP::UI::Declarative::List::ListItem> list_items{
        {.id = "alpha", .label = "Alpha"},
        {.id = "beta", .label = "Beta"},
        {.id = "gamma", .label = "Gamma"},
    };
    SP::UI::Declarative::List::Args list_args{};
    list_args.items = list_items;
    list_args.on_child_event = [status_label_path = *status_label](SP::UI::Declarative::ListChildContext& ctx) {
        std::ostringstream stream;
        stream << "List event from child '" << ctx.child_id << "'";
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto list = SP::UI::Declarative::List::Create(space,
                                                   window_view,
                                                   "scenario_list",
                                                   list_args);
    if (!list) {
        std::cerr << "widgets_example: failed to create list\n";
        return 1;
    }

    std::vector<SP::UI::Declarative::Tree::TreeNode> tree_nodes;
    SP::UI::Declarative::Tree::TreeNode settings{};
    settings.id = "settings";
    settings.label = "Settings";
    settings.expandable = true;
    tree_nodes.push_back(settings);
    SP::UI::Declarative::Tree::TreeNode input{};
    input.id = "input";
    input.parent_id = "settings";
    input.label = "Input";
    tree_nodes.push_back(input);
    SP::UI::Declarative::Tree::TreeNode display{};
    display.id = "display";
    display.parent_id = "settings";
    display.label = "Display";
    tree_nodes.push_back(display);
    SP::UI::Declarative::Tree::TreeNode about{};
    about.id = "about";
    about.label = "About";
    tree_nodes.push_back(about);
    SP::UI::Declarative::Tree::Args tree_args{};
    tree_args.nodes = tree_nodes;
    tree_args.on_node_event = [status_label_path = *status_label](SP::UI::Declarative::TreeNodeContext& ctx) {
        std::ostringstream stream;
        stream << "Tree node event for '" << ctx.node_id << "'";
        log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                      status_label_path,
                                                      stream.str()),
                  "Label::SetText");
    };
    auto tree = SP::UI::Declarative::Tree::Create(space,
                                                   window_view,
                                                   "navigation_tree",
                                                   tree_args);
    if (!tree) {
        std::cerr << "widgets_example: failed to create tree\n";
        return 1;
    }

    auto paint_controls_layout = PaintControlsNS::ComputeLayoutMetrics(options.width, options.height);
    auto gallery_brush_state = std::make_shared<BrushState>();

    PaintControlsNS::BrushSliderConfig gallery_slider_config{
        .layout = paint_controls_layout,
        .brush_state = gallery_brush_state,
        .minimum = 1.0f,
        .maximum = 64.0f,
        .step = 1.0f,
        .on_change = [status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx, float value) {
            std::ostringstream stream;
            stream << "Brush slider demo = " << std::fixed << std::setprecision(1) << value;
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          status_label_path,
                                                          stream.str()),
                      "Label::SetText");
        },
    };

    auto palette_entries = PaintControlsNS::BuildDefaultPaletteEntries(active_theme);
    PaintControlsNS::PaletteComponentConfig palette_config{
        .layout = paint_controls_layout,
        .theme = active_theme,
        .entries = std::span<const PaintControlsNS::PaletteEntry>(palette_entries.data(), palette_entries.size()),
        .brush_state = gallery_brush_state,
        .on_select = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx,
                                                        PaintControlsNS::PaletteEntry const& entry) {
            std::ostringstream stream;
            stream << "Palette demo selected " << entry.label;
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          status_label_path,
                                                          stream.str()),
                      "Label::SetText");
        },
    };

    PaintControlsNS::HistoryActionsConfig history_config{
        .layout = paint_controls_layout,
        .on_action = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx,
                                                        PaintControlsNS::HistoryAction action) {
            std::ostringstream stream;
            stream << (action == PaintControlsNS::HistoryAction::Undo ? "Undo" : "Redo")
                   << " demo action";
            log_error(SP::UI::Declarative::Label::SetText(ctx.space,
                                                          status_label_path,
                                                          stream.str()),
                      "Label::SetText");
        },
        .undo_label = "Undo Demo",
        .redo_label = "Redo Demo",
    };

    SP::UI::Declarative::Stack::Args paint_controls_stack{};
    paint_controls_stack.style.axis = SP::UI::Runtime::Widgets::StackAxis::Vertical;
    paint_controls_stack.style.spacing = std::max(10.0f, paint_controls_layout.controls_spacing * 0.5f);
    paint_controls_stack.style.padding_main_start = paint_controls_layout.controls_padding_main;
    paint_controls_stack.style.padding_main_end = paint_controls_layout.controls_padding_main;
    paint_controls_stack.style.padding_cross_start = paint_controls_layout.controls_padding_cross;
    paint_controls_stack.style.padding_cross_end = paint_controls_layout.controls_padding_cross;
    paint_controls_stack.style.width = std::min(420.0f, paint_controls_layout.controls_width);

    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "demo_brush_slider",
        .fragment = PaintControlsNS::BuildBrushSliderFragment(gallery_slider_config),
    });
    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "demo_palette",
        .fragment = PaintControlsNS::BuildPaletteFragment(palette_config),
    });
    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "demo_history",
        .fragment = PaintControlsNS::BuildHistoryActionsFragment(history_config),
    });
    PaintControlsNS::EnsureActivePanel(paint_controls_stack);

    auto paint_controls = SP::UI::Declarative::Stack::Create(space,
                                                             window_view,
                                                             "paint_controls_gallery",
                                                             std::move(paint_controls_stack));
    if (!paint_controls) {
        std::cerr << "widgets_example: failed to create paint controls demo\n";
        return 1;
    }

    auto readiness = ensure_declarative_scene_ready(space,
                                                    scene->path,
                                                    window->path,
                                                    window->view_name);
    if (!readiness) {
        std::cerr << "widgets_example: scene readiness failed: "
                  << SP::describeError(readiness.error()) << "\n";
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    if (ScreenshotCli::DeclarativeScreenshotRequested(options.screenshot)) {
        auto pose = [&]() -> SP::Expected<void> {
            if (slider) {
                auto set_slider = SP::UI::Declarative::Slider::SetValue(space, *slider, 60.0f);
                if (!set_slider) {
                    return set_slider;
                }
            }
            if (toggle) {
                auto set_toggle = SP::UI::Declarative::Toggle::SetChecked(space, *toggle, true);
                if (!set_toggle) {
                    return set_toggle;
                }
            }
            if (status_label) {
                auto set_label = SP::UI::Declarative::Label::SetText(space,
                                                                     *status_label,
                                                                     "Screenshot capture ready");
                if (!set_label) {
                    return set_label;
                }
            }
            return SP::Expected<void>{};
        };
        auto capture = ScreenshotCli::CaptureDeclarativeScreenshotIfRequested(space,
                                                                              scene->path,
                                                                              window->path,
                                                                              window->view_name,
                                                                              options.width,
                                                                              options.height,
                                                                              options.screenshot,
                                                                              pose);
        if (!capture) {
            std::cerr << "widgets_example: screenshot capture failed: "
                      << SP::describeError(capture.error()) << "\n";
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    if (options.headless) {
        std::cout << "widgets_example: headless mode enabled, declarative widgets mounted at\n"
                  << "  " << button->getPath() << "\n"
                  << "  " << slider->getPath() << "\n"
                  << "  " << list->getPath() << "\n"
                  << "  " << tree->getPath() << "\n";
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
