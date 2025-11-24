#include "declarative_example_shared.hpp"

#include <pathspace/PathSpace.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/examples/paint/PaintControls.hpp>
#include <pathspace/layer/io/PathIOGamepad.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;
namespace PaintControlsNS = SP::Examples::PaintControls;
using PaintControlsNS::BrushState;

static std::atomic<bool> g_running{true};

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool paint_controls_demo = false;
};

static auto parse_options(int argc, char** argv) -> CommandLineOptions {
    CommandLineOptions opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("devices_example");

    cli.add_flag("--paint-controls-demo", {.on_set = [&] { opts.paint_controls_demo = true; }});
    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});

    (void)cli.parse(argc, argv);

    opts.width = std::max(640, opts.width);
    opts.height = std::max(480, opts.height);
    return opts;
}

static void sigint_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Minimal aliases to keep the example readable and close to the sketch
using PointerDeviceEvent = PathIOMouse::Event;
using TextInputDeviceEvent = PathIOKeyboard::Event;
using GamepadEvent = PathIOGamepad::Event;
// Stream events via operator<<; no custom print overloads
#if defined(__APPLE__)
#include <pathspace/ui/LocalWindowBridge.hpp>
namespace SP {
    void PSInitGameControllerInput(PathIOGamepad*);
}
#endif

// Print helpers: accept Expected<T> and only print when an event is available
namespace SP {
static inline std::ostream& operator<<(std::ostream& os, Error const& err) {
    os << "[error]";
    if (err.message.has_value()) {
        os << " " << *err.message;
    } else {
        os << " code=" << static_cast<int>(err.code);
    }
    return os;
}
template <typename T>
static inline std::ostream& operator<<(std::ostream& os, Expected<T> const& e) {
    if (e.has_value()) {
        return os << *e;
    }
    return os << e.error();
}
} // namespace SP

struct DeviceEventSink {
    PathSpace& space;
    std::optional<SP::UI::Builders::WidgetPath> status_label;
};

static void update_status_label(DeviceEventSink& sink, std::string const& message) {
    if (!sink.status_label) {
        return;
    }
    auto result = SP::UI::Declarative::Label::SetText(sink.space, *sink.status_label, message);
    if (!result) {
        auto const& error = result.error();
        std::cerr << "devices_example: failed to update status label";
        if (error.message) {
            std::cerr << " (" << *error.message << ")";
        }
        std::cerr << std::endl;
    }
}

static bool drain_device_events(DeviceEventSink& sink) {
    bool printed = false;
    auto handle_event = [&](auto const& event, std::string_view channel) {
        std::ostringstream stream;
        stream << channel << ": " << event;
        std::cout << stream.str() << std::endl;
        update_status_label(sink, stream.str());
        printed = true;
    };
    if (auto pe = sink.space.take<"/system/devices/in/pointer/default/events", PointerDeviceEvent>(); pe.has_value()) {
        handle_event(*pe, "pointer");
    }
    if (auto ke = sink.space.take<"/system/devices/in/text/default/events", TextInputDeviceEvent>(); ke.has_value()) {
        handle_event(*ke, "keyboard");
    }
    if (auto ge = sink.space.take<"/system/devices/in/gamepad/default/events", GamepadEvent>(); ge.has_value()) {
        handle_event(*ge, "gamepad");
    }
    return printed;
}

struct PaintControlsDemoHandles {
    SP::UI::Builders::WindowPath window_path;
    std::string view_name;
    SP::UI::Builders::App::BootstrapResult bootstrap;
    SP::UI::Builders::WidgetPath status_label;
};

static void log_expected_error(std::string const& context, Error const& error) {
    std::cerr << "devices_example: " << context << " failed";
    if (error.message) {
        std::cerr << " (" << *error.message << ")";
    } else {
        std::cerr << " (code=" << static_cast<int>(error.code) << ")";
    }
    std::cerr << std::endl;
}

static std::optional<PaintControlsDemoHandles> launch_paint_controls_demo(PathSpace& space,
                                                                          CommandLineOptions const& options) {
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        log_expected_error("LaunchStandard", launch.error());
        return std::nullopt;
    }
    auto app = SP::App::Create(space,
                               "devices_controls",
                               {.title = "PathSpace Devices Controls"});
    if (!app) {
        log_expected_error("App::Create", app.error());
        return std::nullopt;
    }
    auto app_root = *app;
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    auto theme_selection = SP::UI::Builders::Widgets::LoadTheme(space, app_root_view, "");
    auto active_theme = theme_selection.theme;

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "devices_controls_window";
    window_opts.title = "Devices Example Controls";
    window_opts.width = options.width;
    window_opts.height = options.height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    if (!window) {
        log_expected_error("Window::Create", window.error());
        return std::nullopt;
    }

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "devices_controls_scene";
    scene_opts.description = "Devices example paint controls showcase";
    scene_opts.view = window->view_name;
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    if (!scene) {
        log_expected_error("Scene::Create", scene.error());
        return std::nullopt;
    }

    auto bootstrap = PathSpaceExamples::build_bootstrap_from_window(space,
                                                                    app_root_view,
                                                                    window->path,
                                                                    window->view_name);
    if (!bootstrap) {
        log_expected_error("build_bootstrap_from_window", bootstrap.error());
        return std::nullopt;
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto status_label = SP::UI::Declarative::Label::Create(space,
                                                           window_view,
                                                           "devices_status_label",
                                                           {
                                                               .text = "Waiting for device events…",
                                                               .typography = PaintControlsNS::MakeTypography(18.0f, 22.0f),
                                                           });
    if (!status_label) {
        log_expected_error("Label::Create", status_label.error());
        return std::nullopt;
    }

    auto layout_metrics = PaintControlsNS::ComputeLayoutMetrics(options.width, options.height);
    auto brush_state = std::make_shared<BrushState>();

    PaintControlsNS::BrushSliderConfig slider_config{
        .layout = layout_metrics,
        .brush_state = brush_state,
        .minimum = 1.0f,
        .maximum = 64.0f,
        .step = 1.0f,
        .on_change = [status_label_path = *status_label](SP::UI::Declarative::SliderContext& ctx, float value) {
            std::ostringstream stream;
            stream << "Brush size set to " << std::fixed << std::setprecision(1) << value;
            (void)SP::UI::Declarative::Label::SetText(ctx.space, status_label_path, stream.str());
        },
    };

    auto palette_entries = PaintControlsNS::BuildDefaultPaletteEntries(active_theme);
    PaintControlsNS::PaletteComponentConfig palette_config{
        .layout = layout_metrics,
        .theme = active_theme,
        .entries = std::span<const PaintControlsNS::PaletteEntry>(palette_entries.data(), palette_entries.size()),
        .brush_state = brush_state,
        .on_select = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx,
                                                        PaintControlsNS::PaletteEntry const& entry) {
            std::ostringstream stream;
            stream << "Palette color selected: " << entry.label;
            (void)SP::UI::Declarative::Label::SetText(ctx.space, status_label_path, stream.str());
        },
    };

    PaintControlsNS::HistoryActionsConfig history_config{
        .layout = layout_metrics,
        .on_action = [status_label_path = *status_label](SP::UI::Declarative::ButtonContext& ctx,
                                                        PaintControlsNS::HistoryAction action) {
            std::ostringstream stream;
            stream << (action == PaintControlsNS::HistoryAction::Undo ? "Undo" : "Redo") << " requested";
            (void)SP::UI::Declarative::Label::SetText(ctx.space, status_label_path, stream.str());
        },
        .undo_label = "Undo Stroke",
        .redo_label = "Redo Stroke",
    };

    SP::UI::Declarative::Stack::Args paint_controls_stack{};
    paint_controls_stack.style.axis = SP::UI::Builders::Widgets::StackAxis::Vertical;
    paint_controls_stack.style.spacing = std::max(10.0f, layout_metrics.controls_spacing * 0.5f);
    paint_controls_stack.style.padding_main_start = layout_metrics.controls_padding_main;
    paint_controls_stack.style.padding_main_end = layout_metrics.controls_padding_main;
    paint_controls_stack.style.padding_cross_start = layout_metrics.controls_padding_cross;
    paint_controls_stack.style.padding_cross_end = layout_metrics.controls_padding_cross;
    paint_controls_stack.style.width = std::min(420.0f, layout_metrics.controls_width);

    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "devices_brush_slider",
        .fragment = PaintControlsNS::BuildBrushSliderFragment(slider_config),
    });
    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "devices_palette",
        .fragment = PaintControlsNS::BuildPaletteFragment(palette_config),
    });
    paint_controls_stack.panels.push_back(SP::UI::Declarative::Stack::Panel{
        .id = "devices_history",
        .fragment = PaintControlsNS::BuildHistoryActionsFragment(history_config),
    });
    PaintControlsNS::EnsureActivePanel(paint_controls_stack);

    auto paint_controls = SP::UI::Declarative::Stack::Create(space,
                                                             window_view,
                                                             "devices_paint_controls",
                                                             std::move(paint_controls_stack));
    if (!paint_controls) {
        log_expected_error("Stack::Create", paint_controls.error());
        return std::nullopt;
    }

    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene->path,
                                                                       window->path,
                                                                       window->view_name);
    if (!readiness) {
        log_expected_error("scene readiness", readiness.error());
        return std::nullopt;
    }

    constexpr std::string_view kPointerDevice = "/system/devices/in/pointer/default";
    constexpr std::string_view kKeyboardDevice = "/system/devices/in/text/default";
    PathSpaceExamples::ensure_device_push_config(space, std::string{kPointerDevice}, "devices_example_ui");
    PathSpaceExamples::ensure_device_push_config(space, std::string{kKeyboardDevice}, "devices_example_ui");
    std::array<std::string, 1> pointer_devices{std::string{kPointerDevice}};
    std::array<std::string, 1> keyboard_devices{std::string{kKeyboardDevice}};
    PathSpaceExamples::subscribe_window_devices(space,
                                                window->path,
                                                std::span<const std::string>(pointer_devices.data(),
                                                                             pointer_devices.size()),
                                                std::span<const std::string>{},
                                                std::span<const std::string>(keyboard_devices.data(),
                                                                             keyboard_devices.size()));

    PaintControlsDemoHandles handles{
        .window_path = window->path,
        .view_name = window->view_name,
        .bootstrap = std::move(*bootstrap),
        .status_label = *status_label,
    };
    return handles;
}

// Device initialization kept minimal: mount providers at the sketch paths without spinning up threads
static inline void initialize_devices(PathSpace& space) {
#if defined(__APPLE__)
    // Use local window to forward events without global permissions
    auto mouse = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Off);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Off);
    auto gamepad = std::make_unique<PathIOGamepad>(PathIOGamepad::BackendMode::Auto);
    PathIOMouse* mousePtr = mouse.get();
    PathIOKeyboard* keyboardPtr = keyboard.get();
    PathIOGamepad* gamepadPtr = gamepad.get();

    space.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    space.insert<"/system/devices/in/text/default">(std::move(keyboard));
    space.insert<"/system/devices/in/gamepad/default">(std::move(gamepad));

    (void)mousePtr;
    (void)keyboardPtr;
    SP::UI::SetLocalWindowCallbacks({});
    SP::UI::InitLocalWindow();
    PSInitGameControllerInput(gamepadPtr);
#else
    auto mouse = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Auto);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Auto);
    auto gamepad = std::make_unique<PathIOGamepad>(PathIOGamepad::BackendMode::Auto);

    space.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    space.insert<"/system/devices/in/text/default">(std::move(keyboard));
    space.insert<"/system/devices/in/gamepad/default">(std::move(gamepad));
#endif

    // Enable push-mode delivery for the default pointer device and register a subscriber.
    space.insert("/system/devices/in/pointer/default/config/push/enabled", true);
    space.insert("/system/devices/in/pointer/default/config/push/rate_limit_hz", static_cast<std::uint32_t>(480));
    space.insert("/system/devices/in/pointer/default/config/push/subscribers/devices_example", true);

    space.insert("/system/devices/in/text/default/config/push/enabled", true);
    space.insert("/system/devices/in/text/default/config/push/rate_limit_hz", static_cast<std::uint32_t>(480));
    space.insert("/system/devices/in/text/default/config/push/subscribers/devices_example", true);
}

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    std::signal(SIGINT, sigint_handler);

    PathSpace space;
    initialize_devices(space);
    // Issue a sample rumble command on the default gamepad (ignored if unsupported)
    {
        SP::PathIOGamepad::HapticsCommand rumble = SP::PathIOGamepad::HapticsCommand::constant(0.25f, 0.5f, 200);
        space.insert<"/system/devices/in/gamepad/default/rumble">(rumble);
    }

    DeviceEventSink sink{space, std::nullopt};
    if (options.paint_controls_demo) {
        auto demo = launch_paint_controls_demo(space, options);
        if (!demo) {
            return 1;
        }
        sink.status_label = demo->status_label;

        PathSpaceExamples::LocalInputBridge bridge{};
        bridge.space = &space;
        PathSpaceExamples::install_local_window_bridge(bridge);

        PathSpaceExamples::PresentLoopHooks hooks{};
        hooks.per_frame = [&]() {
            if (!g_running.load(std::memory_order_acquire)) {
                SP::UI::RequestLocalWindowQuit();
            }
            (void)drain_device_events(sink);
        };

        PathSpaceExamples::run_present_loop(space,
                                            demo->window_path,
                                            demo->view_name,
                                            demo->bootstrap,
                                            options.width,
                                            options.height,
                                            hooks);
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    while (g_running.load(std::memory_order_acquire)) {
#if defined(__APPLE__)
        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            g_running.store(false, std::memory_order_release);
            break;
        }
#endif
        if (!drain_device_events(sink)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return 0;
}

/*
Build notes:

- This example depends on the library target "PathSpace". Add an examples section to your CMake
  to build it conditionally (e.g., -DBUILD_PATHSPACE_EXAMPLES=ON):

  option(BUILD_PATHSPACE_EXAMPLES "Build PathSpace examples" OFF)
  if(BUILD_PATHSPACE_EXAMPLES)
      add_executable(devices_example examples/devices_example.cpp)
      target_link_libraries(devices_example PRIVATE PathSpace)
      if(APPLE AND ENABLE_PATHIO_MACOS)
          target_compile_definitions(devices_example PRIVATE PATHIO_BACKEND_MACOS=1)
      endif()
  endif()

- Run: ./devices_example
  - Ctrl-C to exit.
  - Use `--paint-controls-demo [--width=<W>] [--height=<H>]` to launch the declarative window that embeds
    `SP::Examples::PaintControls` alongside the device event stream.
  - With -DDEVICES_EXAMPLE_SIMULATE=1 (default here), it will simulate plug/unplug and events.
  - When OS backends are implemented, mount those providers instead and remove the simulation flag.

*/
