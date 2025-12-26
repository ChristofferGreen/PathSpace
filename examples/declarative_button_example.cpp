#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

constexpr int window_width = 640;
constexpr int window_height = 360;

using namespace SP::UI::Declarative;

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> screenshot_path;
    bool screenshot_exit = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshot_path = std::filesystem::path{argv[++i]};
        }
        if (arg == "--screenshot_exit") {
            screenshot_exit = true;
        }
    }

    SP::PathSpace space;
    SP::System::LaunchStandard(space).value();

    auto app = SP::App::Create(space,
                               "declarative_button_example",
                               {.title = "Declarative Button"})
                    .value();

    auto window = SP::Window::Create(space,
                                     app,
                                     "Declarative Button",
                                     window_width,
                                     window_height)
                      .value();

    auto scene = SP::Scene::Create(space,
                                   app,
                                   window.path,
                                   {.name = "button_scene",
                                    .view = window.view_name})
                     .value();

    auto window_view_path = std::string(window.path.getPath()) + "/views/" + window.view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    auto surface_rel = space.read<std::string, std::string>(window_view_path + "/surface").value();
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app.getPath()}, surface_rel).value();
    SP::UI::Surface::SetScene(space,
                              SP::UI::SurfacePath{surface_abs.getPath()},
                              scene.path).value();

    SP::UI::Declarative::Stack::Create(space,
                                       window_view,
                                       "button_column",
                                       SP::UI::Declarative::Stack::Args{.panels = {
                                            {.id = "hello_button", .fragment = Button::Fragment(Button::Args{.label = "Say Hello"})},
                                            {.id = "goodbye_button", .fragment = Button::Fragment(Button::Args{.label = "Say Goodbye"})},
                                       }}).value();

    // Ensure drawables exist before any screenshot.
    SP::UI::Declarative::SceneLifecycle::MarkDirty(space,
                                                   scene.path,
                                                   SP::UI::Runtime::Scene::DirtyKind::All)
        .value();
    SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_opts{};
    publish_opts.wait_timeout = std::chrono::milliseconds{2000};
    SP::UI::Declarative::SceneLifecycle::ForcePublish(space,
                                                      scene.path,
                                                      publish_opts)
        .value();

    if (screenshot_path) {
        SP::UI::Screenshot::DeclarativeScreenshotOptions screenshot_options{
            .width = window_width,
            .height = window_height,
            .output_png = *screenshot_path,
            .view_name = window.view_name,
            .require_present = true,
            .present_before_capture = true,
            .allow_software_fallback = true,
            .force_software = false,
        };
        SP::UI::Screenshot::CaptureDeclarative(space,
                                               scene.path,
                                               window.path,
                                               screenshot_options)
            .value();
    }
    if (screenshot_exit) {
        SP::System::ShutdownDeclarativeRuntime(space);
        return 0;
    }

    SP::App::RunUI(space,
                   scene,
                   window,
                   {.window_width = window_width,
                    .window_height = window_height,
                    .window_title = "Declarative Button"})
        .value();

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
