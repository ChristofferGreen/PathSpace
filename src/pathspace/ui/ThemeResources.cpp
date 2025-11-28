#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <pathspace/ui/declarative/ThemeConfig.hpp>

namespace SP::UI::Runtime::Config::Theme {

auto SanitizeName(std::string_view theme_name) -> std::string {
    return SP::UI::Declarative::ThemeConfig::SanitizeName(theme_name);
}

auto Resolve(AppRootPathView appRoot,
             std::string_view theme_name) -> SP::Expected<ThemePaths> {
    return SP::UI::Declarative::ThemeConfig::Resolve(appRoot, theme_name);
}

auto Ensure(PathSpace& space,
            AppRootPathView appRoot,
            std::string_view theme_name,
            Widgets::WidgetTheme const& defaults) -> SP::Expected<ThemePaths> {
    return SP::UI::Declarative::ThemeConfig::Ensure(space, appRoot, theme_name, defaults);
}

auto Load(PathSpace& space,
          ThemePaths const& paths) -> SP::Expected<Widgets::WidgetTheme> {
    return SP::UI::Declarative::ThemeConfig::Load(space, paths);
}

auto SetActive(PathSpace& space,
               AppRootPathView appRoot,
               std::string_view theme_name) -> SP::Expected<void> {
    return SP::UI::Declarative::ThemeConfig::SetActive(space, appRoot, theme_name);
}

auto LoadActive(PathSpace& space,
                AppRootPathView appRoot) -> SP::Expected<std::string> {
    return SP::UI::Declarative::ThemeConfig::LoadActive(space, appRoot);
}

} // namespace SP::UI::Runtime::Config::Theme
