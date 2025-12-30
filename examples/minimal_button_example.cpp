#include <pathspace/PathSpace.hpp>
#include <pathspace/system/Standard.hpp>

constexpr int window_width = 640;
constexpr int window_height = 360;

using namespace SP::UI::Declarative;

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot_os_path;
    bool screenshot_exit = false;
    bool dump_json = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshot_path = std::filesystem::path{argv[++i]};
        } else if (arg == "--screenshot_os" && i + 1 < argc) {
            screenshot_os_path = std::filesystem::path{argv[++i]};
            screenshot_exit = true;
        } else if (arg == "--screenshot_exit") {
            screenshot_exit = true;
        } else if (arg == "--dump_json") {
            dump_json = true;
        }
    }

    SP::PathSpace space;

    if (auto launch = SP::System::LaunchStandard(space); !launch) {
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

    auto window = SP::Window::Create(space, *app, SP::Window::CreateOptions{.name = "declarative_button",
                                                                            .title = "Declarative Button",
                                                                            .width = window_width,
                                                                            .height = window_height,
                                                                            .visible = true});
    if (!window) {
        std::fprintf(stderr, "Window::Create failed: %s\n", SP::describeError(window.error()).c_str());
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

    using StackConstraints = SP::UI::Runtime::Widgets::StackChildConstraints;
    StackConstraints fill_half{};
    fill_half.weight = 1.0f; // split available vertical space evenly

    SP::UI::Runtime::Widgets::StackLayoutStyle vertical_fill{};
    vertical_fill.axis = SP::UI::Runtime::Widgets::StackAxis::Vertical;

    auto stack = SP::UI::Declarative::Stack::Create(
        space,
        window_view,
        "button_column",
        SP::UI::Declarative::Stack::Args{
            .panels = {
                {.id = "hello_button",
                 .fragment = Button::Fragment(Button::Args{.label = "Say Hello"}),
                 .constraints = fill_half},
                {.id = "goodbye_button",
                 .fragment = Button::Fragment(Button::Args{.label = "Say Goodbye"}),
                 .constraints = fill_half},
            },
            .style = vertical_fill,
        });
    if (!stack) {
        std::fprintf(stderr, "stack create failed: %s\n", SP::describeError(stack.error()).c_str());
        return 1;
    }
    auto shutdown_runtime = [&space]() { SP::System::ShutdownDeclarativeRuntime(space); };

    auto readiness = PathSpaceExamples::ensure_declarative_scene_ready(space,
                                                                       scene->path,
                                                                       window->path,
                                                                       window->view_name,
                                                                       PathSpaceExamples::DeclarativeReadinessOptions{.force_scene_publish = true,
                                                                                                                      .wait_for_buckets = true});
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

    bool needs_any_capture = screenshot_path.has_value() || screenshot_os_path.has_value();
    if (needs_any_capture) {
        run_options.run_once = false;
    }

    // Inline RunUI so we can screenshot immediately after presents.
    auto derived_app_root = SP::App::derive_app_root(SP::App::ConcretePathView{scene->path.getPath()});
    if (!derived_app_root) {
        std::fprintf(stderr, "derive app root failed: %s\n", SP::describeError(derived_app_root.error()).c_str());
        shutdown_runtime();
        return 1;
    }
    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    SP::App::AppRootPathView{derived_app_root->getPath()},
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        std::fprintf(stderr, "BuildPresentHandles failed: %s\n", SP::describeError(present_handles.error()).c_str());
        shutdown_runtime();
        return 1;
    }

    PathSpaceExamples::LocalInputBridge bridge{};
    bridge.space = &space;
    PathSpaceExamples::install_local_window_bridge(bridge);

    auto surface_desc =
        space.read<SP::UI::Runtime::SurfaceDesc, std::string>(std::string(present_handles->surface.getPath()) + "/desc");
    if (!surface_desc) {
        std::fprintf(stderr, "surface desc read failed: %s\n", SP::describeError(surface_desc.error()).c_str());
        shutdown_runtime();
        return 1;
    }
    int window_width_run = run_options.window_width > 0 ? run_options.window_width : surface_desc->size_px.width;
    int window_height_run = run_options.window_height > 0 ? run_options.window_height : surface_desc->size_px.height;
    std::string title = run_options.window_title.empty() ? "PathSpace Declarative Window"
                                                         : run_options.window_title;

    SP::UI::InitLocalWindowWithSize(window_width_run, window_height_run, title.c_str());
    std::uint64_t frames_rendered = 0;
    bool captured1 = false;
    bool captured_os = false;
    int os_attempts = 0;
    constexpr int kMaxOsAttempts = 120; // ~2 seconds at 60fps
    while (true) {
        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            break;
        }
        int content_w = window_width_run;
        int content_h = window_height_run;
        SP::UI::GetLocalWindowContentSize(&content_w, &content_h);
        if (content_w > 0 && content_h > 0 && (content_w != window_width_run || content_h != window_height_run)) {
            window_width_run = content_w;
            window_height_run = content_h;
            (void)SP::UI::Declarative::ResizePresentSurface(space,
                                                            *present_handles,
                                                            window_width_run,
                                                            window_height_run);
        }

        auto present_frame_expected = SP::UI::Declarative::PresentWindowFrame(space, *present_handles);
        if (!present_frame_expected) {
            std::fprintf(stderr, "PresentWindowFrame failed: %s\n", SP::describeError(present_frame_expected.error()).c_str());
            break;
        }
        auto const& present_frame = *present_frame_expected;
        auto presented = SP::UI::Declarative::PresentFrameToLocalWindow(present_frame,
                                                                        window_width_run,
                                                                        window_height_run);
        ++frames_rendered;

        // Capture after presents so framebuffer matches the window.
        if (screenshot_path && !captured1 && frames_rendered >= 30) {
            if (SP::UI::SaveLocalWindowScreenshot(screenshot_path->c_str())) {
                captured1 = true;
            } else {
                std::fprintf(stderr, "screenshot capture failed (SaveLocalWindowScreenshot)\n");
            }
        }
        if (screenshot_os_path && !captured_os && frames_rendered >= 30) {
            ++os_attempts;
            if (SP::UI::SaveActiveWindowScreenshot(screenshot_os_path->c_str())) {
                std::fprintf(stderr, "active window screenshot saved to %s\n", screenshot_os_path->c_str());
                captured_os = true;
            } else {
                std::fprintf(stderr, "active window screenshot failed (SaveActiveWindowScreenshot)\n");
                if (os_attempts >= kMaxOsAttempts) {
                    std::fprintf(stderr, "active window screenshot giving up after %d attempts\n", os_attempts);
                    captured_os = true; // allow exit instead of hanging forever
                }
            }
        }

        if (run_options.run_once && frames_rendered >= 1) {
            break;
        }
        if (needs_any_capture && captured1 && (!screenshot_os_path || captured_os)) {
            SP::UI::RequestLocalWindowQuit();
        }
    }

    SP::UI::SetLocalWindowCallbacks({});
    shutdown_runtime();

    if (dump_json) {
        return export_json(0);
    }

    return 0;
}
