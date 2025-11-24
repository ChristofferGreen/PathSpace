#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/io/IoTrellis.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/runtime/TelemetryControl.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/PaintSurfaceUploader.hpp>
#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace SP::System::detail {
inline auto sanitize_identifier(std::string_view raw, std::string_view fallback) -> std::string {
    if (raw.empty()) {
        raw = fallback;
    }
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else if (ch == ' ' || ch == '.') {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out.assign(fallback.begin(), fallback.end());
    }
    return out;
}
} // namespace SP::System::detail

namespace SP::System {

struct LaunchOptions {
    std::string default_theme_name = "default";
    bool start_input_runtime = true;
    SP::UI::Declarative::InputTaskOptions input_task_options{};
    bool start_io_trellis = true;
    SP::IO::IoTrellisOptions io_trellis_options{};
    bool start_io_pump = true;
    SP::Runtime::IoPumpOptions io_pump_options{};
    bool start_io_telemetry_control = true;
    SP::Runtime::TelemetryControlOptions telemetry_control_options{};
    bool start_widget_event_trellis = true;
    SP::UI::Declarative::WidgetEventTrellisOptions widget_event_options{};
    bool start_paint_gpu_uploader = true;
    SP::UI::Declarative::PaintSurfaceUploaderOptions paint_gpu_options{};
};

struct LaunchResult {
    bool already_launched = false;
    std::string default_theme_path;
    bool input_runtime_started = false;
    std::string input_runtime_state_path;
    bool io_trellis_started = false;
    bool io_pump_started = false;
    std::string io_pump_state_path;
    bool telemetry_control_started = false;
    std::string telemetry_state_path;
    bool widget_event_trellis_started = false;
    std::string widget_event_trellis_state_path;
    bool paint_gpu_uploader_started = false;
    std::string paint_gpu_state_path;
};

[[nodiscard]] auto LaunchStandard(PathSpace& space,
                                  LaunchOptions const& options = {}) -> SP::Expected<LaunchResult>;

auto ShutdownDeclarativeRuntime(PathSpace& space) -> void;

} // namespace SP::System

namespace SP::App {

struct CreateOptions {
    std::string title;
    std::string default_theme = "default";
};

[[nodiscard]] auto Create(PathSpace& space,
                          std::string_view app_name,
                          CreateOptions const& options = {}) -> SP::Expected<AppRootPath>;

} // namespace SP::App

namespace SP::Window {

struct CreateOptions {
    std::string name = "main_window";
    std::string title;
    int width = 0;
    int height = 0;
    float scale = 0.0f;
    std::string background = "#101218";
    std::string view = "main";
    bool visible = false;
};

struct CreateResult {
    SP::UI::Builders::WindowPath path;
    std::string view_name;
};

[[nodiscard]] auto Create(PathSpace& space,
                          SP::App::AppRootPathView app_root,
                          CreateOptions const& options = {}) -> SP::Expected<CreateResult>;

[[nodiscard]] inline auto Create(PathSpace& space,
                                 SP::App::AppRootPathView app_root,
                                 std::string_view title,
                                 int width = 1280,
                                 int height = 720) -> SP::Expected<CreateResult> {
    CreateOptions options{};
    options.title = std::string(title);
    options.name = SP::System::detail::sanitize_identifier(title, "window");
    options.width = width;
    options.height = height;
    options.visible = true;
    return Create(space, app_root, options);
}

[[nodiscard]] inline auto Create(PathSpace& space,
                                 SP::App::AppRootPath const& app_root,
                                 CreateOptions const& options = {}) -> SP::Expected<CreateResult> {
    return Create(space, SP::App::AppRootPathView{app_root.getPath()}, options);
}

[[nodiscard]] inline auto Create(PathSpace& space,
                                 SP::App::AppRootPath const& app_root,
                                 std::string_view title,
                                 int width = 1280,
                                 int height = 720) -> SP::Expected<CreateResult> {
    return Create(space, SP::App::AppRootPathView{app_root.getPath()}, title, width, height);
}

} // namespace SP::Window

namespace SP::Scene {

struct CreateOptions {
    std::string name;
    std::string description;
    std::string view = "main";
    bool attach_to_window = true;
};

struct CreateResult {
    SP::UI::Builders::ScenePath path;
    std::string view_name;
};

[[nodiscard]] auto Create(PathSpace& space,
                          SP::App::AppRootPathView app_root,
                          SP::UI::Builders::WindowPath const& window_path,
                          CreateOptions const& options = {}) -> SP::Expected<CreateResult>;

[[nodiscard]] inline auto Create(PathSpace& space,
                                 SP::App::AppRootPath const& app_root,
                                 SP::UI::Builders::WindowPath const& window_path,
                                  CreateOptions const& options = {}) -> SP::Expected<CreateResult> {
    return Create(space, SP::App::AppRootPathView{app_root.getPath()}, window_path, options);
}

[[nodiscard]] auto Shutdown(PathSpace& space,
                            SP::UI::Builders::ScenePath const& scene_path) -> SP::Expected<void>;

} // namespace SP::Scene

namespace SP::App {

struct RunOptions {
    int window_width = 1280;
    int window_height = 720;
    std::string window_title;
};

[[nodiscard]] auto RunUI(PathSpace& space,
                         SP::Scene::CreateResult const& scene,
                         SP::Window::CreateResult const& window,
                         RunOptions const& options = {}) -> SP::Expected<void>;

[[nodiscard]] auto RunUI(PathSpace& space,
                         SP::UI::Builders::ScenePath const& scene_path,
                         RunOptions const& options = {}) -> SP::Expected<void>;

} // namespace SP::App
