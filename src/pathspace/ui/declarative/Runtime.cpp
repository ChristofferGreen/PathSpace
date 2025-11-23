#include <pathspace/ui/declarative/Runtime.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/declarative/PaintSurfaceUploader.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

struct LocalInputBridge {
    SP::PathSpace* space = nullptr;
    std::string pointer_queue = "/system/devices/in/pointer/default/events";
    std::string keyboard_queue = "/system/devices/in/text/default/events";
};

auto utf32_to_utf8(char32_t ch) -> std::string {
    std::string out;
    if (ch <= 0x7F) {
        out.push_back(static_cast<char>(ch));
        return out;
    }
    if (ch <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        return out;
    }
    if (ch <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        return out;
    }
    out.push_back(static_cast<char>(0xF0 | ((ch >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    return out;
}

auto now_timestamp_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto to_mouse_button(SP::UI::LocalMouseButton button) -> SP::MouseButton {
    using SP::MouseButton;
    using SP::UI::LocalMouseButton;
    switch (button) {
    case LocalMouseButton::Left:
        return MouseButton::Left;
    case LocalMouseButton::Right:
        return MouseButton::Right;
    case LocalMouseButton::Middle:
        return MouseButton::Middle;
    case LocalMouseButton::Button4:
        return MouseButton::Button4;
    case LocalMouseButton::Button5:
        return MouseButton::Button5;
    }
    return MouseButton::Left;
}

auto to_key_modifiers(unsigned int modifiers) -> std::uint32_t {
    using SP::Mod_Alt;
    using SP::Mod_Ctrl;
    using SP::Mod_Meta;
    using SP::Mod_None;
    using SP::Mod_Shift;
    std::uint32_t result = Mod_None;
    if (modifiers & SP::UI::LocalKeyModifierShift) {
        result |= Mod_Shift;
    }
    if (modifiers & SP::UI::LocalKeyModifierControl) {
        result |= Mod_Ctrl;
    }
    if (modifiers & SP::UI::LocalKeyModifierAlt) {
        result |= Mod_Alt;
    }
    if (modifiers & SP::UI::LocalKeyModifierCommand) {
        result |= Mod_Meta;
    }
    return result;
}

void forward_mouse_event(SP::UI::LocalMouseEvent const& event, void* user_data) {
    auto* bridge = static_cast<LocalInputBridge*>(user_data);
    if (!bridge || !bridge->space) {
        return;
    }
    SP::PathIOMouse::Event pointer{};
    pointer.timestampNs = now_timestamp_ns();
    switch (event.type) {
    case SP::UI::LocalMouseEventType::Move:
        pointer.type = SP::MouseEventType::Move;
        pointer.dx = event.dx;
        pointer.dy = event.dy;
        break;
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        pointer.type = SP::MouseEventType::AbsoluteMove;
        pointer.x = event.x;
        pointer.y = event.y;
        break;
    case SP::UI::LocalMouseEventType::ButtonDown:
        pointer.type = SP::MouseEventType::ButtonDown;
        pointer.button = to_mouse_button(event.button);
        pointer.x = event.x;
        pointer.y = event.y;
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        pointer.type = SP::MouseEventType::ButtonUp;
        pointer.button = to_mouse_button(event.button);
        pointer.x = event.x;
        pointer.y = event.y;
        break;
    case SP::UI::LocalMouseEventType::Wheel:
        pointer.type = SP::MouseEventType::Wheel;
        pointer.wheel = event.wheel;
        break;
    }
    (void)bridge->space->insert(bridge->pointer_queue, pointer);
}

void forward_keyboard_event(SP::UI::LocalKeyEvent const& event, void* user_data) {
    auto* bridge = static_cast<LocalInputBridge*>(user_data);
    if (!bridge || !bridge->space) {
        return;
    }
    SP::PathIOKeyboard::Event key{};
    key.timestampNs = now_timestamp_ns();
    key.keycode = static_cast<int>(event.keycode);
    key.modifiers = to_key_modifiers(event.modifiers);
    key.deviceId = 0;
    switch (event.type) {
    case SP::UI::LocalKeyEventType::KeyDown:
        key.type = SP::KeyEventType::KeyDown;
        break;
    case SP::UI::LocalKeyEventType::KeyUp:
        key.type = SP::KeyEventType::KeyUp;
        break;
    }
    (void)bridge->space->insert(bridge->keyboard_queue, key);
    if (event.type == SP::UI::LocalKeyEventType::KeyDown && event.character != U'\0') {
        SP::PathIOKeyboard::Event text = key;
        text.type = SP::KeyEventType::Text;
        text.text = utf32_to_utf8(event.character);
        (void)bridge->space->insert(bridge->keyboard_queue, text);
    }
}

void clear_mouse_state(void*) {
}

void install_local_window_bridge(LocalInputBridge& bridge) {
    SP::UI::LocalWindowCallbacks callbacks{};
    callbacks.mouse_event = &forward_mouse_event;
    callbacks.clear_mouse = &clear_mouse_state;
    callbacks.key_event = &forward_keyboard_event;
    callbacks.user_data = &bridge;
    SP::UI::SetLocalWindowCallbacks(callbacks);
}

void ensure_device_push_config(SP::PathSpace& space,
                              std::string const& device_base,
                              std::string const& subscriber) {
    space.insert(device_base + "/config/push/enabled", true);
    space.insert(device_base + "/config/push/rate_limit_hz", static_cast<std::uint32_t>(480));
    auto subscribers_path = device_base + "/config/push/subscribers/" + subscriber;
    space.insert(subscribers_path, true);
}

void subscribe_window_devices(SP::PathSpace& space,
                              SP::UI::Builders::WindowPath const& window,
                              std::span<const std::string> pointer_devices,
                              std::span<const std::string> text_devices) {
    auto token = SP::Runtime::MakeRuntimeWindowToken(window.getPath());
    std::string base = std::string{"/system/widgets/runtime/windows/"} + token;
    auto set_devices = [&](std::string const& kind, std::span<const std::string> devices) {
        std::vector<std::string> unique;
        for (auto const& device : devices) {
            if (std::find(unique.begin(), unique.end(), device) == unique.end()) {
                unique.push_back(device);
            }
        }
        space.insert(base + "/subscriptions/" + kind + "/devices", unique);
    };
    set_devices("pointer", pointer_devices);
    set_devices("button", pointer_devices);
    set_devices("text", text_devices);
}

struct SceneWindowBinding {
    SP::UI::Builders::WindowPath window_path;
    std::string view_name;
};

auto derive_app_root_from_scene(SP::UI::Builders::ScenePath const& scene_path)
    -> SP::Expected<SP::App::AppRootPath> {
    return SP::App::derive_app_root(SP::App::ConcretePathView{scene_path.getPath()});
}

auto resolve_scene_window_binding(SP::PathSpace& space,
                                 SP::UI::Builders::ScenePath const& scene_path,
                                 SP::App::AppRootPathView app_root)
    -> SP::Expected<SceneWindowBinding> {
    auto windows_root = std::string(scene_path.getPath()) + "/structure/window";
    auto names = space.listChildren(SP::ConcretePathStringView{windows_root});
    if (names.empty()) {
        return std::unexpected(make_error("scene missing window binding",
                                          SP::Error::Code::NotFound));
    }
    auto const& window_component = names.front();
    auto view_value = space.read<std::string, std::string>(windows_root + "/" + window_component + "/view");
    if (!view_value) {
        return std::unexpected(view_value.error());
    }
    auto window_path_str = std::string(app_root.getPath()) + "/windows/" + window_component;
    SceneWindowBinding binding{
        .window_path = SP::UI::Builders::WindowPath{window_path_str},
        .view_name = *view_value,
    };
    return binding;
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

    auto target_absolute = SP::App::resolve_app_relative(app_root, **target_relative);
    if (!target_absolute) {
        return std::unexpected(target_absolute.error());
    }

    auto settings_path = std::string(target_absolute->getPath()) + "/settings";
    auto existing_settings = read_optional<SP::UI::Builders::RenderSettings>(space, settings_path);
    if (!existing_settings) {
        return std::unexpected(existing_settings.error());
    }
    if (!existing_settings->has_value()) {
        SP::UI::Builders::RenderSettings defaults{};
        defaults.surface.size_px.width = surface_params.desc.size_px.width > 0
            ? surface_params.desc.size_px.width
            : (width > 0 ? width : 1280);
        defaults.surface.size_px.height = surface_params.desc.size_px.height > 0
            ? surface_params.desc.size_px.height
            : (height > 0 ? height : 720);
        defaults.surface.dpi_scale = 1.0f;
        defaults.surface.visibility = true;
        defaults.renderer.backend_kind = SP::UI::Builders::RendererKind::Software2D;
        defaults.renderer.metal_uploads_enabled = false;
        defaults.clear_color = {0.11f, 0.12f, 0.15f, 1.0f};

        auto settings_status = SP::UI::Builders::Renderer::UpdateSettings(space,
                                                                          SP::App::ConcretePathView{target_absolute->getPath()},
                                                                          defaults);
        if (!settings_status) {
            return std::unexpected(settings_status.error());
        }
    }

    ViewBinding binding{
        .surface_relative = *surface_relative,
        .renderer_relative = **target_relative,
    };
    return binding;
}

auto build_bootstrap_from_window(SP::PathSpace& space,
                                SP::App::AppRootPathView app_root,
                                SP::UI::Builders::WindowPath const& window,
                                std::string const& view_name)
    -> SP::Expected<SP::UI::Builders::App::BootstrapResult> {
    using namespace SP::UI::Builders;
    App::BootstrapResult bootstrap{};
    bootstrap.window = window;
    bootstrap.view_name = view_name;

    auto renderer_rel = space.read<std::string, std::string>(std::string(window.getPath())
                                                             + "/views/" + view_name + "/renderer");
    if (!renderer_rel) {
        return std::unexpected(renderer_rel.error());
    }
    auto renderer_abs = SP::App::resolve_app_relative(app_root, *renderer_rel);
    if (!renderer_abs) {
        return std::unexpected(renderer_abs.error());
    }
    bootstrap.renderer = RendererPath{renderer_abs->getPath()};

    auto surface_rel = space.read<std::string, std::string>(std::string(window.getPath())
                                                            + "/views/" + view_name + "/surface");
    if (!surface_rel) {
        return std::unexpected(surface_rel.error());
    }
    auto surface_abs = SP::App::resolve_app_relative(app_root, *surface_rel);
    if (!surface_abs) {
        return std::unexpected(surface_abs.error());
    }
    bootstrap.surface = SurfacePath{surface_abs->getPath()};

    auto target_rel = space.read<std::string, std::string>(std::string(bootstrap.surface.getPath()) + "/target");
    if (!target_rel) {
        return std::unexpected(target_rel.error());
    }
    auto target_abs = SP::App::resolve_app_relative(app_root, *target_rel);
    if (!target_abs) {
        return std::unexpected(target_abs.error());
    }
    bootstrap.target = *target_abs;

    auto surface_desc = space.read<SurfaceDesc, std::string>(std::string(bootstrap.surface.getPath()) + "/desc");
    if (!surface_desc) {
        return std::unexpected(surface_desc.error());
    }
    bootstrap.surface_desc = *surface_desc;

    auto settings = SP::UI::Builders::Renderer::ReadSettings(space,
                                                             SP::App::ConcretePathView{bootstrap.target.getPath()});
    if (!settings) {
        return std::unexpected(settings.error());
    }
    bootstrap.applied_settings = *settings;

    SP::UI::PathWindowPresentPolicy policy{};
    auto present_mode_path = std::string(window.getPath()) + "/views/" + view_name + "/present/policy";
    auto present_mode = space.read<std::string, std::string>(present_mode_path);
    if (present_mode) {
        auto const& mode = *present_mode;
        if (mode == "AlwaysFresh") {
            policy.mode = SP::UI::PathWindowPresentMode::AlwaysFresh;
        } else if (mode == "PreferLatestCompleteWithBudget") {
            policy.mode = SP::UI::PathWindowPresentMode::PreferLatestCompleteWithBudget;
        } else {
            policy.mode = SP::UI::PathWindowPresentMode::AlwaysLatestComplete;
        }
    }
    bootstrap.present_policy = policy;
    return bootstrap;
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
        auto started = SP::UI::Declarative::CreateInputTask(space, options.input_task_options);
        if (!started) {
            return std::unexpected(started.error());
        }
        result.input_runtime_started = *started;
        result.input_runtime_state_path = std::string{kInputRuntimeState};
    }

    if (options.start_io_pump) {
        auto pump_started = SP::Runtime::CreateIOPump(space, options.io_pump_options);
        if (!pump_started) {
            return std::unexpected(pump_started.error());
        }
        result.io_pump_started = *pump_started;
        result.io_pump_state_path = options.io_pump_options.state_path.empty()
            ? std::string{"/system/widgets/runtime/io/state/running"}
            : options.io_pump_options.state_path;
    }

    if (options.start_io_telemetry_control) {
        auto telemetry_started = SP::Runtime::CreateTelemetryControl(space,
                                                                     options.telemetry_control_options);
        if (!telemetry_started) {
            return std::unexpected(telemetry_started.error());
        }
        result.telemetry_control_started = *telemetry_started;
        result.telemetry_state_path = options.telemetry_control_options.state_path.empty()
            ? std::string{"/_system/telemetry/io/state/running"}
            : options.telemetry_control_options.state_path;
    }

    if (options.start_widget_event_trellis) {
        auto trellis_started = SP::UI::Declarative::CreateWidgetEventTrellis(space,
                                                                             options.widget_event_options);
        if (!trellis_started) {
            return std::unexpected(trellis_started.error());
        }
        result.widget_event_trellis_started = *trellis_started;
        result.widget_event_trellis_state_path = options.widget_event_options.state_path.empty()
            ? std::string{"/system/widgets/runtime/events/state/running"}
            : options.widget_event_options.state_path;
    }

    if (options.start_paint_gpu_uploader) {
        auto uploader_started = SP::UI::Declarative::CreatePaintSurfaceUploader(space,
                                                                               options.paint_gpu_options);
        if (!uploader_started) {
            return std::unexpected(uploader_started.error());
        }
        result.paint_gpu_uploader_started = *uploader_started;
        result.paint_gpu_state_path = options.paint_gpu_options.state_path;
    }

    return result;
}

auto ShutdownDeclarativeRuntime(PathSpace& space) -> void {
    SP::UI::Declarative::SceneLifecycle::StopAll(space);
    SP::UI::Declarative::ShutdownWidgetEventTrellis(space);
    SP::Runtime::ShutdownIOPump(space);
    SP::Runtime::ShutdownTelemetryControl(space);
    SP::UI::Declarative::ShutdownInputTask(space);
    SP::UI::Declarative::ShutdownPaintSurfaceUploader(space);
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

    auto runtime_token = SP::Runtime::MakeRuntimeWindowToken(window->getPath());
    std::string runtime_base = "/system/widgets/runtime/windows/";
    runtime_base.append(runtime_token);

    if (auto status = ensure_value<std::string>(space,
                                        runtime_base + "/window",
                                        window->getPath());
        !status) {
        return std::unexpected(status.error());
    }

    auto ensure_vector = [&](std::string const& path) -> SP::Expected<void> {
        return ensure_value<std::vector<std::string>>(space, path, std::vector<std::string>{});
    };

    if (auto status = ensure_vector(runtime_base + "/subscriptions/pointer/devices"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_vector(runtime_base + "/subscriptions/button/devices"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_vector(runtime_base + "/subscriptions/text/devices"); !status) {
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

namespace SP::App {

auto RunUI(SP::PathSpace& space,
           SP::Scene::CreateResult const& scene,
           SP::Window::CreateResult const& window,
           RunOptions const& options) -> SP::Expected<void> {
    auto app_root = derive_app_root(SP::App::ConcretePathView{scene.path.getPath()});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }

    auto bootstrap = build_bootstrap_from_window(space,
                                                 SP::App::AppRootPathView{app_root->getPath()},
                                                 window.path,
                                                 window.view_name);
    if (!bootstrap) {
        return std::unexpected(bootstrap.error());
    }

    std::array<std::string, 1> pointer_devices{std::string{"/system/devices/in/pointer/default"}};
    std::array<std::string, 1> keyboard_devices{std::string{"/system/devices/in/text/default"}};
    ensure_device_push_config(space, pointer_devices[0], "app_runui");
    ensure_device_push_config(space, keyboard_devices[0], "app_runui");
    subscribe_window_devices(space,
                             window.path,
                             std::span<const std::string>(pointer_devices.data(), pointer_devices.size()),
                             std::span<const std::string>(keyboard_devices.data(), keyboard_devices.size()));

    LocalInputBridge bridge{};
    bridge.space = &space;
    install_local_window_bridge(bridge);

    int window_width = options.window_width > 0 ? options.window_width
                                                : bootstrap->surface_desc.size_px.width;
    int window_height = options.window_height > 0 ? options.window_height
                                                  : bootstrap->surface_desc.size_px.height;
    std::string title = options.window_title.empty() ? "PathSpace Declarative Window"
                                                     : options.window_title;

    SP::UI::InitLocalWindowWithSize(window_width, window_height, title.c_str());
    auto last_frame = std::chrono::steady_clock::now();
    while (true) {
        SP::UI::PollLocalWindow();
        if (SP::UI::LocalWindowQuitRequested()) {
            break;
        }
        int content_w = window_width;
        int content_h = window_height;
        SP::UI::GetLocalWindowContentSize(&content_w, &content_h);
        if (content_w > 0 && content_h > 0 && (content_w != window_width || content_h != window_height)) {
            window_width = content_w;
            window_height = content_h;
            (void)SP::UI::Builders::App::UpdateSurfaceSize(space,
                                                           *bootstrap,
                                                           window_width,
                                                           window_height);
        }

        auto present_result = SP::UI::Builders::Window::Present(space, window.path, window.view_name);
        if (!present_result) {
            return std::unexpected(present_result.error());
        }
        SP::UI::Builders::App::PresentToLocalWindow(*present_result,
                                                    window_width,
                                                    window_height);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - last_frame;
        if (elapsed < std::chrono::milliseconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4) - elapsed);
        }
        last_frame = now;
    }

    SP::UI::SetLocalWindowCallbacks({});
    return {};
}

auto RunUI(SP::PathSpace& space,
           SP::UI::Builders::ScenePath const& scene_path,
           RunOptions const& options) -> SP::Expected<void> {
    auto app_root = derive_app_root_from_scene(scene_path);
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto binding = resolve_scene_window_binding(space, scene_path, SP::App::AppRootPathView{app_root->getPath()});
    if (!binding) {
        return std::unexpected(binding.error());
    }
    SP::Scene::CreateResult scene_result{
        .path = scene_path,
        .view_name = binding->view_name,
    };
    SP::Window::CreateResult window_result{
        .path = binding->window_path,
        .view_name = binding->view_name,
    };
    return RunUI(space, scene_result, window_result, options);
}

} // namespace SP::App
