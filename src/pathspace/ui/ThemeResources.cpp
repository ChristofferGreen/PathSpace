#include <pathspace/ui/Builders.hpp>
#include "BuildersDetail.hpp"

#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <pathspace/app/AppPaths.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>

namespace SP::UI::Builders::Config::Theme {

using namespace Detail;
using Widgets::WidgetTheme;

namespace {

constexpr std::string_view kThemeSegment = "/config/theme/";
constexpr std::size_t kMaxThemeInheritanceDepth = 16;

auto extract_app_root(std::string const& theme_root) -> SP::Expected<std::string> {
    auto pos = theme_root.find(kThemeSegment);
    if (pos == std::string::npos) {
        return std::unexpected(make_error("theme root path missing '/config/theme/' segment",
                                          SP::Error::Code::InvalidPath));
    }
    if (pos == 0) {
        return std::unexpected(make_error("theme root missing application prefix",
                                          SP::Error::Code::InvalidPath));
    }
    return theme_root.substr(0, pos);
}

auto extract_theme_component(std::string const& theme_root) -> SP::Expected<std::string> {
    auto slash = theme_root.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= theme_root.size()) {
        return std::unexpected(make_error("theme root missing terminal component",
                                          SP::Error::Code::InvalidPath));
    }
    return theme_root.substr(slash + 1);
}

auto make_theme_root(std::string const& app_root, std::string const& sanitized) -> std::string {
    std::string root = app_root;
    root.append(kThemeSegment);
    root.append(sanitized);
    return root;
}

} // namespace

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
    auto app_root = extract_app_root(paths.root.getPath());
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto initial_theme = extract_theme_component(paths.root.getPath());
    if (!initial_theme) {
        return std::unexpected(initial_theme.error());
    }

    std::unordered_set<std::string> visited;
    visited.reserve(4);
    std::optional<WidgetTheme> resolved;

    std::string current = SanitizeName(*initial_theme);
    for (std::size_t depth = 0; depth < kMaxThemeInheritanceDepth; ++depth) {
        if (!visited.insert(current).second) {
            return std::unexpected(make_error("theme inheritance cycle detected at '" + current + "'",
                                              SP::Error::Code::InvalidType));
        }

        auto theme_root = make_theme_root(*app_root, current);
        auto value_path = theme_root + "/value";
        auto loaded = space.read<WidgetTheme, std::string>(value_path);
        if (loaded) {
            if (!resolved) {
                resolved = *loaded;
            }
        } else {
            auto const code = loaded.error().code;
            if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
                return std::unexpected(loaded.error());
            }
        }

        auto inherits_path = theme_root + "/style/inherits";
        auto inherits = space.read<std::string, std::string>(inherits_path);
        if (!inherits) {
            auto const code = inherits.error().code;
            if (code == SP::Error::Code::NoSuchPath || code == SP::Error::Code::NoObjectFound) {
                if (resolved) {
                    return *resolved;
                }
                return std::unexpected(make_error("theme '" + current + "' missing value and inherits",
                                                  SP::Error::Code::NoSuchPath));
            }
            return std::unexpected(inherits.error());
        }

        auto sanitized_parent = SanitizeName(*inherits);
        if (sanitized_parent.empty()) {
            return std::unexpected(make_error("theme '" + current + "' inherits invalid theme name",
                                              SP::Error::Code::InvalidType));
        }
        current = std::move(sanitized_parent);
    }

    return std::unexpected(make_error("theme inheritance depth exceeded",
                                      SP::Error::Code::CapacityExceeded));
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
