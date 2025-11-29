#include "declarative_example_shared.hpp"

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/StackReadiness.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

using PathSpaceExamples::LocalInputBridge;
using PathSpaceExamples::install_local_window_bridge;

struct Options {
    int width = 800;
    int height = 600;
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot_compare_path;
    std::optional<std::filesystem::path> screenshot_diff_path;
    std::optional<std::filesystem::path> screenshot_metrics_path;
    double screenshot_max_mean_error = 0.0015;
    bool screenshot_require_present = false;
};

auto parse_options(int argc, char** argv) -> Options {
    Options opts;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("paint_example_new");

    cli.add_int("--width", {.on_value = [&](int value) { opts.width = value; }});
    cli.add_int("--height", {.on_value = [&](int value) { opts.height = value; }});
    cli.add_double("--screenshot-max-mean-error", {.on_value = [&](double value) {
                       opts.screenshot_max_mean_error = value;
                   }});
    cli.add_flag("--screenshot-require-present", {.on_set = [&]() {
                     opts.screenshot_require_present = true;
                 }});
    cli.add_value("--screenshot", {.on_value = [&](std::optional<std::string_view> value)
                                                 -> ExampleCli::ParseError {
        if (!value) {
            return "missing value for --screenshot";
        }
        opts.screenshot_path = std::filesystem::path{std::string(*value)};
        return std::nullopt;
    }});
    cli.add_value("--screenshot-compare",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-compare";
                      }
                      opts.screenshot_compare_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});
    cli.add_value("--screenshot-diff",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-diff";
                      }
                      opts.screenshot_diff_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});
    cli.add_value("--screenshot-metrics",
                  {.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
                      if (!value) {
                          return "missing value for --screenshot-metrics";
                      }
                      opts.screenshot_metrics_path = std::filesystem::path{std::string(*value)};
                      return std::nullopt;
                  }});

    (void)cli.parse(argc, argv);

    opts.width = std::clamp(opts.width, 320, 3840);
    opts.height = std::clamp(opts.height, 240, 2160);
    return opts;
}

auto exit_with_error(SP::PathSpace& space,
                     std::string_view message,
                     std::optional<SP::Error> error = std::nullopt) -> int {
    std::cerr << "paint_example_new: " << message;
    if (error) {
        std::cerr << " (" << SP::describeError(*error) << ")";
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
    std::cerr << "paint_example_new: " << context << " failed";
    if (error.message) {
        std::cerr << ": " << *error.message;
    }
    std::cerr << std::endl;
}

auto window_component_name(SP::UI::WindowPath const& window_path) -> std::string {
    auto raw = std::string(window_path.getPath());
    auto slash = raw.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= raw.size()) {
        return raw;
    }
    return raw.substr(slash + 1);
}

auto capture_screenshot(SP::PathSpace& space,
                        SP::Scene::CreateResult const& scene,
                        SP::Window::CreateResult const& window,
                        Options const& options) -> SP::Expected<void> {
    if (!options.screenshot_path) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                         "screenshot capture requested without output path"});
    }

    SP::UI::Screenshot::DeclarativeScreenshotOptions screenshot_options{};
    screenshot_options.width = options.width;
    screenshot_options.height = options.height;
    screenshot_options.output_png = options.screenshot_path;
    screenshot_options.baseline_png = options.screenshot_compare_path;
    screenshot_options.diff_png = options.screenshot_diff_path;
    screenshot_options.metrics_json = options.screenshot_metrics_path;
    screenshot_options.max_mean_error = options.screenshot_max_mean_error;
    screenshot_options.require_present = options.screenshot_require_present
                                         || options.screenshot_compare_path.has_value();
    screenshot_options.view_name = window.view_name;
    screenshot_options.force_publish = true;
    screenshot_options.wait_for_runtime_metrics = true;

    auto capture = SP::UI::Screenshot::CaptureDeclarative(space,
                                                          scene.path,
                                                          window.path,
                                                          screenshot_options);
    if (!capture) {
        return std::unexpected(capture.error());
    }
    if (capture->matched_baseline) {
        std::cout << "paint_example_new: screenshot matched baseline (mean error "
                  << capture->mean_error.value_or(0.0) << ")\n";
    } else {
        std::cout << "paint_example_new: saved screenshot to "
                  << options.screenshot_path->string() << "\n";
    }
    return {};
}

auto enable_framebuffer_capture(SP::PathSpace& space,
                                SP::Window::CreateResult const& window,
                                bool enabled) -> SP::Expected<void> {
    auto capture_path = std::string(window.path.getPath())
                        + "/views/" + window.view_name + "/present/params/capture_framebuffer";
    auto inserted = space.insert(capture_path, enabled);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto bind_scene_to_surface(SP::PathSpace& space,
                           SP::App::AppRootPath const& app_root,
                           SP::Scene::CreateResult const& scene,
                           SP::Window::CreateResult const& window) -> SP::Expected<void> {
    auto view_base = std::string(window.path.getPath()) + "/views/" + window.view_name;
    auto surface_rel = space.read<std::string, std::string>(view_base + "/surface");
    if (!surface_rel) {
        return std::unexpected(surface_rel.error());
    }
    if (surface_rel->empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "window view missing surface binding"});
    }
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root.getPath()},
                                                     *surface_rel);
    if (!surface_abs) {
        return std::unexpected(surface_abs.error());
    }
    auto surface_path = SP::UI::SurfacePath{surface_abs->getPath()};
    return SP::UI::Surface::SetScene(space, surface_path, scene.path);
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);

    SP::PathSpace space;
    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        return exit_with_error(space, "LaunchStandard failed", launch.error());
    }

    auto app = SP::App::Create(space, "paint_example_new", {.title = "Declarative Button"});
    if (!app) {
        return exit_with_error(space, "App::Create failed", app.error());
    }

    auto window = SP::Window::Create(space, *app, "Declarative Button", options.width, options.height);
    if (!window) {
        return exit_with_error(space, "Window::Create failed", window.error());
    }
    if (auto capture = enable_framebuffer_capture(space, *window, true); !capture) {
        return exit_with_error(space, "failed to enable framebuffer capture", capture.error());
    }

    auto scene = SP::Scene::Create(space, *app, window->path);
    if (!scene) {
        return exit_with_error(space, "Scene::Create failed", scene.error());
    }
    if (std::getenv("PAINT_EXAMPLE_NEW_DEBUG")) {
        std::cerr << "paint_example_new(debug): scene path=" << scene->path.getPath() << "\n";
    }
    if (auto bind = bind_scene_to_surface(space, *app, *scene, *window); !bind) {
        return exit_with_error(space, "Surface::SetScene failed", bind.error());
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto window_widgets_root = window_view_path + "/widgets";

    using namespace SP::UI::Declarative;

    auto status_label = Label::Create(space,
                                      window_view,
                                      "status_label",
                                      "Tap the button or pick a greeting");
    if (!status_label) {
        return exit_with_error(space, "Label::Create failed", status_label.error());
    }

    auto pressed_state = std::make_shared<bool>(false);

    Button::Args button_args{};
    button_args.label = "Say Hello!";
    button_args.style.width = 240.0f;
    button_args.style.height = 64.0f;
    button_args.style.corner_radius = 16.0f;
    button_args.style.text_color = {0.95f, 0.98f, 1.0f, 1.0f};
    button_args.style.typography.font_size = 30.0f;
    button_args.style.typography.line_height = 36.0f;
    button_args.on_press = [pressed_state, status_label_path = *status_label](ButtonContext& ctx) {
        *pressed_state = !*pressed_state;
        auto text = *pressed_state ? "Hello from PathSpace!" : "Tap the button or pick a greeting";
        log_error(Label::SetText(ctx.space, status_label_path, text), "Label::SetText");
        std::cout << "paint_example_new: button pressed (" << (*pressed_state ? "armed" : "reset") << ")\n";
    };
    auto button = Button::Create(space,
                                 window_view,
                                 "hello_button",
                                 button_args);
    if (!button) {
        return exit_with_error(space, "Button::Create failed", button.error());
    }

    std::vector<List::ListItem> greetings{
        {.id = "hello", .label = "Hello"},
        {.id = "bonjour", .label = "Bonjour"},
        {.id = "konnichiwa", .label = "Konnichiwa"},
    };

    List::Args list_args{};
    list_args.items = greetings;
    list_args.on_child_event = [status_label_path = *status_label](ListChildContext& ctx) {
        auto text = std::string{"Selected greeting: "} + ctx.child_id;
        log_error(Label::SetText(ctx.space, status_label_path, text), "Label::SetText");
    };
    list_args.children = {
        {"row_inline_label", Label::Fragment(Label::Args{.text = "Inline hello"})},
        {"row_inline_button",
         Button::Fragment(Button::Args{
             .label = "Bonus",
             .on_press = [](ButtonContext&) {
                 std::cout << "paint_example_new: bonus button clicked!\n";
             },
         })},
    };

    auto greetings_list = List::Create(space,
                                       window_view,
                                       "greetings_list",
                                       list_args);
    if (!greetings_list) {
        return exit_with_error(space, "List::Create failed", greetings_list.error());
    }

    auto list_view = SP::App::ConcretePathView{greetings_list->getPath()};
    auto hi_item = Label::Create(space, list_view, "hi_item", "Hi there");
    if (!hi_item) {
        return exit_with_error(space, "failed to add list child", hi_item.error());
    }
    if (std::getenv("PAINT_EXAMPLE_NEW_DEBUG")) {
        auto lifecycle_state_path = std::string(scene->path.getPath()) + "/runtime/lifecycle/state/running";
        if (auto running = space.read<bool, std::string>(lifecycle_state_path); running) {
            std::cerr << "paint_example_new(debug): lifecycle running=" << (*running ? "true" : "false") << "\n";
        }
        auto print_children = [&](std::string const& root, int depth, auto&& self_ref) -> void {
            auto entries = space.listChildren(SP::ConcretePathStringView{root});
            for (auto const& entry : entries) {
                std::cerr << std::string(depth * 2, ' ') << "- " << entry << "\n";
                self_ref(root + "/" + entry, depth + 1, self_ref);
            }
        };
        auto widget_names = space.listChildren(SP::ConcretePathStringView{window_widgets_root});
        std::cerr << "paint_example_new(debug): window widgets=";
        for (auto const& name : widget_names) {
            std::cerr << " " << name;
        }
        if (widget_names.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto window_component = window_component_name(window->path);
        auto scene_widgets_root = std::string(scene->path.getPath())
                                  + "/structure/widgets/windows/" + window_component
                                  + "/views/" + window->view_name + "/widgets";
        auto scene_window_root = std::string(scene->path.getPath()) + "/structure/widgets/windows";
        auto scene_window_ids = space.listChildren(SP::ConcretePathStringView{scene_window_root});
        std::cerr << "paint_example_new(debug): scene window ids=";
        for (auto const& id : scene_window_ids) {
            std::cerr << " " << id;
        }
        if (scene_window_ids.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto scene_widgets = space.listChildren(SP::ConcretePathStringView{scene_widgets_root});
        std::cerr << "paint_example_new(debug): scene widgets=";
        for (auto const& name : scene_widgets) {
            std::cerr << " " << name;
        }
        if (scene_widgets.empty()) {
            std::cerr << " <none>";
        }
        std::cerr << "\n";
        auto lifecycle_error_queue = std::string(scene->path.getPath()) + "/runtime/lifecycle/log/errors/queue";
        if (auto lifecycle_error = space.take<std::string>(lifecycle_error_queue); lifecycle_error) {
            std::cerr << "paint_example_new(debug): lifecycle error " << *lifecycle_error << "\n";
        }
        for (auto const& name : widget_names) {
            std::cerr << "paint_example_new(debug): subtree for " << name << "\n";
            print_children(window_widgets_root + "/" + name, 1, print_children);
            auto bucket_path = scene_widgets_root + "/" + name;
            auto bucket_file = bucket_path + "/render/bucket/drawables.bin";
            auto bucket_data = space.read<std::vector<std::uint8_t>, std::string>(bucket_file);
            if (bucket_data) {
                std::cerr << "paint_example_new(debug): bucket payload bytes=" << bucket_data->size() << "\n";
            } else {
                std::cerr << "paint_example_new(debug): bucket read error "
                          << SP::describeError(bucket_data.error()) << "\n";
            }
        }
    }

    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene->path,
                                                                       window->path,
                                                                       window->view_name);
    if (!readiness) {
        return exit_with_error(space, "scene readiness failed", readiness.error());
    }

    if (options.screenshot_path) {
        auto screenshot = capture_screenshot(space, *scene, *window, options);
        if (!screenshot) {
            return exit_with_error(space, "screenshot capture failed", screenshot.error());
        }
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    auto run = SP::App::RunUI(space,
                              *scene,
                              *window,
                              {.window_width = options.width,
                               .window_height = options.height,
                               .window_title = "Declarative Button"});
    if (!run) {
        return exit_with_error(space, "App::RunUI failed", run.error());
    }

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
