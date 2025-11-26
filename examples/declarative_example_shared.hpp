#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>

namespace PathSpaceExamples {

struct LocalInputBridge {
    SP::PathSpace* space = nullptr;
    std::string pointer_queue = "/system/devices/in/pointer/default/events";
    std::string keyboard_queue = "/system/devices/in/text/default/events";
};

inline auto utf32_to_utf8(char32_t ch) -> std::string {
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

inline auto now_timestamp_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

inline auto to_mouse_button(SP::UI::LocalMouseButton button) -> SP::MouseButton {
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

inline auto to_key_modifiers(unsigned int modifiers) -> std::uint32_t {
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

inline void forward_mouse_event(SP::UI::LocalMouseEvent const& event, void* user_data) {
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

inline void forward_keyboard_event(SP::UI::LocalKeyEvent const& event, void* user_data) {
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

inline void clear_mouse_state(void*) {
    // Nothing to do; declarative runtime tracks state per widget.
}

inline void install_local_window_bridge(LocalInputBridge& bridge) {
    SP::UI::LocalWindowCallbacks callbacks{};
    callbacks.mouse_event = &forward_mouse_event;
    callbacks.clear_mouse = &clear_mouse_state;
    callbacks.key_event = &forward_keyboard_event;
    callbacks.user_data = &bridge;
    SP::UI::SetLocalWindowCallbacks(callbacks);
}

inline auto build_bootstrap_from_window(SP::PathSpace& space,
                                        SP::App::AppRootPathView app_root,
                                        SP::UI::Builders::WindowPath const& window,
                                        std::string const& view_name)
    -> SP::Expected<SP::UI::Builders::App::BootstrapResult> {
    using namespace SP::UI::Builders;
    App::BootstrapResult bootstrap{};
    bootstrap.window = window;
    bootstrap.view_name = view_name;

    auto renderer_rel = space.read<std::string, std::string>(
        std::string(window.getPath()) + "/views/" + view_name + "/renderer");
    if (!renderer_rel) {
        return std::unexpected(renderer_rel.error());
    }
    auto renderer_abs = SP::App::resolve_app_relative(app_root, *renderer_rel);
    if (!renderer_abs) {
        return std::unexpected(renderer_abs.error());
    }
    bootstrap.renderer = RendererPath{renderer_abs->getPath()};

    auto surface_rel = space.read<std::string, std::string>(
        std::string(window.getPath()) + "/views/" + view_name + "/surface");
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

    auto surface_desc = space.read<SurfaceDesc, std::string>(
        std::string(bootstrap.surface.getPath()) + "/desc");
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

    // Present policy nodes may not exist yet; default to AlwaysLatestComplete.
    SP::UI::PathWindowPresentPolicy policy{};
    auto present_mode_path = std::string(window.getPath()) + "/views/" + view_name + "/present/policy";
    auto present_mode = space.read<std::string, std::string>(present_mode_path);
    if (!present_mode) {
        auto const& error = present_mode.error();
        if (error.code != SP::Error::Code::NoObjectFound
            && error.code != SP::Error::Code::NoSuchPath) {
            return std::unexpected(error);
        }
    } else {
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

inline void subscribe_window_devices(SP::PathSpace& space,
                                     SP::UI::Builders::WindowPath const& window,
                                     std::span<std::string const> pointer_devices,
                                     std::span<std::string const> button_devices,
                                     std::span<std::string const> text_devices) {
    auto token = SP::Runtime::MakeRuntimeWindowToken(window.getPath());
    std::string base = std::string{"/system/widgets/runtime/windows/"} + token;
    auto set_devices = [&](std::string const& kind, std::span<std::string const> devices) {
        std::vector<std::string> unique;
        for (auto const& device : devices) {
            if (std::find(unique.begin(), unique.end(), device) == unique.end()) {
                unique.push_back(device);
            }
        }
        space.insert(base + "/subscriptions/" + kind + "/devices", unique);
    };
    set_devices("pointer", pointer_devices);
    set_devices("button", button_devices);
    set_devices("text", text_devices);
}

inline void ensure_device_push_config(SP::PathSpace& space,
                                      std::string const& device_base,
                                      std::string const& subscriber) {
    space.insert(device_base + "/config/push/enabled", true);
    space.insert(device_base + "/config/push/rate_limit_hz", static_cast<std::uint32_t>(480));
    auto subscribers_path = device_base + "/config/push/subscribers/" + subscriber;
    space.insert(subscribers_path, true);
}

struct PresentLoopHooks {
    std::function<void()> before_present;
    std::function<void()> after_present;
    std::function<void()> per_frame;
    std::function<void(SP::UI::Builders::Window::WindowPresentResult const&)> on_present;
};

inline void run_present_loop(SP::PathSpace& space,
                             SP::UI::Builders::WindowPath const& window,
                             std::string const& view_name,
                             SP::UI::Builders::App::BootstrapResult& bootstrap,
                             int initial_width,
                             int initial_height,
                             PresentLoopHooks hooks = {}) {
    int window_width = initial_width;
    int window_height = initial_height;
    SP::UI::InitLocalWindowWithSize(window_width, window_height, "PathSpace Declarative Window");
    auto last_frame = std::chrono::steady_clock::now();
    while (true) {
        if (hooks.per_frame) {
            hooks.per_frame();
        }
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
                                                           bootstrap,
                                                           window_width,
                                                           window_height);
        }
        if (hooks.before_present) {
            hooks.before_present();
        }
        auto present_result = SP::UI::Builders::Window::Present(space, window, view_name);
        if (present_result) {
            if (hooks.on_present) {
                hooks.on_present(*present_result);
            }
            SP::UI::Builders::App::PresentToLocalWindow(*present_result,
                                                        window_width,
                                                        window_height);
        }
        if (hooks.after_present) {
            hooks.after_present();
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - last_frame;
        if (elapsed < std::chrono::milliseconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4) - elapsed);
        }
        last_frame = now;
    }
}

struct DeclarativeReadinessOptions {
    std::chrono::milliseconds widget_timeout{std::chrono::milliseconds(5000)};
    std::chrono::milliseconds revision_timeout{std::chrono::milliseconds(3000)};
    bool wait_for_structure = true;
    bool wait_for_buckets = true;
    bool wait_for_revision = true;
    bool wait_for_runtime_metrics = false;
    std::chrono::milliseconds runtime_metrics_timeout{std::chrono::milliseconds(2000)};
    std::optional<std::uint64_t> min_revision;
    bool ensure_scene_window_mirror = false;
    std::optional<std::string> scene_window_component_override;
    std::optional<std::string> scene_view_override;
    bool force_scene_publish = false;
    bool pump_scene_before_force_publish = true;
    SP::UI::Declarative::SceneLifecycle::ManualPumpOptions scene_pump_options{};
};

struct DeclarativeReadinessResult {
    std::size_t widget_count = 0;
    std::optional<std::uint64_t> scene_revision;
};

inline auto make_window_view_path(SP::UI::Builders::WindowPath const& window,
                                 std::string const& view_name) -> std::string {
    std::string view_path = std::string(window.getPath());
    view_path.append("/views/");
    view_path.append(view_name);
    return view_path;
}

inline auto window_component_name(std::string const& window_path) -> std::string {
    auto slash = window_path.find_last_of('/');
    if (slash == std::string::npos) {
        return window_path;
    }
    return window_path.substr(slash + 1);
}

inline auto app_root_from_window(SP::UI::Builders::WindowPath const& window) -> std::string {
    auto full = std::string(window.getPath());
    auto windows_pos = full.find("/windows/");
    if (windows_pos == std::string::npos) {
        return {};
    }
    return full.substr(0, windows_pos);
}

inline auto make_scene_widgets_root_components(SP::UI::Builders::ScenePath const& scene,
                                               std::string_view window_component,
                                               std::string_view view_name) -> std::string {
    std::string root = std::string(scene.getPath());
    root.append("/structure/widgets/windows/");
    root.append(window_component);
    root.append("/views/");
    root.append(view_name);
    root.append("/widgets");
    return root;
}

inline auto make_scene_widgets_root(SP::UI::Builders::ScenePath const& scene,
                                    SP::UI::Builders::WindowPath const& window,
                                    std::string const& view_name) -> std::string {
    auto window_component = window_component_name(std::string(window.getPath()));
    return make_scene_widgets_root_components(scene, window_component, view_name);
}

inline auto force_window_software_renderer(SP::PathSpace& space,
                                           SP::UI::Builders::WindowPath const& window,
                                           std::string const& view_name) -> SP::Expected<void> {
    auto view_base = std::string(window.getPath()) + "/views/" + view_name;
    auto renderer_rel = space.read<std::string, std::string>(view_base + "/renderer");
    if (!renderer_rel) {
        return std::unexpected(renderer_rel.error());
    }
    if (renderer_rel->empty()) {
        return {};
    }
    auto app_root = app_root_from_window(window);
    if (app_root.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "window missing app root"});
    }
    std::string renderer_abs = app_root;
    renderer_abs.push_back('/');
    renderer_abs.append(*renderer_rel);
    auto renderer_view = SP::ConcretePathStringView{renderer_abs};
    auto settings = SP::UI::Builders::Renderer::ReadSettings(space, renderer_view);
    if (!settings) {
        return std::unexpected(settings.error());
    }
    if (!settings->renderer.metal_uploads_enabled) {
        return {};
    }
    settings->renderer.metal_uploads_enabled = false;
    return SP::UI::Builders::Renderer::UpdateSettings(space, renderer_view, *settings);
}

inline auto count_window_widgets(SP::PathSpace& space,
                                 SP::UI::Builders::WindowPath const& window,
                                 std::string const& view_name) -> std::size_t {
    auto widgets_root = make_window_view_path(window, view_name) + "/widgets";
    auto children = space.listChildren(SP::ConcretePathStringView{widgets_root});
    return children.size();
}

inline auto wait_for_runtime_metric_visible(SP::PathSpace& space,
                                            std::string const& metric_path,
                                            std::chrono::milliseconds timeout) -> SP::Expected<void> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = space.read<std::uint64_t, std::string>(metric_path);
        if (value) {
            return {};
        }
        auto const& error = value.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "runtime metric path did not appear: " + metric_path});
}

inline auto wait_for_runtime_metrics_ready(SP::PathSpace& space,
                                           std::chrono::milliseconds timeout) -> SP::Expected<void> {
    constexpr std::string_view kInputMetric =
        "/system/widgets/runtime/input/metrics/widgets_processed_total";
    constexpr std::string_view kWidgetOpsMetric =
        "/system/widgets/runtime/events/metrics/widget_ops_total";
    if (auto status = wait_for_runtime_metric_visible(space, std::string(kInputMetric), timeout);
        !status) {
        return status;
    }
    return wait_for_runtime_metric_visible(space, std::string(kWidgetOpsMetric), timeout);
}

inline auto wait_for_declarative_scene_widgets(SP::PathSpace& space,
                                               std::string const& widgets_root,
                                               std::size_t expected_widgets,
                                               std::chrono::milliseconds timeout) -> SP::Expected<void> {
    if (expected_widgets == 0) {
        return {};
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto children = space.listChildren(SP::ConcretePathStringView{widgets_root});
        if (children.size() >= expected_widgets) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "scene widget structure did not publish"});
}

inline auto wait_for_declarative_widget_buckets(SP::PathSpace& space,
                                                SP::UI::Builders::ScenePath const& scene,
                                                std::size_t expected_widgets,
                                                std::chrono::milliseconds timeout) -> SP::Expected<void> {
    if (expected_widgets == 0) {
        return {};
    }
    auto metrics_base = std::string(scene.getPath()) + "/runtime/lifecycle/metrics";
    auto widgets_path = metrics_base + "/widgets_with_buckets";
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto buckets = space.read<std::uint64_t, std::string>(widgets_path);
        if (buckets && *buckets >= expected_widgets) {
            return {};
        }
        if (buckets && *buckets == 0 && expected_widgets == 0) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "widgets never published render buckets"});
}

inline auto wait_for_declarative_scene_revision(SP::PathSpace& space,
                                                SP::UI::Builders::ScenePath const& scene,
                                                std::chrono::milliseconds timeout,
                                                std::optional<std::uint64_t> min_revision = std::nullopt)
    -> SP::Expected<std::uint64_t> {
    auto revision_path = std::string(scene.getPath()) + "/current_revision";
    auto format_revision = [](std::uint64_t revision) {
        std::ostringstream oss;
        oss << std::setw(16) << std::setfill('0') << revision;
        return oss.str();
    };
    std::optional<std::uint64_t> ready_revision;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto revision = space.read<std::uint64_t, std::string>(revision_path);
        if (revision) {
            if (*revision != 0
                && (!min_revision.has_value() || *revision > *min_revision)) {
                ready_revision = *revision;
                break;
            }
        } else {
            auto const& error = revision.error();
            if (error.code != SP::Error::Code::NoSuchPath
                && error.code != SP::Error::Code::NoObjectFound) {
                return std::unexpected(error);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ready_revision) {
        return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                         "scene revision did not publish"});
    }
    auto revision_str = format_revision(*ready_revision);
    auto bucket_path = std::string(scene.getPath()) + "/builds/" + revision_str + "/bucket/drawables.bin";
    auto bucket_deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < bucket_deadline) {
        auto drawables = space.read<std::vector<std::uint8_t>, std::string>(bucket_path);
        if (drawables) {
            return *ready_revision;
        }
        auto const& error = drawables.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "scene bucket did not publish"});
}

inline auto read_scene_lifecycle_diagnostics(SP::PathSpace& space,
                                             SP::UI::Builders::ScenePath const& scene) -> std::string {
    auto metrics_base = std::string(scene.getPath()) + "/runtime/lifecycle/metrics";
    auto read_string = [&](std::string const& leaf) -> std::optional<std::string> {
        auto value = space.read<std::string, std::string>(metrics_base + "/" + leaf);
        if (!value) {
            auto const& error = value.error();
            if (error.code == SP::Error::Code::NoSuchPath
                || error.code == SP::Error::Code::NoObjectFound) {
                return std::nullopt;
            }
            return std::string{"<error reading " + leaf + ">"};
        }
        return *value;
    };
    auto read_uint = [&](std::string const& leaf) -> std::optional<std::uint64_t> {
        auto value = space.read<std::uint64_t, std::string>(metrics_base + "/" + leaf);
        if (!value) {
            auto const& error = value.error();
            if (error.code == SP::Error::Code::NoSuchPath
                || error.code == SP::Error::Code::NoObjectFound) {
                return std::nullopt;
            }
            return std::uint64_t{0};
        }
        return *value;
    };
    std::ostringstream oss;
    bool has_data = false;
    if (auto widgets = read_uint("widgets_with_buckets")) {
        oss << "widgets_with_buckets=" << *widgets;
        has_data = true;
    }
    if (auto descriptor = read_string("last_descriptor_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_descriptor_error=" << *descriptor;
        has_data = true;
    }
    if (auto bucket = read_string("last_bucket_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_bucket_error=" << *bucket;
        has_data = true;
    }
    if (auto last_error = read_string("last_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_error=" << *last_error;
        has_data = true;
    }
    if (!has_data) {
        return {};
    }
    return oss.str();
}

inline auto force_scene_publish_with_retry(SP::PathSpace& space,
                                           SP::UI::Builders::ScenePath const& scene,
                                           std::chrono::milliseconds widget_timeout,
                                           std::chrono::milliseconds publish_timeout,
                                           std::optional<std::uint64_t> min_revision,
                                           DeclarativeReadinessOptions const& readiness_options)
    -> SP::Expected<std::uint64_t> {
    auto deadline = std::chrono::steady_clock::now() + widget_timeout;
    SP::Error last_error{SP::Error::Code::Timeout, "scene force publish timed out"};
    SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
    publish_options.wait_timeout = publish_timeout;
    publish_options.min_revision = min_revision;
    auto make_pump_options = [&]() {
        auto pump_options = readiness_options.scene_pump_options;
        if (pump_options.wait_timeout.count() <= 0) {
            pump_options.wait_timeout = widget_timeout;
        }
        return pump_options;
    };
    auto pump_if_needed = [&]() -> SP::Expected<void> {
        if (!readiness_options.pump_scene_before_force_publish) {
            return {};
        }
        auto pump_options = make_pump_options();
        auto pump_result = SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene, pump_options);
        if (!pump_result) {
            auto const& error = pump_result.error();
            if (error.code == SP::Error::Code::Timeout) {
                return std::unexpected(error);
            }
            return std::unexpected(error);
        }
        return {};
    };
    if (readiness_options.pump_scene_before_force_publish) {
        auto pump_status = pump_if_needed();
        if (!pump_status) {
            last_error = pump_status.error();
        }
    }
    while (std::chrono::steady_clock::now() < deadline) {
        auto forced = SP::UI::Declarative::SceneLifecycle::ForcePublish(space, scene, publish_options);
        if (forced) {
            return forced;
        }
        last_error = forced.error();
        if (last_error.code == SP::Error::Code::NoObjectFound
            && readiness_options.pump_scene_before_force_publish) {
            auto pump_status = pump_if_needed();
            if (!pump_status) {
                last_error = pump_status.error();
                if (last_error.code != SP::Error::Code::Timeout
                    && last_error.code != SP::Error::Code::NoObjectFound) {
                    return std::unexpected(last_error);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
            continue;
        }
        if (last_error.code != SP::Error::Code::NoObjectFound
            && last_error.code != SP::Error::Code::Timeout) {
            auto diag = read_scene_lifecycle_diagnostics(space, scene);
            if (!diag.empty()) {
                if (last_error.message) {
                    last_error.message = *last_error.message + "; " + diag;
                } else {
                    last_error.message = diag;
                }
            }
            return std::unexpected(last_error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    auto diag = read_scene_lifecycle_diagnostics(space, scene);
    if (!diag.empty()) {
        if (last_error.message) {
            last_error.message = *last_error.message + "; " + diag;
        } else {
            last_error.message = diag;
        }
    }
    return std::unexpected(last_error);
}

inline auto readiness_skip_requested() -> bool {
    return std::getenv("PATHSPACE_SKIP_UI_READY_WAIT") != nullptr;
}

inline auto ensure_declarative_scene_ready(SP::PathSpace& space,
                                           SP::UI::Builders::ScenePath const& scene,
                                           SP::UI::Builders::WindowPath const& window,
                                           std::string const& view_name,
                                           DeclarativeReadinessOptions const& options = {})
    -> SP::Expected<DeclarativeReadinessResult> {
    DeclarativeReadinessResult result{};
    result.widget_count = count_window_widgets(space, window, view_name);
    auto scene_window_component = options.scene_window_component_override
        ? *options.scene_window_component_override
        : window_component_name(std::string(window.getPath()));
    auto scene_view_name = options.scene_view_override
        ? *options.scene_view_override
        : view_name;
    if (options.wait_for_runtime_metrics) {
        auto metrics_ready = wait_for_runtime_metrics_ready(space, options.runtime_metrics_timeout);
        if (!metrics_ready) {
            return std::unexpected(metrics_ready.error());
        }
    }
    if (readiness_skip_requested()) {
        return result;
    }
    if (result.widget_count == 0) {
        return result;
    }
    std::optional<std::uint64_t> publish_revision;
    if (options.force_scene_publish) {
        auto forced = force_scene_publish_with_retry(space,
                                                     scene,
                                                     options.widget_timeout,
                                                     options.revision_timeout,
                                                     options.min_revision,
                                                     options);
        if (!forced) {
            return std::unexpected(forced.error());
        }
        publish_revision = *forced;
    }
    if (options.wait_for_buckets && !options.force_scene_publish) {
        auto status = wait_for_declarative_widget_buckets(space,
                                                          scene,
                                                          result.widget_count,
                                                          options.widget_timeout);
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    if (options.wait_for_revision) {
        if (publish_revision) {
            result.scene_revision = *publish_revision;
        } else {
            auto revision = wait_for_declarative_scene_revision(space,
                                                                scene,
                                                                options.revision_timeout,
                                                                options.min_revision);
            if (!revision) {
                return std::unexpected(revision.error());
            }
            result.scene_revision = *revision;
        }
    }
    if (options.wait_for_structure && !options.force_scene_publish) {
        auto scene_widgets_root = make_scene_widgets_root_components(scene,
                                                                     scene_window_component,
                                                                     scene_view_name);
        auto status = wait_for_declarative_scene_widgets(space,
                                                        scene_widgets_root,
                                                        result.widget_count,
                                                        options.widget_timeout);
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    return result;
}

} // namespace PathSpaceExamples
