#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <string>
#include <string_view>

namespace SP::UI::Declarative::ThemeConfig {

using WidgetTheme = SP::UI::Builders::Widgets::WidgetTheme;

struct ThemePaths {
    SP::App::ConcretePath root;
    SP::App::ConcretePath value;
};

auto SanitizeName(std::string_view theme_name) -> std::string;

auto Resolve(SP::App::AppRootPathView app_root,
             std::string_view theme_name) -> SP::Expected<ThemePaths>;

auto Ensure(PathSpace& space,
            SP::App::AppRootPathView app_root,
            std::string_view theme_name,
            WidgetTheme const& defaults) -> SP::Expected<ThemePaths>;

auto Load(PathSpace& space,
          ThemePaths const& paths) -> SP::Expected<WidgetTheme>;

auto SetActive(PathSpace& space,
               SP::App::AppRootPathView app_root,
               std::string_view theme_name) -> SP::Expected<void>;

auto LoadActive(PathSpace& space,
                SP::App::AppRootPathView app_root) -> SP::Expected<std::string>;

} // namespace SP::UI::Declarative::ThemeConfig
