#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Theme.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace ThemeConfig = SP::UI::Declarative::ThemeConfig;

namespace {

template <typename T>
auto unwrap_or_exit(SP::Expected<T> value, std::string_view context) -> T {
    if (!value) {
        std::cerr << "[declarative_theme_example] " << context << ": "
                  << (value.error().message ? *value.error().message : "unknown error") << '\n';
        std::exit(1);
    }
    return *std::move(value);
}

auto unwrap_or_exit(SP::Expected<void> value, std::string_view context) -> void {
    if (!value) {
        std::cerr << "[declarative_theme_example] " << context << ": "
                  << (value.error().message ? *value.error().message : "unknown error") << '\n';
        std::exit(1);
    }
}

auto print_color(std::array<float, 4> const& color) -> void {
    std::cout << std::fixed << std::setprecision(2)
              << "rgba(" << color[0] << ", " << color[1] << ", " << color[2] << ", "
              << color[3] << ")" << std::endl;
}

} // namespace

int main() {
    SP::PathSpace space;

    auto launch = SP::System::LaunchStandard(space);
    unwrap_or_exit(std::move(launch), "LaunchStandard");

    auto app = unwrap_or_exit(SP::App::Create(space,
                                             "declarative_theme_demo",
                                             {.title = "Declarative Theme Demo"}),
                              "App::Create");
    auto window = unwrap_or_exit(SP::Window::Create(space,
                                                    app,
                                                    {.name = "main_window",
                                                     .title = "Declarative Theme Window",
                                                     .width = 1280,
                                                     .height = 720,
                                                     .visible = false}),
                                 "Window::Create");

    auto scene = unwrap_or_exit(SP::Scene::Create(space,
                                                  app,
                                                  window.path,
                                                  {.name = "theme_scene",
                                                   .description = "Declarative theme preview"}),
                                 "Scene::Create");
    (void)scene;

    auto app_view = SP::App::AppRootPathView{app.getPath()};

    SP::UI::Declarative::Theme::CreateOptions base_theme_options{};
    base_theme_options.name = "sunrise";
    base_theme_options.set_active = true;
    auto base_theme = unwrap_or_exit(SP::UI::Declarative::Theme::Create(space,
                                                                       app_view,
                                                                       base_theme_options),
                                     "Theme::Create sunrise");

    SP::UI::Declarative::Theme::ColorValue sunrise_primary{};
    sunrise_primary.rgba = {0.20f, 0.32f, 0.84f, 1.0f};
    unwrap_or_exit(SP::UI::Declarative::Theme::SetColor(space,
                                                        app_view,
                                                        base_theme.canonical_name,
                                                        "button/background",
                                                        sunrise_primary),
                   "Theme::SetColor sunrise button/background");

    SP::UI::Declarative::Theme::CreateOptions sunset_options{};
    sunset_options.name = "sunset";
    sunset_options.inherits = base_theme.canonical_name;
    sunset_options.set_active = true;
    auto sunset_theme = unwrap_or_exit(SP::UI::Declarative::Theme::Create(space,
                                                                         app_view,
                                                                         sunset_options),
                                       "Theme::Create sunset");

    SP::UI::Declarative::Theme::ColorValue sunset_override{};
    sunset_override.rgba = {0.95f, 0.35f, 0.35f, 1.0f};
    unwrap_or_exit(SP::UI::Declarative::Theme::SetColor(space,
                                                        app_view,
                                                        sunset_theme.canonical_name,
                                                        "button/background",
                                                        sunset_override),
                   "Theme::SetColor sunset button/background");

    auto window_view_path = std::string(window.path.getPath()) + "/views/" + window.view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};
    auto press_count = std::make_shared<int>(0);
    SP::UI::Declarative::Button::Args button_args{};
    button_args.label = "Declarative Hello";
    button_args.theme = sunset_theme.canonical_name;
    button_args.on_press = [press_count](SP::UI::Declarative::ButtonContext&) {
        ++(*press_count);
        std::cout << "Button pressed " << *press_count << " time(s)." << std::endl;
    };

    auto button = unwrap_or_exit(SP::UI::Declarative::Button::Create(space,
                                                                     window_view,
                                                                     "primary_button",
                                                                     button_args),
                                 "Button::Create");

    auto button_theme_paths = unwrap_or_exit(
        ThemeConfig::Resolve(app_view, sunset_theme.canonical_name),
        "Resolve sunset theme");
    auto compiled = unwrap_or_exit(space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
                                      button_theme_paths.value.getPath()),
                                   "Read compiled sunset theme");

    std::cout << "Active theme: " << sunset_theme.canonical_name << '\n'
              << "Button background color = ";
    print_color(compiled.button.background_color);

    std::cout << "Switching active theme back to " << base_theme.canonical_name << "...\n";
    unwrap_or_exit(ThemeConfig::SetActive(space,
                                                              app_view,
                                                              base_theme.canonical_name),
                   "SetActive sunrise");

    auto updated_paths = unwrap_or_exit(
        ThemeConfig::Resolve(app_view, base_theme.canonical_name),
        "Resolve sunrise theme");
    auto updated = unwrap_or_exit(space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
                                      updated_paths.value.getPath()),
                                   "Read compiled sunrise theme");
    std::cout << "Now active theme: " << base_theme.canonical_name << '\n'
              << "Button background (fallback) = ";
    print_color(updated.button.background_color);

    (void)button;

    SP::System::ShutdownDeclarativeRuntime(space);
    return 0;
}
