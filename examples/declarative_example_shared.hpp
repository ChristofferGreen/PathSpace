#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/SceneReadiness.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
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

inline void subscribe_window_devices(SP::PathSpace& space,
                                     SP::UI::WindowPath const& window,
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
    std::function<void(SP::UI::Declarative::PresentFrame const&)> on_present;
};

inline void run_present_loop(SP::PathSpace& space,
                             SP::UI::WindowPath const& window,
                             std::string const& view_name,
                             SP::UI::Declarative::PresentHandles const& present_handles,
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
            (void)SP::UI::Declarative::ResizePresentSurface(space,
                                                            present_handles,
                                                            window_width,
                                                            window_height);
        }
        if (hooks.before_present) {
            hooks.before_present();
        }
        auto present_frame = SP::UI::Declarative::PresentWindowFrame(space, present_handles);
        if (present_frame) {
            if (hooks.on_present) {
                hooks.on_present(*present_frame);
            }
            auto present_status = SP::UI::Declarative::PresentFrameToLocalWindow(*present_frame,
                                                                                 window_width,
                                                                                 window_height);
            (void)present_status;
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

using DeclarativeReadinessOptions = SP::UI::Declarative::DeclarativeReadinessOptions;
using DeclarativeReadinessResult = SP::UI::Declarative::DeclarativeReadinessResult;

inline auto make_window_view_path(SP::UI::WindowPath const& window,
                                 std::string const& view_name) -> std::string {
    return SP::UI::Declarative::MakeWindowViewPath(window, view_name);
}

inline auto window_component_name(std::string const& window_path) -> std::string {
    return SP::UI::Declarative::WindowComponentName(window_path);
}

inline auto app_root_from_window(SP::UI::WindowPath const& window) -> std::string {
    return SP::UI::Declarative::AppRootFromWindow(window);
}

inline auto make_scene_widgets_root_components(SP::UI::ScenePath const& scene,
                                               std::string_view window_component,
                                               std::string_view view_name) -> std::string {
    return SP::UI::Declarative::MakeSceneWidgetsRootComponents(scene, window_component, view_name);
}

inline auto make_scene_widgets_root(SP::UI::ScenePath const& scene,
                                    SP::UI::WindowPath const& window,
                                    std::string const& view_name) -> std::string {
    return SP::UI::Declarative::MakeSceneWidgetsRoot(scene, window, view_name);
}

inline auto force_window_software_renderer(SP::PathSpace& space,
                                           SP::UI::WindowPath const& window,
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
    auto settings = SP::UI::Runtime::Renderer::ReadSettings(space, renderer_view);
    if (!settings) {
        return std::unexpected(settings.error());
    }
    if (!settings->renderer.metal_uploads_enabled) {
        return {};
    }
    settings->renderer.metal_uploads_enabled = false;
    return SP::UI::Runtime::Renderer::UpdateSettings(space, renderer_view, *settings);
}

inline auto count_window_widgets(SP::PathSpace& space,
                                 SP::UI::WindowPath const& window,
                                 std::string const& view_name) -> std::size_t {
    return SP::UI::Declarative::CountWindowWidgets(space, window, view_name);
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
    return SP::UI::Declarative::WaitForRuntimeMetricsReady(space, timeout);
}

inline auto wait_for_declarative_scene_widgets(SP::PathSpace& space,
                                               std::string const& widgets_root,
                                               std::size_t expected_widgets,
                                               std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return SP::UI::Declarative::WaitForDeclarativeSceneWidgets(space,
                                                              widgets_root,
                                                              expected_widgets,
                                                              timeout);
}

inline auto wait_for_declarative_widget_buckets(SP::PathSpace& space,
                                                SP::UI::ScenePath const& scene,
                                                std::size_t expected_widgets,
                                                std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return SP::UI::Declarative::WaitForDeclarativeWidgetBuckets(space,
                                                               scene,
                                                               expected_widgets,
                                                               timeout);
}

inline auto wait_for_declarative_scene_revision(SP::PathSpace& space,
                                                SP::UI::ScenePath const& scene,
                                                std::chrono::milliseconds timeout,
                                                std::optional<std::uint64_t> min_revision = std::nullopt)
    -> SP::Expected<std::uint64_t> {
    return SP::UI::Declarative::WaitForDeclarativeSceneRevision(space,
                                                                scene,
                                                                timeout,
                                                                min_revision);
}

inline auto read_scene_lifecycle_diagnostics(SP::PathSpace& space,
                                             SP::UI::ScenePath const& scene) -> std::string {
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

inline auto ensure_declarative_scene_ready(SP::PathSpace& space,
                                           SP::UI::ScenePath const& scene,
                                           SP::UI::WindowPath const& window,
                                           std::string const& view_name,
                                           DeclarativeReadinessOptions const& options = {})
    -> SP::Expected<DeclarativeReadinessResult> {
    return SP::UI::Declarative::EnsureDeclarativeSceneReady(space,
                                                            scene,
                                                            window,
                                                            view_name,
                                                            options);
}

} // namespace PathSpaceExamples
