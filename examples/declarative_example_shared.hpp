#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/runtime/IOPump.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>

#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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

} // namespace PathSpaceExamples
