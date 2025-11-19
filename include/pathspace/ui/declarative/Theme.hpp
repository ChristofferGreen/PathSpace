#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Declarative::Theme {

using WidgetTheme = SP::UI::Builders::Widgets::WidgetTheme;

struct CreateOptions {
    std::string name;
    std::optional<std::string> inherits;
    std::optional<WidgetTheme> seed_theme;
    bool overwrite_existing_value = false;
    bool populate_tokens = true;
    bool set_active = false;
};

struct CreateResult {
    std::string canonical_name;
    SP::App::ConcretePath edit_root;
};

struct ColorValue {
    std::array<float, 4> rgba{};
};

auto Create(PathSpace& space,
            SP::App::AppRootPathView app_root,
            CreateOptions const& options) -> SP::Expected<CreateResult>;

auto SetColor(PathSpace& space,
              SP::App::AppRootPathView app_root,
              std::string_view theme_name,
              std::string_view token,
              ColorValue const& value) -> SP::Expected<void>;

auto RebuildValue(PathSpace& space,
                  SP::App::AppRootPathView app_root,
                  std::string_view theme_name) -> SP::Expected<void>;

} // namespace SP::UI::Declarative::Theme
