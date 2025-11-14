#include <pathspace/ui/declarative/Runtime.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/Helpers.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace SP::UI::Builders::Detail;
using ScenePath = SP::UI::Builders::ScenePath;
using WindowPath = SP::UI::Builders::WindowPath;

constexpr auto kSystemLaunchFlag = std::string_view{"/system/state/runtime_launched"};

template <typename T>
auto ensure_value(SP::PathSpace& space,
                 std::string const& path,
                 T const& value) -> SP::Expected<void> {
    auto existing = read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return replace_single<T>(space, path, value);
}

auto make_identifier(std::string_view raw,
                     std::string_view label) -> SP::Expected<std::string> {
    if (raw.empty()) {
        return std::unexpected(make_error(std::string(label) + " must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    std::string sanitized;
    sanitized.reserve(raw.size());
    for (char ch : raw) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            sanitized.push_back(ch);
            continue;
        }
        return std::unexpected(make_error(std::string(label)
                                              + " must contain only alphanumeric, '-' or '_' characters",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    return sanitized;
}

auto make_relative(SP::App::AppRootPathView app_root,
                   SP::App::ConcretePathView absolute) -> SP::Expected<std::string> {
    auto const& root = app_root.getPath();
    auto const& target = absolute.getPath();
    if (target == root) {
        return std::string{};
    }
    if (target.size() < root.size() + 1 || target[root.size()] != '/') {
        return std::unexpected(make_error("path does not fall within the application root",
                                          SP::Error::Code::InvalidPath));
    }
    return std::string(target.substr(root.size() + 1));
}

auto extract_component(SP::App::ConcretePathView path) -> std::string {
    std::string value{path.getPath()};
    auto slash = value.find_last_of('/');
    if (slash == std::string::npos) {
        return value;
    }
    return value.substr(slash + 1);
}

} // namespace

namespace SP::System {

auto LaunchStandard(PathSpace& space, LaunchOptions const& options) -> SP::Expected<LaunchResult> {
    LaunchResult result{};

    auto existing_flag = read_optional<bool>(space, std::string{kSystemLaunchFlag});
    if (!existing_flag) {
        return std::unexpected(existing_flag.error());
    }
    result.already_launched = existing_flag->value_or(false);

    if (!result.already_launched) {
        auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
        auto timestamp = static_cast<std::uint64_t>(now.time_since_epoch().count());
        if (auto status = replace_single<bool>(space,
                                               std::string{kSystemLaunchFlag},
                                               true);
            !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<std::uint64_t>(space,
                                                        "/system/state/launch_time_ms",
                                                        timestamp);
            !status) {
            return std::unexpected(status.error());
        }
    }

    if (!options.default_theme_name.empty()) {
        result.default_theme_path = std::string{"/system/themes/"} + options.default_theme_name;
        auto name_path = result.default_theme_path + "/name";
        if (auto status = ensure_value<std::string>(space, name_path, options.default_theme_name); !status) {
            return std::unexpected(status.error());
        }
        auto active_path = result.default_theme_path + "/active";
        if (auto status = ensure_value<bool>(space, active_path, true); !status) {
            return std::unexpected(status.error());
        }
    }

    return result;
}

} // namespace SP::System

namespace SP::App {

auto Create(PathSpace& space,
            std::string_view app_name,
            CreateOptions const& options) -> SP::Expected<AppRootPath> {
    auto identifier = make_identifier(app_name, "application name");
    if (!identifier) {
        return std::unexpected(identifier.error());
    }

    std::string absolute_root{"/system/applications/"};
    absolute_root.append(*identifier);
    auto normalized = normalize_app_root(AppRootPathView{absolute_root});
    if (!normalized) {
        return std::unexpected(normalized.error());
    }

    auto title = options.title.empty() ? std::string{*identifier} : options.title;
    auto title_path = std::string(normalized->getPath()) + "/state/title";
    if (auto status = ensure_value<std::string>(space, title_path, title); !status) {
        return std::unexpected(status.error());
    }

    if (!options.default_theme.empty()) {
        auto default_theme_path = std::string(normalized->getPath()) + "/themes/default";
        if (auto status = ensure_value<std::string>(space, default_theme_path, options.default_theme); !status) {
            return std::unexpected(status.error());
        }
    }

    return *normalized;
}

} // namespace SP::App

namespace SP::Window {

auto Create(PathSpace& space,
            SP::App::AppRootPathView app_root,
            CreateOptions const& options) -> SP::Expected<CreateResult> {
    auto name = make_identifier(options.name, "window name");
    if (!name) {
        return std::unexpected(name.error());
    }
    auto view = make_identifier(options.view, "view name");
    if (!view) {
        return std::unexpected(view.error());
    }

    SP::UI::Builders::WindowParams params{};
    params.name = *name;
    params.title = options.title.empty() ? params.name : options.title;
    params.width = options.width > 0 ? options.width : 1280;
    params.height = options.height > 0 ? options.height : 720;
    params.scale = options.scale > 0.0f ? options.scale : 1.0f;
    params.background = options.background.empty() ? "#101218" : options.background;

    SP::App::AppRootPath app_root_value{std::string(app_root.getPath())};
    auto window = SP::UI::Window::Create(space, app_root_value, params);
    if (!window) {
        return std::unexpected(window.error());
    }

    auto base = std::string(window->getPath());
    if (auto status = ensure_value<bool>(space, base + "/state/visible", options.visible); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<bool>(space, base + "/render/dirty", false); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<std::string>(space, base + "/style/theme", std::string{}); !status) {
        return std::unexpected(status.error());
    }

    auto view_base = base + "/views/" + *view;
    if (auto status = ensure_value<std::string>(space, view_base + "/scene", std::string{}); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<std::string>(space, view_base + "/surface", std::string{}); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<std::string>(space, view_base + "/htmlTarget", std::string{}); !status) {
        return std::unexpected(status.error());
    }

    CreateResult result{};
    result.path = *window;
    result.view_name = *view;
    return result;
}

} // namespace SP::Window

namespace SP::Scene {

auto Create(PathSpace& space,
            SP::App::AppRootPathView app_root,
            WindowPath const& window_path,
            CreateOptions const& options) -> SP::Expected<CreateResult> {
    auto view = make_identifier(options.view, "view name");
    if (!view) {
        return std::unexpected(view.error());
    }

    if (auto status = SP::App::ensure_within_app(app_root, SP::App::ConcretePathView{window_path.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    std::string scene_name;
    if (!options.name.empty()) {
        auto validated = make_identifier(options.name, "scene name");
        if (!validated) {
            return std::unexpected(validated.error());
        }
        scene_name = *validated;
    } else {
        scene_name = extract_component(SP::App::ConcretePathView{window_path.getPath()});
        scene_name.append("_scene");
    }

    SP::UI::SceneParams params{};
    params.name = scene_name;
    params.description = options.description.empty() ? (scene_name + " scene") : options.description;

    SP::App::AppRootPath app_root_value{std::string(app_root.getPath())};
    auto scene = SP::UI::Scene::Create(space, app_root_value, params);
    if (!scene) {
        return std::unexpected(scene.error());
    }

    auto base = std::string(scene->getPath());
    if (auto status = ensure_value<bool>(space, base + "/render/dirty", false); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<bool>(space, base + "/state/attached", options.attach_to_window); !status) {
        return std::unexpected(status.error());
    }

    auto window_component = extract_component(SP::App::ConcretePathView{window_path.getPath()});
    auto structure_base = base + "/structure/window/" + window_component;
    if (auto status = ensure_value<std::string>(space, structure_base + "/view", *view); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<std::string>(space, structure_base + "/focus/current", std::string{}); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_value<double>(space, structure_base + "/metrics/dpi", 1.0); !status) {
        return std::unexpected(status.error());
    }

    auto relative_scene = make_relative(app_root, SP::App::ConcretePathView{scene->getPath()});
    if (!relative_scene) {
        return std::unexpected(relative_scene.error());
    }

    if (options.attach_to_window) {
        auto view_base = std::string(window_path.getPath()) + "/views/" + *view;
        if (auto status = replace_single<std::string>(space,
                                                      view_base + "/scene",
                                                      *relative_scene);
            !status) {
            return std::unexpected(status.error());
        }
    }

    CreateResult result{};
    result.path = *scene;
    result.view_name = *view;
    return result;
}

} // namespace SP::Scene
