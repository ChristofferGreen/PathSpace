#pragma once
#include "layer/PathIO.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace SP {
#if defined(PATHIO_BACKEND_MACOS)
// Weakly-linked helper implemented in an ObjC++ TU to call GameController rumble.
// If absent at link time, remain functional and report InvalidPermissions.
extern "C" bool PSGameControllerApplyRumble(int deviceId, float low, float high, uint32_t durationMs) __attribute__((weak_import));
#endif

/**
 * PathIOGamepad — concrete IO provider for gamepad devices.
 *
 * Characteristics:
 * - Path-agnostic: can be mounted anywhere in a parent PathSpace.
 * - Typed out()/take(): serves GamepadEvent with peek (non-pop) and pop semantics and supports blocking waits.
 * - in(): accepts HapticsCommand (rumble) at control paths; executes on simulation backend and, when integrated,
 *   on OS backends. Until platform integration, macOS OS mode reports InvalidPermissions for haptics.
 * - Concurrency: thread-safe enqueue/peek/pop with condition_variable-based blocking reads.
 * - Notifications: when mounted with a shared context, simulateEvent will wake waiters via targeted notify.
 *
 * Paths (recommended when mounted under /system/devices):
 * - Inputs (read/take events):
 *   - /system/devices/in/gamepad/<id>/events
 * - Outputs (write commands):
 *   - /system/devices/out/gamepad/<id>/rumble  (HapticsCommand)
 *
 * Notes:
 * - This provider ignores the iterator's relative path for reads (serves the event stream regardless),
 *   but it targets notifications to "<mount>" and "<mount>/events".
 * - For writes, it accepts HapticsCommand regardless of tail path, with best-effort routing by suffix
 *   ("/rumble" or "/haptics"); other control leaves may be added later.
 */
class PathIOGamepad final : public PathIO {
public:
    enum class EventType {
        Connected,
        Disconnected,
        ButtonDown,
        ButtonUp,
        AxisMove
    };

    struct Event {
        int        deviceId    = 0;
        EventType  type        = EventType::Connected;
        // Button index (for ButtonDown/Up)
        int        button      = -1;
        // Axis index/value (for AxisMove)
        int        axis        = -1;
        float      value       = 0.0f; // normalized [-1, 1] for axes
        // Monotonic timestamp in nanoseconds for ordering/merging
        uint64_t   timestampNs = 0;

        // Stream operator for logging/printing
        friend std::ostream& operator<<(std::ostream& os, Event const& e) {
            switch (e.type) {
                case EventType::Connected:
                    return os << "[gamepad] connected id=" << e.deviceId;
                case EventType::Disconnected:
                    return os << "[gamepad] disconnected id=" << e.deviceId;
                case EventType::ButtonDown:
                    return os << "[gamepad] button down " << e.button;
                case EventType::ButtonUp:
                    return os << "[gamepad] button up " << e.button;
                case EventType::AxisMove:
                    return os << "[gamepad] axis " << e.axis << " value=" << e.value;
            }
            return os;
        }
    };

    // Haptics command (rumble) — normalized [0, 1] strengths and duration in milliseconds.
    struct HapticsCommand {
        float   lowFrequency  = 0.0f;   // e.g., "strong" motor
        float   highFrequency = 0.0f;   // e.g., "weak" motor
        uint32_t durationMs   = 0;      // 0 may be interpreted as "continuous" by some backends

        static HapticsCommand constant(float low, float high, uint32_t ms) {
            return HapticsCommand{low, high, ms};
        }
    };

    using GamepadEvent = Event;
    using Command = HapticsCommand;

    enum class BackendMode { Off, Auto, Simulation, OS };

public:
    explicit PathIOGamepad(BackendMode mode = BackendMode::Off, int deviceId = 0)
        : mode_(mode), deviceId_(deviceId) {
#if defined(PATHIO_BACKEND_MACOS)
        if (mode_ == BackendMode::Auto) {
            // OS integration is not yet wired; prefer Simulation until implemented.
            mode_ = BackendMode::Simulation;
        }
#else
        if (mode_ == BackendMode::Auto) {
            mode_ = BackendMode::Simulation;
        }
#endif
        // No background worker needed for v1; events arrive via simulate* or platform hooks when integrated.
    }

    ~PathIOGamepad() {
        // No worker_ currently; if we add one, ensure proper teardown here.
    }

    // ---- Simulation/back-end event API (thread-safe) ----

    void simulateConnected(int deviceId = -1) {
        Event ev;
        ev.deviceId    = (deviceId >= 0 ? deviceId : deviceId_);
        ev.type        = EventType::Connected;
        ev.timestampNs = nowNs_();
        enqueue_(ev);
    }

    void simulateDisconnected(int deviceId = -1) {
        Event ev;
        ev.deviceId    = (deviceId >= 0 ? deviceId : deviceId_);
        ev.type        = EventType::Disconnected;
        ev.timestampNs = nowNs_();
        enqueue_(ev);
    }

    void simulateButtonDown(int button, int deviceId = -1) {
        Event ev;
        ev.deviceId    = (deviceId >= 0 ? deviceId : deviceId_);
        ev.type        = EventType::ButtonDown;
        ev.button      = button;
        ev.timestampNs = nowNs_();
        enqueue_(ev);
    }

    void simulateButtonUp(int button, int deviceId = -1) {
        Event ev;
        ev.deviceId    = (deviceId >= 0 ? deviceId : deviceId_);
        ev.type        = EventType::ButtonUp;
        ev.button      = button;
        ev.timestampNs = nowNs_();
        enqueue_(ev);
    }

    void simulateAxisMove(int axis, float value, int deviceId = -1) {
        Event ev;
        ev.deviceId    = (deviceId >= 0 ? deviceId : deviceId_);
        ev.type        = EventType::AxisMove;
        ev.axis        = axis;
        ev.value       = value;
        ev.timestampNs = nowNs_();
        enqueue_(ev);
    }

    // ---- Introspection helpers (thread-safe) ----
    size_t pending() const {
        std::lock_guard<std::mutex> lg(mutex_);
        return queue_.size();
    }

    std::optional<Event> peek() const {
        std::lock_guard<std::mutex> lg(mutex_);
        if (queue_.empty()) return std::nullopt;
        return queue_.front();
    }

    std::optional<Event> pop() {
        std::lock_guard<std::mutex> lg(mutex_);
        if (queue_.empty()) return std::nullopt;
        Event ev = queue_.front();
        queue_.pop_front();
        return ev;
    }

    void clear() {
        std::lock_guard<std::mutex> lg(mutex_);
        queue_.clear();
    }

    // ---- Haptics (command) API ----

    // Returns true if this provider can execute haptics (simulation or OS backend).
    bool hapticsSupported() const noexcept {
        if (mode_ == BackendMode::Simulation) return true;
#if defined(PATHIO_BACKEND_MACOS)
        return &PSGameControllerApplyRumble != nullptr;
#else
        return false;
#endif
    }

    // Apply a haptics command (thread-safe). Returns an Error if unsupported.
    std::optional<Error> applyHaptics(Command const& cmd) {
        // Clamp inputs
        Command clamped = cmd;
        clamped.lowFrequency  = clamp01_(clamped.lowFrequency);
        clamped.highFrequency = clamp01_(clamped.highFrequency);

        if (mode_ == BackendMode::Simulation) {
            {
                std::lock_guard<std::mutex> lg(mutex_);
                lastHaptics_ = clamped;
            }
            // Simulation has no external effect; accept the command.
            return std::nullopt;
        }
#if defined(PATHIO_BACKEND_MACOS)
        if (&PSGameControllerApplyRumble != nullptr) {
            bool ok = PSGameControllerApplyRumble(deviceId_, clamped.lowFrequency, clamped.highFrequency, clamped.durationMs);
            if (ok) return std::nullopt;
            return Error{Error::Code::InvalidPermissions, "Gamepad haptics command rejected by GameController"};
        }
        return Error{Error::Code::InvalidPermissions, "Gamepad haptics OS helper not linked (macOS)"};
#else
        return Error{Error::Code::InvalidPermissions, "Gamepad haptics unsupported on this platform"};
#endif
    }

public:
    // ---- PathSpaceBase overrides ----

    // in(): accept HapticsCommand on control paths ("/rumble" or "/haptics" suffix), otherwise reject by type.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override {
        InsertReturn ret;

        // Only HapticsCommand is supported for in()
        if (data.metadata.typeInfo != &typeid(Command)) {
            ret.errors.emplace_back(Error::Code::InvalidType, "PathIOGamepad only accepts HapticsCommand for in()");
            return ret;
        }
        auto const* cmd = reinterpret_cast<Command const*>(data.obj);
        if (!cmd) {
            ret.errors.emplace_back(Error::Code::MalformedInput, "Null HapticsCommand pointer");
            return ret;
        }

        // Best-effort path tail routing: accept empty, "rumble"/"haptics", or suffixes "/rumble" or "/haptics"
        const std::string tail = std::string(path.currentToEnd());
        if (!tail.empty()) {
            bool ok =
                (tail == "rumble" || tail == "haptics") ||
                endsWith_(tail, "/rumble") || endsWith_(tail, "/haptics");
            if (!ok) {
                ret.errors.emplace_back(Error::Code::InvalidPath, "Unsupported control path for gamepad haptics");
                return ret;
            }
        }

        if (auto err = applyHaptics(*cmd)) {
            ret.errors.push_back(*err);
            return ret;
        }

        ret.nbrValuesInserted = 1;
        return ret;
    }

    // out(): serve typed GamepadEvent with peek/pop and optional blocking semantics.
    auto out(Iterator const& /*path*/,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override {
        if (inputMetadata.typeInfo != &typeid(Event)) {
            return Error{Error::Code::InvalidType, "PathIOGamepad only supports GamepadEvent reads"};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Null output pointer for PathIOGamepad::out"};
        }

        // Fast path: try without blocking
        {
            std::lock_guard<std::mutex> lg(mutex_);
            if (!queue_.empty()) {
                Event ev = queue_.front();
                if (options.doPop) {
                    queue_.pop_front();
                }
                *reinterpret_cast<Event*>(obj) = ev;
                return std::nullopt;
            }
        }

        // Non-blocking requested
        if (!options.doBlock) {
            return Error{Error::Code::NoObjectFound, "No gamepad event available"};
        }

        // Blocking wait
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (!cv_.wait_until(lk, deadline, [this] { return !queue_.empty(); })) {
                return Error{Error::Code::Timeout, "Timed out waiting for gamepad event"};
            }
            if (queue_.empty()) {
                return Error{Error::Code::NoObjectFound, "No gamepad event available after wake"};
            }
            Event ev = queue_.front();
            if (options.doPop) {
                queue_.pop_front();
            }
            *reinterpret_cast<Event*>(obj) = ev;
        }
        return std::nullopt;
    }

    auto shutdown() -> void override {
        // No special shutdown at the moment.
    }

    auto notify(std::string const& /*notificationPath*/) -> void override {
        // Provider does not rely on external notifications.
    }

    // Capture mount prefix to enable targeted notifications on enqueues.
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
        std::lock_guard<std::mutex> lg(mutex_);
        mountPrefix_ = std::move(prefix);
    }

private:
    void enqueue_(Event const& ev) {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            queue_.push_back(ev);
        }
        cv_.notify_all();
        notifyTargets_();
    }

    void notifyTargets_() {
        if (auto ctx = this->getContext()) {
            std::string mount;
            {
                std::lock_guard<std::mutex> lg(mutex_);
                mount = mountPrefix_;
            }
            if (!mount.empty()) {
                ctx->notify(mount);
                ctx->notify(mount + "/events");
            } else {
                ctx->notifyAll();
            }
        }
    }

    static uint64_t nowNs_() {
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
    }

    static float clamp01_(float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    static bool endsWith_(std::string const& s, std::string const& suffix) {
        if (s.size() < suffix.size()) return false;
        return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
    }

private:
    BackendMode                mode_{BackendMode::Off};
    int                        deviceId_{0};

    mutable std::mutex         mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>          queue_;
    std::string                mountPrefix_;

    // Last applied haptics (simulation or OS backend if integrated)
    std::optional<Command>     lastHaptics_;
};

} // namespace SP