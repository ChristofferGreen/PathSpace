#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace SP::System {

struct LaunchOptions {
    std::string default_theme_name = "default";
};

struct LaunchResult {
    bool already_launched = false;
    std::string default_theme_path;
};

[[nodiscard]] auto LaunchStandard(PathSpace& space,
                                  LaunchOptions const& options = {}) -> SP::Expected<LaunchResult>;

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
                                 SP::App::AppRootPath const& app_root,
                                 CreateOptions const& options = {}) -> SP::Expected<CreateResult> {
    return Create(space, SP::App::AppRootPathView{app_root.getPath()}, options);
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

} // namespace SP::Scene
