#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SP::IO {

enum class PointerType : std::uint8_t {
    Mouse = 0,
    Stylus,
    Touch,
    GamepadStick,
    VRController,
    Pose
};

enum class ButtonSource : std::uint8_t {
    Mouse = 0,
    Keyboard,
    Gamepad,
    VRController,
    PhoneButton,
    Custom
};

enum class ButtonModifiers : std::uint32_t {
    None     = 0,
    Shift    = 1u << 0,
    Control  = 1u << 1,
    Alt      = 1u << 2,
    Command  = 1u << 3,
    Function = 1u << 4
};

[[nodiscard]] constexpr auto operator|(ButtonModifiers lhs, ButtonModifiers rhs) -> ButtonModifiers {
    return static_cast<ButtonModifiers>(static_cast<std::uint32_t>(lhs) |
                                        static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr auto operator&(ButtonModifiers lhs, ButtonModifiers rhs) -> ButtonModifiers {
    return static_cast<ButtonModifiers>(static_cast<std::uint32_t>(lhs) &
                                        static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr auto hasModifier(ButtonModifiers value, ButtonModifiers flag) -> bool {
    return (value & flag) != ButtonModifiers::None;
}

struct Pose {
    float position[3]{0.0f, 0.0f, 0.0f};    // meters in app/world space
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion (x, y, z, w)
};

struct StylusInfo {
    float pressure = 0.0f; // 0..1
    float tilt_x   = 0.0f; // radians
    float tilt_y   = 0.0f; // radians
    float twist    = 0.0f; // radians around stylus axis
    bool  eraser   = false;
};

struct PointerEvent {
    std::string device_path;
    std::uint64_t pointer_id = 0;
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    float absolute_x = 0.0f;
    float absolute_y = 0.0f;
    bool absolute = false;
    PointerType type = PointerType::Mouse;
    std::optional<Pose> pose{};
    std::optional<StylusInfo> stylus{};
    ButtonModifiers modifiers = ButtonModifiers::None;
    std::chrono::nanoseconds timestamp{};
};

struct ButtonEvent {
    ButtonSource source = ButtonSource::Mouse;
    std::string device_path;
    std::uint32_t button_code = 0;
    int button_id = 0;
    bool pressed = false;
    bool repeat = false;
    float analog_value = 0.0f;
    ButtonModifiers modifiers = ButtonModifiers::None;
    std::chrono::nanoseconds timestamp{};
};

struct TextEvent {
    std::string device_path;
    char32_t codepoint = 0;
    ButtonModifiers modifiers = ButtonModifiers::None;
    bool repeat = false;
    std::chrono::nanoseconds timestamp{};
};

struct IoEventPaths {
    static constexpr std::string_view kRoot = "/system/io/events";
    static constexpr std::string_view kPointerQueue = "/system/io/events/pointer";
    static constexpr std::string_view kButtonQueue = "/system/io/events/button";
    static constexpr std::string_view kTextQueue = "/system/io/events/text";
    static constexpr std::string_view kPoseQueue = "/system/io/events/pose";
    static constexpr std::string_view kDeviceConfigRoot = "/system/devices/in";
    static constexpr std::string_view kPushConfigSuffix = "config/push";
};

struct DevicePushConfigSnapshot {
    bool push_enabled = false;
    std::uint32_t rate_limit_hz = 240;
    std::uint32_t max_queue = 256;
    bool telemetry_enabled = false;
};

} // namespace SP::IO

