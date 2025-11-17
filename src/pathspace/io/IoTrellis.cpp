#include <pathspace/io/IoTrellis.hpp>

#include <pathspace/io/IoEvents.hpp>
#include <pathspace/layer/io/PathIOGamepad.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/log/TaggedLogger.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SP::IO {

namespace {

using namespace std::chrono_literals;

constexpr auto kMinSleep = std::chrono::milliseconds{1};
constexpr auto kMinWait = std::chrono::milliseconds{1};

auto clamp_duration(std::chrono::milliseconds value,
                    std::chrono::milliseconds fallback,
                    std::chrono::milliseconds min_value) -> std::chrono::milliseconds {
    if (value <= std::chrono::milliseconds::zero()) {
        return fallback;
    }
    if (value < min_value) {
        return min_value;
    }
    return value;
}

auto ns_from_uint64(std::uint64_t raw) -> std::chrono::nanoseconds {
    return std::chrono::nanoseconds{static_cast<std::int64_t>(raw)};
}

auto to_pointer_modifiers(std::uint32_t modifiers) -> ButtonModifiers {
    ButtonModifiers result = ButtonModifiers::None;
    if (modifiers & Mod_Shift) {
        result = result | ButtonModifiers::Shift;
    }
    if (modifiers & Mod_Ctrl) {
        result = result | ButtonModifiers::Control;
    }
    if (modifiers & Mod_Alt) {
        result = result | ButtonModifiers::Alt;
    }
    if (modifiers & Mod_Meta) {
        result = result | ButtonModifiers::Command;
    }
    return result;
}

auto decode_utf8(std::string_view utf8) -> std::vector<char32_t> {
    std::vector<char32_t> codepoints;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            codepoints.push_back(static_cast<char32_t>(c));
            ++i;
            continue;
        }

        auto remaining = utf8.size() - i;
        if ((c & 0xE0) == 0xC0 && remaining >= 2) {
            unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            if ((c1 & 0xC0) != 0x80) {
                ++i;
                continue;
            }
            char32_t cp = static_cast<char32_t>(((c & 0x1Fu) << 6u) | (c1 & 0x3Fu));
            codepoints.push_back(cp);
            i += 2;
            continue;
        }

        if ((c & 0xF0) == 0xE0 && remaining >= 3) {
            unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
            if (((c1 & 0xC0) != 0x80) || ((c2 & 0xC0) != 0x80)) {
                ++i;
                continue;
            }
            char32_t cp = static_cast<char32_t>(((c & 0x0Fu) << 12u) |
                                                ((c1 & 0x3Fu) << 6u) |
                                                (c2 & 0x3Fu));
            codepoints.push_back(cp);
            i += 3;
            continue;
        }

        if ((c & 0xF8) == 0xF0 && remaining >= 4) {
            unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(utf8[i + 3]);
            if (((c1 & 0xC0) != 0x80) ||
                ((c2 & 0xC0) != 0x80) ||
                ((c3 & 0xC0) != 0x80)) {
                ++i;
                continue;
            }
            char32_t cp = static_cast<char32_t>(((c & 0x07u) << 18u) |
                                                ((c1 & 0x3Fu) << 12u) |
                                                ((c2 & 0x3Fu) << 6u) |
                                                (c3 & 0x3Fu));
            codepoints.push_back(cp);
            i += 4;
            continue;
        }

        ++i;
    }
    return codepoints;
}

std::mutex g_registry_mutex;
std::unordered_map<PathSpace*, std::weak_ptr<IoTrellisImpl>> g_registry;

auto make_error(std::string message) -> Error {
    return Error{Error::Code::InvalidPath, std::move(message)};
}

} // namespace

struct IoTrellisImpl : std::enable_shared_from_this<IoTrellisImpl> {
    enum class DeviceKind {
        Pointer,
        Keyboard,
        Gamepad
    };

    struct DeviceEntry {
        std::string device_root;
        std::string events_path;
        std::string enabled_path;
        std::string subscriber_path;
        bool        toggled_enabled = false;
        bool        toggled_subscriber = false;
        DeviceKind  kind = DeviceKind::Pointer;
    };

    struct GamepadState {
        float sticks[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
    };

    IoTrellisImpl(PathSpace& space, IoTrellisOptions options)
        : space_(space)
        , options_(std::move(options)) {
        options_.event_wait_timeout = clamp_duration(options_.event_wait_timeout, 2ms, kMinWait);
        options_.idle_sleep = clamp_duration(options_.idle_sleep, 2ms, kMinSleep);
        options_.discovery_interval = clamp_duration(options_.discovery_interval, 1000ms, 50ms);
        options_.telemetry_publish_interval = clamp_duration(options_.telemetry_publish_interval, 200ms, 50ms);
        options_.telemetry_poll_interval = clamp_duration(options_.telemetry_poll_interval, 250ms, 50ms);
    }

    ~IoTrellisImpl() {
        shutdown();
    }

    auto start() -> SP::Expected<void> {
        try {
            worker_ = std::thread([self = shared_from_this()] { self->run(); });
        } catch (...) {
            return std::unexpected(make_error("Failed to start IO Trellis worker thread"));
        }
        return {};
    }

    void shutdown() {
        bool expected = false;
        if (!stop_flag_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        teardown_devices(pointer_devices_);
        teardown_devices(keyboard_devices_);
        teardown_devices(gamepad_devices_);
        unregister();
    }

private:
    using DeviceRegistry = std::unordered_map<std::string, DeviceEntry>;

    void unregister() {
        std::lock_guard<std::mutex> guard(g_registry_mutex);
        auto it = g_registry.find(&space_);
        if (it == g_registry.end()) {
            return;
        }
        if (auto locked = it->second.lock(); !locked || locked.get() == this) {
            g_registry.erase(it);
        }
    }

    void run() {
#if defined(SP_LOG_DEBUG)
        set_thread_name("IoTrellis");
#endif
        auto next_discovery = std::chrono::steady_clock::now();
        auto next_telemetry_poll = std::chrono::steady_clock::now();
        auto next_metrics_publish = std::chrono::steady_clock::now() + options_.telemetry_publish_interval;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();

            if (now >= next_discovery) {
                refresh_devices();
                next_discovery = now + options_.discovery_interval;
            }

            if (!options_.telemetry_toggle_path.empty() && now >= next_telemetry_poll) {
                poll_telemetry();
                next_telemetry_poll = now + options_.telemetry_poll_interval;
            }

            bool processed = false;
            if (options_.enable_pointer) {
                processed |= drain_pointer_devices();
            }
            if (options_.enable_keyboard) {
                processed |= drain_keyboard_devices();
            }
            if (options_.enable_gamepad) {
                processed |= drain_gamepad_devices();
            }

            if (telemetry_enabled_.load(std::memory_order_relaxed) && now >= next_metrics_publish) {
                publish_metrics();
                next_metrics_publish = now + options_.telemetry_publish_interval;
            }

            if (!processed) {
                std::this_thread::sleep_for(options_.idle_sleep);
            }
        }
    }

    void refresh_devices() {
        refresh_devices_for_root("/system/devices/in/pointer", DeviceKind::Pointer);
        refresh_devices_for_root("/system/devices/in/text", DeviceKind::Keyboard);
        refresh_devices_for_root("/system/devices/in/keyboard", DeviceKind::Keyboard);
        refresh_devices_for_root("/system/devices/in/gamepad", DeviceKind::Gamepad);
    }

    void refresh_devices_for_root(std::string_view root, DeviceKind kind) {
        std::string root_str{root};
        auto devices = space_.listChildren(SP::ConcretePathStringView{root_str});
        if (devices.empty()) {
            teardown_missing_devices(kind, {});
            return;
        }

        std::unordered_set<std::string> seen;
        seen.reserve(devices.size());
        auto& registry = registry_for(kind);

        for (auto const& device_name : devices) {
            std::string device_root = root_str;
            if (!device_root.empty() && device_root.back() != '/') {
                device_root.push_back('/');
            }
            device_root.append(device_name);
            seen.insert(device_root);
            if (registry.contains(device_root)) {
                continue;
            }
            DeviceEntry entry{
                .device_root = device_root,
                .events_path = device_root + "/events",
                .enabled_path = device_root + "/config/push/enabled",
                .subscriber_path = device_root + "/config/push/subscribers/" + options_.subscriber_name,
                .toggled_enabled = false,
                .toggled_subscriber = false,
                .kind = kind,
            };
            if (options_.subscriber_name.empty()) {
                entry.subscriber_path.clear();
            }
            configure_device(entry);
            registry.emplace(device_root, std::move(entry));
        }

        teardown_missing_devices(kind, seen);
    }

    void teardown_missing_devices(DeviceKind kind, std::unordered_set<std::string> const& keep) {
        auto& registry = registry_for(kind);
        for (auto it = registry.begin(); it != registry.end();) {
            if (!keep.empty() && keep.contains(it->first)) {
                ++it;
                continue;
            }
            teardown_device(it->second);
            it = registry.erase(it);
        }
    }

    DeviceRegistry& registry_for(DeviceKind kind) {
        switch (kind) {
        case DeviceKind::Pointer:
            return pointer_devices_;
        case DeviceKind::Keyboard:
            return keyboard_devices_;
        case DeviceKind::Gamepad:
        default:
            return gamepad_devices_;
        }
    }

    void configure_device(DeviceEntry& entry) {
        if (set_bool(entry.enabled_path, true)) {
            entry.toggled_enabled = true;
        }
        if (!entry.subscriber_path.empty() && set_bool(entry.subscriber_path, true)) {
            entry.toggled_subscriber = true;
        }
    }

    void teardown_device(DeviceEntry const& entry) {
        if (entry.toggled_subscriber && !entry.subscriber_path.empty()) {
            (void)space_.insert(entry.subscriber_path, false);
        }
        if (entry.toggled_enabled) {
            (void)space_.insert(entry.enabled_path, false);
        }
        if (entry.kind == DeviceKind::Gamepad) {
            std::string prefix = entry.device_root;
            if (!prefix.empty()) {
                prefix.push_back('#');
            }
            for (auto it = gamepad_state_.begin(); it != gamepad_state_.end();) {
                if (it->first.rfind(prefix, 0) == 0) {
                    it = gamepad_state_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    template <typename Registry>
    void teardown_devices(Registry& registry) {
        for (auto& [_, entry] : registry) {
            teardown_device(entry);
        }
        registry.clear();
    }

    auto drain_pointer_devices() -> bool {
        bool processed = false;
        auto wait = options_.event_wait_timeout;
        for (auto& [_, device] : pointer_devices_) {
            processed |= drain_pointer_device(device, wait);
        }
        return processed;
    }

    auto drain_pointer_device(DeviceEntry const& device,
                              std::chrono::milliseconds wait) -> bool {
        bool processed = false;
        auto options = Out{} & Block{wait};
        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto taken = space_.take<PathIOMouse::Event, std::string>(device.events_path, options);
            if (taken) {
                handle_mouse_event(device.device_root, *taken);
                processed = true;
                continue;
            }
            auto const& error = taken.error();
            if (error.code == Error::Code::NoObjectFound ||
                error.code == Error::Code::Timeout ||
                error.code == Error::Code::NoSuchPath) {
                break;
            }
            break;
        }
        return processed;
    }

    void handle_mouse_event(std::string const& device_path,
                            PathIOMouse::Event const& raw) {
        PointerEvent pointer{};
        pointer.device_path = device_path;
        pointer.pointer_id = static_cast<std::uint64_t>(raw.deviceId);
        pointer.meta.timestamp = ns_from_uint64(raw.timestampNs);
        pointer.type = PointerType::Mouse;

        switch (raw.type) {
        case SP::MouseEventType::Move:
            pointer.motion.delta_x = static_cast<float>(raw.dx);
            pointer.motion.delta_y = static_cast<float>(raw.dy);
            pointer.motion.absolute = false;
            emit_pointer_event(pointer);
            break;
        case SP::MouseEventType::AbsoluteMove:
            pointer.motion.absolute = true;
            pointer.motion.absolute_x = static_cast<float>(raw.x);
            pointer.motion.absolute_y = static_cast<float>(raw.y);
            emit_pointer_event(pointer);
            break;
        case SP::MouseEventType::Wheel:
            pointer.motion.delta_y = static_cast<float>(raw.wheel);
            pointer.motion.absolute = false;
            emit_pointer_event(pointer);
            break;
        case SP::MouseEventType::ButtonDown:
        case SP::MouseEventType::ButtonUp:
            emit_mouse_button(device_path, raw, raw.type == SP::MouseEventType::ButtonDown);
            break;
        }
    }

    void emit_mouse_button(std::string const& device_path,
                           PathIOMouse::Event const& raw,
                           bool pressed) {
        ButtonEvent button{};
        button.device_path = device_path;
        button.source = ButtonSource::Mouse;
        button.button_code = static_cast<std::uint32_t>(raw.button);
        button.button_id = static_cast<int>(raw.button);
        button.state.pressed = pressed;
        button.state.repeat = false;
        button.meta.timestamp = ns_from_uint64(raw.timestampNs);
        emit_button_event(button);
    }

    auto drain_keyboard_devices() -> bool {
        bool processed = false;
        auto wait = options_.event_wait_timeout;
        for (auto& [_, device] : keyboard_devices_) {
            processed |= drain_keyboard_device(device, wait);
        }
        return processed;
    }

    auto drain_keyboard_device(DeviceEntry const& device,
                               std::chrono::milliseconds wait) -> bool {
        bool processed = false;
        auto options = Out{} & Block{wait};
        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto taken = space_.take<PathIOKeyboard::Event, std::string>(device.events_path, options);
            if (taken) {
                handle_keyboard_event(device.device_root, *taken);
                processed = true;
                continue;
            }
            auto const& error = taken.error();
            if (error.code == Error::Code::NoObjectFound ||
                error.code == Error::Code::Timeout ||
                error.code == Error::Code::NoSuchPath) {
                break;
            }
            break;
        }
        return processed;
    }

    void handle_keyboard_event(std::string const& device_path,
                               PathIOKeyboard::Event const& raw) {
        auto modifiers = to_pointer_modifiers(raw.modifiers);
        switch (raw.type) {
        case SP::KeyEventType::KeyDown:
        case SP::KeyEventType::KeyUp: {
            ButtonEvent button{};
            button.device_path = device_path;
            button.source = ButtonSource::Keyboard;
            button.button_code = static_cast<std::uint32_t>(raw.keycode);
            button.button_id = raw.keycode;
            button.state.pressed = raw.type == SP::KeyEventType::KeyDown;
            button.meta.modifiers = modifiers;
            button.meta.timestamp = ns_from_uint64(raw.timestampNs);
            emit_button_event(button);
            break;
        }
        case SP::KeyEventType::Text: {
            auto characters = decode_utf8(raw.text);
            for (auto cp : characters) {
                TextEvent text{};
                text.device_path = device_path;
                text.codepoint = cp;
                text.modifiers = modifiers;
                text.timestamp = ns_from_uint64(raw.timestampNs);
                emit_text_event(text);
            }
            break;
        }
        }
    }

    auto drain_gamepad_devices() -> bool {
        bool processed = false;
        auto wait = options_.event_wait_timeout;
        for (auto& [_, device] : gamepad_devices_) {
            processed |= drain_gamepad_device(device, wait);
        }
        return processed;
    }

    auto drain_gamepad_device(DeviceEntry const& device,
                              std::chrono::milliseconds wait) -> bool {
        bool processed = false;
        auto options = Out{} & Block{wait};
        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto taken = space_.take<PathIOGamepad::Event, std::string>(device.events_path, options);
            if (taken) {
                handle_gamepad_event(device.device_root, *taken);
                processed = true;
                continue;
            }
            auto const& error = taken.error();
            if (error.code == Error::Code::NoObjectFound ||
                error.code == Error::Code::Timeout ||
                error.code == Error::Code::NoSuchPath) {
                break;
            }
            break;
        }
        return processed;
    }

    void handle_gamepad_event(std::string const& device_path,
                              PathIOGamepad::Event const& raw) {
        switch (raw.type) {
        case PathIOGamepad::EventType::ButtonDown:
        case PathIOGamepad::EventType::ButtonUp: {
            ButtonEvent button{};
            button.device_path = device_path;
            button.source = ButtonSource::Gamepad;
            button.button_code = static_cast<std::uint32_t>(raw.button);
            button.button_id = raw.button;
            button.state.pressed = raw.type == PathIOGamepad::EventType::ButtonDown;
            button.meta.timestamp = ns_from_uint64(raw.timestampNs);
            emit_button_event(button);
            break;
        }
        case PathIOGamepad::EventType::AxisMove:
            emit_gamepad_axis(device_path, raw);
            break;
        case PathIOGamepad::EventType::Connected:
        case PathIOGamepad::EventType::Disconnected:
            break;
        }
    }

    void emit_gamepad_axis(std::string const& device_path,
                           PathIOGamepad::Event const& raw) {
        if (raw.axis < 0) {
            return;
        }
        int stick = raw.axis / 2;
        if (stick > 1) {
            stick = 1;
        }
        bool x_axis = (raw.axis % 2) == 0;
        auto key = device_path + "#" + std::to_string(raw.deviceId);
        auto& state = gamepad_state_[key];
        float previous = x_axis ? state.sticks[stick][0] : state.sticks[stick][1];
        float delta = raw.value - previous;
        if (x_axis) {
            state.sticks[stick][0] = raw.value;
        } else {
            state.sticks[stick][1] = raw.value;
        }

        PointerEvent pointer{};
        pointer.device_path = device_path;
        pointer.pointer_id = (static_cast<std::uint64_t>(raw.deviceId & 0xFFFF) << 16u) |
                             static_cast<std::uint64_t>(stick & 0xFFFF);
        pointer.type = PointerType::GamepadStick;
        pointer.motion.absolute = true;
        pointer.motion.absolute_x = state.sticks[stick][0];
        pointer.motion.absolute_y = state.sticks[stick][1];
        pointer.motion.delta_x = x_axis ? delta : 0.0f;
        pointer.motion.delta_y = x_axis ? 0.0f : delta;
        pointer.meta.timestamp = ns_from_uint64(raw.timestampNs);
        emit_pointer_event(pointer);
    }

    void emit_pointer_event(PointerEvent const& event) {
        pointer_events_.fetch_add(1, std::memory_order_relaxed);
        auto result = space_.insert(std::string{IoEventPaths::kPointerQueue}, event);
        (void)result;
    }

    void emit_button_event(ButtonEvent const& event) {
        button_events_.fetch_add(1, std::memory_order_relaxed);
        auto result = space_.insert(std::string{IoEventPaths::kButtonQueue}, event);
        (void)result;
    }

    void emit_text_event(TextEvent const& event) {
        text_events_.fetch_add(1, std::memory_order_relaxed);
        auto result = space_.insert(std::string{IoEventPaths::kTextQueue}, event);
        (void)result;
    }

    void poll_telemetry() {
        bool enabled = false;
        if (!options_.telemetry_toggle_path.empty()) {
            if (auto current = read_bool(options_.telemetry_toggle_path); current && *current) {
                enabled = true;
            }
        }
        telemetry_enabled_.store(enabled, std::memory_order_release);
    }

    void publish_metrics() {
        auto metrics_root = options_.metrics_root;
        if (metrics_root.empty()) {
            return;
        }
        (void)replace_single_value<std::uint64_t>(
            space_, metrics_root + "/pointer_events_total", pointer_events_.load(std::memory_order_relaxed));
        (void)replace_single_value<std::uint64_t>(
            space_, metrics_root + "/button_events_total", button_events_.load(std::memory_order_relaxed));
        (void)replace_single_value<std::uint64_t>(
            space_, metrics_root + "/text_events_total", text_events_.load(std::memory_order_relaxed));
    }

    template <typename T>
    static auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
        while (true) {
            auto taken = space.take<T, std::string>(path);
            if (taken) {
                continue;
            }
            auto const& error = taken.error();
            if (error.code == Error::Code::NoObjectFound ||
                error.code == Error::Code::NoSuchPath) {
                return {};
            }
            return std::unexpected(error);
        }
    }

    template <typename T>
    static auto replace_single_value(PathSpace& space,
                                     std::string const& path,
                                     T const& value) -> SP::Expected<void> {
        if (auto cleared = drain_queue<T>(space, path); !cleared) {
            return cleared;
        }
        auto inserted = space.insert(path, value);
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        return {};
    }

    auto read_bool(std::string const& path) -> std::optional<bool> {
        auto value = space_.read<bool, std::string>(path);
        if (value) {
            return *value;
        }
        auto const& error = value.error();
        if (error.code == Error::Code::NoSuchPath ||
            error.code == Error::Code::NoObjectFound) {
            return std::nullopt;
        }
        return std::nullopt;
    }

    auto set_bool(std::string const& path, bool desired) -> bool {
        if (path.empty()) {
            return false;
        }
        auto current = read_bool(path);
        if (current && *current == desired) {
            return false;
        }
        auto inserted = space_.insert(path, desired);
        if (!inserted.errors.empty()) {
            return false;
        }
        return true;
    }

private:
    PathSpace& space_;
    IoTrellisOptions options_;
    std::atomic<bool> stop_flag_{false};
    std::thread worker_;

    DeviceRegistry pointer_devices_;
    DeviceRegistry keyboard_devices_;
    DeviceRegistry gamepad_devices_;
    std::unordered_map<std::string, GamepadState> gamepad_state_;

    std::atomic<std::uint64_t> pointer_events_{0};
    std::atomic<std::uint64_t> button_events_{0};
    std::atomic<std::uint64_t> text_events_{0};
    std::atomic<bool> telemetry_enabled_{false};
};

IoTrellisHandle::IoTrellisHandle(std::shared_ptr<IoTrellisImpl> impl)
    : impl_(std::move(impl)) {}

IoTrellisHandle::~IoTrellisHandle() {
    shutdown();
}

IoTrellisHandle::IoTrellisHandle(IoTrellisHandle&& other) noexcept
    : impl_(std::move(other.impl_)) {}

auto IoTrellisHandle::operator=(IoTrellisHandle&& other) noexcept -> IoTrellisHandle& {
    if (this == &other) {
        return *this;
    }
    shutdown();
    impl_ = std::move(other.impl_);
    return *this;
}

void IoTrellisHandle::shutdown() {
    if (impl_) {
        impl_->shutdown();
        impl_.reset();
    }
}

auto CreateIOTrellis(PathSpace& space,
                     IoTrellisOptions const& options) -> SP::Expected<IoTrellisHandle> {
    std::lock_guard<std::mutex> guard(g_registry_mutex);
    if (auto it = g_registry.find(&space); it != g_registry.end()) {
        if (auto existing = it->second.lock()) {
            return IoTrellisHandle{existing};
        }
        g_registry.erase(it);
    }

    auto impl = std::make_shared<IoTrellisImpl>(space, options);
    if (auto started = impl->start(); !started) {
        return std::unexpected(started.error());
    }

    g_registry[&space] = impl;
    return IoTrellisHandle{std::move(impl)};
}

} // namespace SP::IO
