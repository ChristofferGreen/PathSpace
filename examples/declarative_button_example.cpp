#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>
#include <pathspace/ui/screenshot/ScreenshotSlot.hpp>
#include <pathspace/tools/PathSpaceJsonExporter.hpp>

#include "declarative_example_shared.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

constexpr int window_width = 640;
constexpr int window_height = 360;

using namespace SP::UI::Declarative;

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot2_path;
    bool screenshot_exit = false;
    bool dump_json = false;
    bool dump_json_debug = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshot_path = std::filesystem::path{argv[++i]};
        } else if (arg == "--screenshot2" && i + 1 < argc) {
            screenshot2_path = std::filesystem::path{argv[++i]};
            screenshot_exit = true;
        } else if (arg == "--screenshot_exit") {
            screenshot_exit = true;
        } else if (arg == "--dump_json") {
            dump_json = true;
        } else if (arg == "--dump_json_debug") {
            dump_json = true;
            dump_json_debug = true;
        }
    }

    SP::PathSpace space;

    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::fprintf(stderr, "LaunchStandard failed: %s\n", SP::describeError(launch.error()).c_str());
        return 1;
    }

    auto app = SP::App::Create(space,
                               "declarative_button_example",
                               {.title = "Declarative Button"});
    if (!app) {
        std::fprintf(stderr, "App::Create failed: %s\n", SP::describeError(app.error()).c_str());
        return 1;
    }

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "declarative_button";
    window_opts.title = "Declarative Button";
    window_opts.width = window_width;
    window_opts.height = window_height;
    window_opts.visible = true;
    auto window = SP::Window::Create(space, *app, window_opts);
    if (!window) {
        std::fprintf(stderr, "Window::Create failed: %s\n", SP::describeError(window.error()).c_str());
        return 1;
    }

    auto force_software = PathSpaceExamples::force_window_software_renderer(space,
                                                                            window->path,
                                                                            window->view_name);
    if (!force_software) {
        std::fprintf(stderr, "force_window_software_renderer failed: %s\n", SP::describeError(force_software.error()).c_str());
        SP::System::ShutdownDeclarativeRuntime(space);
        return 1;
    }

    auto scene = SP::Scene::Create(space,
                                   *app,
                                   window->path,
                                   {.name = "button_scene",
                                    .view = window->view_name});
    if (!scene) {
        std::fprintf(stderr, "Scene::Create failed: %s\n", SP::describeError(scene.error()).c_str());
        return 1;
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    if (!surface_rel) {
        std::fprintf(stderr, "surface read failed: %s\n", SP::describeError(surface_rel.error()).c_str());
        return 1;
    }
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()}, *surface_rel);
    if (!surface_abs) {
        std::fprintf(stderr, "surface resolve failed: %s\n", SP::describeError(surface_abs.error()).c_str());
        return 1;
    }
    auto set_scene = SP::UI::Surface::SetScene(space,
                                               SP::UI::SurfacePath{surface_abs->getPath()},
                                               scene->path);
    if (!set_scene) {
        std::fprintf(stderr, "set scene failed: %s\n", SP::describeError(set_scene.error()).c_str());
        return 1;
    }

    auto stack = SP::UI::Declarative::Stack::Create(space,
                                                    window_view,
                                                    "button_column",
                                                    SP::UI::Declarative::Stack::Args{.panels = {
                                                         {.id = "hello_button", .fragment = Button::Fragment(Button::Args{.label = "Say Hello"})},
                                                         {.id = "goodbye_button", .fragment = Button::Fragment(Button::Args{.label = "Say Goodbye"})},
                                                    }});
    if (!stack) {
        std::fprintf(stderr, "stack create failed: %s\n", SP::describeError(stack.error()).c_str());
        return 1;
    }
    auto write_label = [&](std::string const& widget_id, std::string const& label) {
        auto widget_path = window_view_path + "/widgets/button_column/children/" + widget_id + "/meta/label";
        (void)space.insert(widget_path, std::string{label});
    };
    write_label("hello_button", "Say Hello");
    write_label("goodbye_button", "Say Goodbye");

    auto shutdown_runtime = [&space]() { SP::System::ShutdownDeclarativeRuntime(space); };

    PathSpaceExamples::DeclarativeReadinessOptions readiness_options{};
    readiness_options.force_scene_publish = true;
    readiness_options.wait_for_buckets = false;
    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene->path,
                                                                       window->path,
                                                                       window->view_name,
                                                                       readiness_options);
    if (!readiness) {
        std::fprintf(stderr,
                     "ensure declarative scene ready failed: %s\n",
                     SP::describeError(readiness.error()).c_str());
        shutdown_runtime();
        return 1;
    }
    if (screenshot_path) {
        SP::UI::Screenshot::CaptureDeclarativeSimple(space,
                                                     scene->path,
                                                     window->path,
                                                     *screenshot_path,
                                                     window_width,
                                                     window_height);
    }

    auto export_json = [&](int previous_status) -> int {
        SP::PathSpaceJsonOptions options{};
        options.visit.root = app->getPath();
        if (dump_json_debug) {
            options.mode = SP::PathSpaceJsonOptions::Mode::Debug;
        }

        auto json = SP::PathSpaceJsonExporter::Export(space, options);
        if (!json) {
            std::fprintf(stderr, "dump json failed: %s\n", SP::describeError(json.error()).c_str());
            return 1;
        }
        std::printf("%s\n", json->c_str());
        return previous_status;
    };

    SP::App::RunOptions run_options{
        .window_width = window_width,
        .window_height = window_height,
        .window_title = "Declarative Button",
        .run_once = dump_json || screenshot_exit,
    };

    bool needs_any_capture = screenshot_path.has_value() || screenshot2_path.has_value();
    if (needs_any_capture) {
        run_options.run_once = false; // keep presenting until we request quit
        std::thread capture_thread([&, screenshot_path, screenshot2_path] {
            // First capture shortly after startup to allow the scene to publish.
            if (screenshot_path) {
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                if (!SP::UI::SaveLocalWindowScreenshot(screenshot_path->c_str())) {
                    std::fprintf(stderr, "screenshot capture failed (SaveLocalWindowScreenshot)\n");
                }
            }
            if (screenshot2_path) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1500});
                if (!SP::UI::SaveLocalWindowScreenshot(screenshot2_path->c_str())) {
                    std::fprintf(stderr, "screenshot2 capture failed (SaveLocalWindowScreenshot)\n");
                }
            }
            SP::UI::RequestLocalWindowQuit();
        });
        capture_thread.detach();
    }

    auto run_ui = SP::App::RunUI(space, *scene, *window, run_options);
    if (!run_ui) {
        std::fprintf(stderr, "RunUI failed: %s\n", SP::describeError(run_ui.error()).c_str());
        shutdown_runtime();
        return 1;
    }
    shutdown_runtime();

    if (dump_json) {
        return export_json(0);
    }

    return 0;
}
