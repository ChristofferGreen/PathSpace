#include <pathspace/ui/Builders.hpp>
#include "BuildersDetail.hpp"

#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <pathspace/app/AppPaths.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace SP::UI::Builders::Config::Theme {

using namespace Detail;
using Widgets::WidgetTheme;

auto SanitizeName(std::string_view name) -> std::string {
    std::string sanitized;
    sanitized.reserve(name.size());
    for (unsigned char ch : name) {
        if (std::isalnum(ch) != 0) {
            sanitized.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == '_' || ch == '-') {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) {
        sanitized = "default";
    }
    return sanitized;
}

namespace {

auto make_paths(AppRootPathView appRoot,
                std::string_view theme_name) -> SP::Expected<ThemePaths> {
    if (auto status = ensure_identifier(theme_name, "theme name"); !status) {
        return std::unexpected(status.error());
    }

    std::string relative{"config/theme/"};
    relative.append(theme_name);

    auto root = SP::App::resolve_app_relative(appRoot, relative);
    if (!root) {
        return std::unexpected(root.error());
    }

    ThemePaths paths{};
    paths.root = *root;
    paths.value = ConcretePath{root->getPath() + "/value"};
    return paths;
}

auto active_theme_path(AppRootPathView appRoot) -> SP::Expected<ConcretePath> {
    auto path = SP::App::resolve_app_relative(appRoot, "config/theme/active");
    if (!path) {
        return std::unexpected(path.error());
    }
    return *path;
}

} // namespace

auto Resolve(AppRootPathView appRoot,
             std::string_view theme_name) -> SP::Expected<ThemePaths> {
    auto sanitized = SanitizeName(theme_name);
    return make_paths(appRoot, sanitized);
}

auto Ensure(PathSpace& space,
            AppRootPathView appRoot,
            std::string_view theme_name,
            WidgetTheme const& defaults) -> SP::Expected<ThemePaths> {
    auto sanitized = SanitizeName(theme_name);
    auto paths = make_paths(appRoot, sanitized);
    if (!paths) {
        return paths;
    }

    auto existing = space.read<WidgetTheme, std::string>(paths->value.getPath());
    if (!existing) {
        auto const code = existing.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(existing.error());
        }
        if (auto status = replace_single<WidgetTheme>(space, paths->value.getPath(), defaults); !status) {
            return std::unexpected(status.error());
        }
    }

    return *paths;
}

auto Load(PathSpace& space,
          ThemePaths const& paths) -> SP::Expected<WidgetTheme> {
    return space.read<WidgetTheme, std::string>(paths.value.getPath());
}

auto SetActive(PathSpace& space,
               AppRootPathView appRoot,
               std::string_view theme_name) -> SP::Expected<void> {
    auto sanitized = SanitizeName(theme_name);
    auto active = active_theme_path(appRoot);
    if (!active) {
        return std::unexpected(active.error());
    }
    if (auto status = replace_single<std::string>(space, active->getPath(), sanitized); !status) {
        return std::unexpected(status.error());
    }
    SP::UI::Declarative::SceneLifecycle::InvalidateThemes(space, appRoot);
    return {};
}

auto LoadActive(PathSpace& space,
                AppRootPathView appRoot) -> SP::Expected<std::string> {
    auto active = active_theme_path(appRoot);
    if (!active) {
        return std::unexpected(active.error());
    }
    return space.read<std::string, std::string>(active->getPath());
}

} // namespace SP::UI::Builders::Config::Theme
