#include <pathspace/ui/declarative/Runtime.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace SP::UI::Builders::Detail;
using SP::PathSpace;
using ScenePath = SP::UI::Builders::ScenePath;
using WindowPath = SP::UI::Builders::WindowPath;

constexpr auto kSystemLaunchFlag = std::string_view{"/system/state/runtime_launched"};
constexpr auto kInputRuntimeState = std::string_view{"/system/widgets/runtime/input/state"};
constexpr auto kRendererConfigSuffix = std::string_view{"/config/renderer/default"};
constexpr auto kDefaultRendererName = std::string_view{"widgets_declarative_renderer"};
constexpr auto kDefaultSurfacePrefix = std::string_view{"widgets_surface"};
constexpr auto kDefaultThemeActive = std::string_view{"/config/theme/active"};

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

auto ensure_theme(PathSpace& space,
                  SP::App::AppRootPathView app_root,
                  std::string_view requested) -> SP::Expected<std::string> {
    std::string normalized = requested.empty() ? "default" : std::string(requested);
    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(normalized);
    auto defaults = sanitized == "sunset"
        ? SP::UI::Builders::Widgets::MakeSunsetWidgetTheme()
        : SP::UI::Builders::Widgets::MakeDefaultWidgetTheme();

    auto ensured = SP::UI::Builders::Config::Theme::Ensure(space, app_root, sanitized, defaults);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    if (auto status = SP::UI::Builders::Config::Theme::SetActive(space, app_root, sanitized); !status) {
        return std::unexpected(status.error());
    }
    return sanitized;
}

auto renderer_config_path(SP::App::AppRootPathView app_root) -> std::string {
    return std::string(app_root.getPath()) + std::string{kRendererConfigSuffix};
}

struct RendererBootstrap {
    SP::UI::Builders::RendererPath renderer_path;
    std::string renderer_relative;
};

auto ensure_renderer(PathSpace& space,
                     SP::App::AppRootPathView app_root,
                     std::string_view renderer_name) -> SP::Expected<RendererBootstrap> {
    SP::UI::Builders::RendererParams params{};
    params.name = renderer_name.empty() ? std::string{kDefaultRendererName} : std::string(renderer_name);
    params.kind = SP::UI::Builders::RendererKind::Software2D;
    params.description = "Declarative widget renderer";
    auto renderer = SP::UI::Builders::Renderer::Create(space, app_root, params);
    if (!renderer) {
        return std::unexpected(renderer.error());
    }
    auto relative = make_relative(app_root, SP::App::ConcretePathView{renderer->getPath()});
    if (!relative) {
        return std::unexpected(relative.error());
    }
    auto config_path = renderer_config_path(app_root);
    if (auto status = ensure_value<std::string>(space, config_path, *relative); !status) {
        return std::unexpected(status.error());
    }
    RendererBootstrap bootstrap{
        .renderer_path = *renderer,
        .renderer_relative = *relative,
    };
    return bootstrap;
}

auto read_renderer_relative(PathSpace& space,
                            SP::App::AppRootPathView app_root) -> SP::Expected<std::string> {
    auto config_path = renderer_config_path(app_root);
    auto stored = read_optional<std::string>(space, config_path);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (stored->has_value()) {
        return **stored;
    }
    auto ensured = ensure_renderer(space, app_root, kDefaultRendererName);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    return ensured->renderer_relative;
}

struct ViewBinding {
    std::string surface_relative;
    std::string renderer_relative;
};

auto make_surface_name(std::string const& window_name,
                       std::string const& view_name) -> std::string {
    std::string name{kDefaultSurfacePrefix};
    name.push_back('_');
    name.append(window_name);
    name.push_back('_');
    name.append(view_name);
    return name;
}

auto ensure_view_binding(PathSpace& space,
                         SP::App::AppRootPathView app_root,
                         std::string const& window_name,
                         std::string const& view_name,
                         int width,
                         int height,
                         std::string const& renderer_relative) -> SP::Expected<ViewBinding> {
    auto renderer_absolute = SP::App::resolve_app_relative(app_root, renderer_relative);
    if (!renderer_absolute) {
        return std::unexpected(renderer_absolute.error());
    }

    SP::UI::Builders::SurfaceParams surface_params{};
    surface_params.name = make_surface_name(window_name, view_name);
    surface_params.renderer = renderer_relative;
    surface_params.desc.size_px.width = width > 0 ? width : 1280;
    surface_params.desc.size_px.height = height > 0 ? height : 720;

    auto surface = SP::UI::Builders::Surface::Create(space, app_root, surface_params);
    if (!surface) {
        return std::unexpected(surface.error());
    }

    auto surface_relative = make_relative(app_root, SP::App::ConcretePathView{surface->getPath()});
    if (!surface_relative) {
        return std::unexpected(surface_relative.error());
    }

    auto target_field = std::string(surface->getPath()) + "/target";
    auto target_relative = read_optional<std::string>(space, target_field);
    if (!target_relative) {
        return std::unexpected(target_relative.error());
    }
    if (!target_relative->has_value()) {
        return std::unexpected(make_error("surface target missing",
                                          SP::Error::Code::InvalidPath));
    }

    ViewBinding binding{
        .surface_relative = *surface_relative,
        .renderer_relative = **target_relative,
    };
    return binding;
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

    auto theme_name = options.default_theme_name.empty()
        ? std::string{"default"}
        : options.default_theme_name;
    result.default_theme_path = std::string{"/system/themes/"} + theme_name;
    auto name_path = result.default_theme_path + "/name";
    if (auto status = ensure_value<std::string>(space, name_path, theme_name); !status) {
        return std::unexpected(status.error());
    }
    auto active_path = result.default_theme_path + "/active";
    if (auto status = ensure_value<bool>(space, active_path, true); !status) {
        return std::unexpected(status.error());
    }

    if (options.start_input_runtime) {
        auto started = SP::UI::Declarative::EnsureInputTask(space, options.input_task_options);
        if (!started) {
            return std::unexpected(started.error());
        }
        result.input_runtime_started = *started;
        result.input_runtime_state_path = std::string{kInputRuntimeState};
    }

    return result;
}

auto ShutdownDeclarativeRuntime(PathSpace& space) -> void {
    SP::UI::Declarative::ShutdownInputTask(space);
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

    auto app_root_view = SP::App::AppRootPathView{normalized->getPath()};
    auto canonical_theme = ensure_theme(space, app_root_view, options.default_theme);
    if (!canonical_theme) {
        return std::unexpected(canonical_theme.error());
    }

    auto default_theme_path = std::string(normalized->getPath()) + "/themes/default";
    if (auto status = ensure_value<std::string>(space, default_theme_path, *canonical_theme); !status) {
        return std::unexpected(status.error());
    }

    auto renderer_bootstrap = ensure_renderer(space, app_root_view, kDefaultRendererName);
    if (!renderer_bootstrap) {
        return std::unexpected(renderer_bootstrap.error());
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
    auto active_theme = SP::UI::Builders::Config::Theme::LoadActive(space, app_root);
    if (active_theme) {
        (void)replace_single<std::string>(space, base + "/style/theme", *active_theme);
    } else {
        auto const& err = active_theme.error();
        if (err.code != SP::Error::Code::NoObjectFound
            && err.code != SP::Error::Code::NoSuchPath) {
            return std::unexpected(err);
        }
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

    auto renderer_relative = read_renderer_relative(space, app_root);
    if (!renderer_relative) {
        return std::unexpected(renderer_relative.error());
    }
    auto binding = ensure_view_binding(space,
                                       app_root,
                                       *name,
                                       *view,
                                       params.width,
                                       params.height,
                                       *renderer_relative);
    if (!binding) {
        return std::unexpected(binding.error());
    }
    if (auto status = replace_single<std::string>(space,
                                                  view_base + "/surface",
                                                  binding->surface_relative);
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space,
                                                  view_base + "/renderer",
                                                  binding->renderer_relative);
        !status) {
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

    auto view_base = std::string(window_path.getPath()) + "/views/" + *view;
    auto view_surface = read_optional<std::string>(space, view_base + "/surface");
    if (!view_surface) {
        return std::unexpected(view_surface.error());
    }
    if (view_surface->has_value()) {
        if (auto status = ensure_value<std::string>(space,
                                                    structure_base + "/surface",
                                                    **view_surface);
            !status) {
            return std::unexpected(status.error());
        }
    }
    auto view_renderer = read_optional<std::string>(space, view_base + "/renderer");
    if (!view_renderer) {
        return std::unexpected(view_renderer.error());
    }
    if (view_renderer->has_value()) {
        if (auto status = ensure_value<std::string>(space,
                                                    structure_base + "/renderer",
                                                    **view_renderer);
            !status) {
            return std::unexpected(status.error());
        }
    }
    auto present_relative = std::string("windows/")
                            + window_component + "/views/" + *view + "/present";
    if (auto status = ensure_value<std::string>(space,
                                                structure_base + "/present",
                                                present_relative);
        !status) {
        return std::unexpected(status.error());
    }

    auto relative_scene = make_relative(app_root, SP::App::ConcretePathView{scene->getPath()});
    if (!relative_scene) {
        return std::unexpected(relative_scene.error());
    }

    if (options.attach_to_window) {
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

    auto lifecycle = SP::UI::Declarative::SceneLifecycle::Start(space,
                                                                app_root,
                                                                result.path,
                                                                window_path,
                                                                *view);
    if (!lifecycle) {
        return std::unexpected(lifecycle.error());
    }
    return result;
}

auto Shutdown(PathSpace& space,
              SP::UI::Builders::ScenePath const& scene_path) -> SP::Expected<void> {
    return SP::UI::Declarative::SceneLifecycle::Stop(space, scene_path);
}

} // namespace SP::Scene
