#include <pathspace/ui/BuildersShared.hpp>

#include <pathspace/ui/LegacyBuildersDeprecation.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>

namespace SP::UI::Builders::Config::Theme {

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
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Config::Theme::Ensure");
    return SP::UI::Declarative::ThemeConfig::Ensure(space, appRoot, theme_name, defaults);
}

auto Load(PathSpace& space,
          ThemePaths const& paths) -> SP::Expected<Widgets::WidgetTheme> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Config::Theme::Load");
    return SP::UI::Declarative::ThemeConfig::Load(space, paths);
}

auto SetActive(PathSpace& space,
               AppRootPathView appRoot,
               std::string_view theme_name) -> SP::Expected<void> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Config::Theme::SetActive");
    return SP::UI::Declarative::ThemeConfig::SetActive(space, appRoot, theme_name);
}

auto LoadActive(PathSpace& space,
                AppRootPathView appRoot) -> SP::Expected<std::string> {
    PATHSPACE_LEGACY_BUILDER_GUARD(space, "Config::Theme::LoadActive");
    return SP::UI::Declarative::ThemeConfig::LoadActive(space, appRoot);
}

} // namespace SP::UI::Builders::Config::Theme
