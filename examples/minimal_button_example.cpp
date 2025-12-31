#include <pathspace/PathSpace.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>
#include <pathspace/tools/PathSpaceJsonExporter.hpp>
#include <cmath>
#include <iostream>
#include <string_view>

using namespace SP::UI::Declarative;

int main(int argc, char** argv) {
    bool dump_json = false;
    bool dump_json_debug = false;
    bool resize_test = false;
    for (int idx = 1; idx < argc; ++idx) {
        std::string_view arg{argv[idx]};
        if (arg == "--dump_json") {
            dump_json = true;
        } else if (arg == "--dump_json_debug") {
            dump_json = true;
            dump_json_debug = true;
        } else if (arg == "--resize-test") {
            resize_test = true;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            std::cerr << "Usage: " << argv[0] << " [--dump_json|--dump_json_debug|--resize-test]\n";
            return 1;
        }
    }

    SP::PathSpace space;

    auto launch = SP::System::LaunchStandard(space);
    if (!launch) {
        std::cerr << "LaunchStandard failed: " << SP::describeError(launch.error()) << '\n';
        return 1;
    }

    auto app = SP::App::Create(space, "declarative_button_example", {.title = "Hello Button"});
    if (!app) {
        std::cerr << "App::Create failed: " << SP::describeError(app.error()) << '\n';
        return 1;
    }

    auto window = SP::Window::Create(space, *app, {.title = "Hello Button", .width = 640, .height = 360, .visible = true});
    if (!window) {
        std::cerr << "Window::Create failed: " << SP::describeError(window.error()) << '\n';
        return 1;
    }

    auto scene = SP::Scene::Create(space, *app, window->path, {.name = "main_scene", .view = window->view_name});
    if (!scene) {
        std::cerr << "Scene::Create failed: " << SP::describeError(scene.error()) << '\n';
        return 1;
    }

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto stack = Stack::Create(
        space,
        window_view,
        "root_stack",
        Stack::Args{
            .panels = {
                Stack::Panel{
                    .id = "button_panel",
                    .fragment = Button::Fragment(Button::Args{.label = "Hello World"})
                },
            },
            .active_panel = "button_panel"
        });
    if (!stack) {
        std::cerr << "Stack::Create failed: " << SP::describeError(stack.error()) << '\n';
        return 1;
    }

    // Wire the scene to the surface so presents succeed.
    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface");
    if (!surface_rel) {
        std::cerr << "surface read failed: " << SP::describeError(surface_rel.error()) << '\n';
        return 1;
    }
    auto surface_abs =
        SP::App::resolve_app_relative(SP::App::AppRootPathView{app->getPath()}, *surface_rel);
    if (!surface_abs) {
        std::cerr << "surface resolve failed: " << SP::describeError(surface_abs.error()) << '\n';
        return 1;
    }
    if (auto set_scene = SP::UI::Runtime::Surface::SetScene(space,
                                                            SP::UI::Runtime::SurfacePath{surface_abs->getPath()},
                                                            scene->path);
        !set_scene) {
        std::cerr << "Surface::SetScene failed: " << SP::describeError(set_scene.error()) << '\n';
        return 1;
    }

    auto present_handles = SP::UI::Declarative::BuildPresentHandles(space,
                                                                    SP::App::AppRootPathView{app->getPath()},
                                                                    window->path,
                                                                    window->view_name);
    if (!present_handles) {
        std::cerr << "BuildPresentHandles failed: " << SP::describeError(present_handles.error()) << '\n';
        return 1;
    }

    // Initialize window based on surface size so resize math stays consistent.
    // Track surface sizes in backing pixels.
    int window_w = 640;
    int window_h = 360;
    if (auto surface_desc = space.read<SP::UI::Runtime::SurfaceDesc, std::string>(
            std::string(present_handles->surface.getPath()) + "/desc")) {
        if (surface_desc->size_px.width > 0) {
            window_w = surface_desc->size_px.width;
        }
        if (surface_desc->size_px.height > 0) {
            window_h = surface_desc->size_px.height;
        }
    }
    SP::UI::InitLocalWindowWithSize(window_w, window_h, "Hello Button");

    if (dump_json) {
        (void)SP::UI::Declarative::ResizePresentSurface(space, *present_handles, window_w, window_h);
        (void)SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene->path);

        auto frame = SP::UI::Declarative::PresentWindowFrame(space, *present_handles);
        if (!frame) {
            std::cerr << "PresentWindowFrame failed: " << SP::describeError(frame.error()) << '\n';
            SP::System::ShutdownDeclarativeRuntime(space);
            return 1;
        }

        SP::System::ShutdownDeclarativeRuntime(space);

        SP::PathSpaceJsonOptions json_options;
        json_options.mode = dump_json_debug ? SP::PathSpaceJsonOptions::Mode::Debug
                                            : SP::PathSpaceJsonOptions::Mode::Minimal;
        json_options.visit.root = app->getPath();
        json_options.visit.includeNestedSpaces = true;
        json_options.visit.includeValues = true;
        json_options.visit.maxChildren = 256;
        json_options.maxQueueEntries = 4;
        json_options.includeOpaquePlaceholders = true;
        json_options.includeDiagnostics = true;

        auto json_export = space.toJSON(json_options);
        if (!json_export) {
            std::cerr << "PathSpace export failed: " << SP::describeError(json_export.error()) << '\n';
            return 1;
        }

        std::cout << *json_export << '\n';
        return 0;
    }

    auto log_desc = [&](std::string const& reason) {
        if (auto tdesc = space.read<SP::UI::Runtime::SurfaceDesc, std::string>(
                std::string(present_handles->target.getPath()) + "/desc")) {
            std::cout << "[resize] " << reason << " target=" << tdesc->size_px.width << "x" << tdesc->size_px.height << '\n';
        }
        if (auto sdesc = space.read<SP::UI::Runtime::SurfaceDesc, std::string>(
                std::string(present_handles->surface.getPath()) + "/desc")) {
            std::cout << "[resize] " << reason << " surface=" << sdesc->size_px.width << "x" << sdesc->size_px.height << '\n';
        }
    };

    if (resize_test) {
        struct Size { int w; int h; };
        std::vector<Size> steps{{640, 360}, {900, 540}, {500, 400}, {720, 720}};
        for (auto const& s : steps) {
            SP::UI::ConfigureLocalWindow(s.w, s.h, "Hello Button");
            std::cout << "[resize-test] request " << s.w << "x" << s.h << '\n';
            int frames = 0;
            bool applied = false;
            while (frames < 300) { // up to ~5 seconds per step
                SP::UI::PollLocalWindow();
                int content_w = 0;
                int content_h = 0;
                SP::UI::GetLocalWindowContentSize(&content_w, &content_h);
                float scale = SP::UI::GetLocalWindowBackingScale();
                if (scale < 1.0f) scale = 1.0f;
                int pixel_w = static_cast<int>(std::llround(static_cast<double>(content_w) * static_cast<double>(scale)));
                int pixel_h = static_cast<int>(std::llround(static_cast<double>(content_h) * static_cast<double>(scale)));
                if (pixel_w > 0 && pixel_h > 0) {
                    window_w = pixel_w;
                    window_h = pixel_h;
                    (void)SP::UI::Declarative::ResizePresentSurface(space, *present_handles, window_w, window_h);
                    log_desc("content-changed");
                    (void)SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene->path);
                    applied = true;
                }

                auto frame = SP::UI::Declarative::PresentWindowFrame(space, *present_handles);
                if (!frame) {
                    std::cerr << "PresentWindowFrame failed: " << SP::describeError(frame.error()) << '\n';
                    break;
                }
                (void)SP::UI::Declarative::PresentFrameToLocalWindow(*frame, window_w, window_h);
                ++frames;
                if (applied) {
                    break;
                }
            }
        }
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    while (true) {
        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            break;
        }
        int content_w = 0;
        int content_h = 0;
        SP::UI::GetLocalWindowContentSize(&content_w, &content_h);
        float scale = SP::UI::GetLocalWindowBackingScale();
        if (scale < 1.0f) scale = 1.0f;
        int pixel_w = static_cast<int>(std::llround(static_cast<double>(content_w) * static_cast<double>(scale)));
        int pixel_h = static_cast<int>(std::llround(static_cast<double>(content_h) * static_cast<double>(scale)));
        if (pixel_w > 0 && pixel_h > 0
            && (pixel_w != window_w || pixel_h != window_h)) {
            window_w = pixel_w;
            window_h = pixel_h;
            (void)SP::UI::Declarative::ResizePresentSurface(space, *present_handles, window_w, window_h);
            log_desc("content-changed");
            (void)SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene->path);
        }

        auto frame = SP::UI::Declarative::PresentWindowFrame(space, *present_handles);
        if (!frame) {
            std::cerr << "PresentWindowFrame failed: " << SP::describeError(frame.error()) << '\n';
            break;
        }
        (void)SP::UI::Declarative::PresentFrameToLocalWindow(*frame, window_w, window_h);
    }
    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
