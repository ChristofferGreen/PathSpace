#pragma once
#include "PathSpace.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>

namespace SP {
/**
 * PathIOGamepad — concrete IO provider for gamepad devices.
 *
 * Characteristics:
 * - Path-agnostic: can be mounted anywhere in a parent PathSpace.
 * - Typed out()/take(): serves GamepadEvent with peek (non-pop) and pop semantics and supports blocking waits.
 * - in(): accepts HapticsCommand (rumble) at control paths; executes on simulation backend and, when integrated,
 *   on OS backends.
 * - Concurrency: thread-safe enqueue/peek/pop with condition_variable-based blocking reads.
 * - Notifications: when mounted with a shared context, simulateEvent will wake waiters via targeted notify.
 */
class PathIOGamepad final : public PathSpaceBase {
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
        int        button      = -1;     // for ButtonDown/Up
        int        axis        = -1;     // for AxisMove
        float      value       = 0.0f;   // normalized [-1, 1] for axes
        uint64_t   timestampNs = 0;      // monotonic timestamp

        friend std::ostream& operator<<(std::ostream& os, Event const& e) {
            switch (e.type) {
                case EventType::Connected:    return os << "[gamepad] connected id=" << e.deviceId;
                case EventType::Disconnected: return os << "[gamepad] disconnected id=" << e.deviceId;
                case EventType::ButtonDown:   return os << "[gamepad] button down " << e.button;
                case EventType::ButtonUp:     return os << "[gamepad] button up " << e.button;
                case EventType::AxisMove:     return os << "[gamepad] axis " << e.axis << " value=" << e.value;
            }
            return os;
        }
    };

    struct HapticsCommand {
        float    lowFrequency  = 0.0f;
        float    highFrequency = 0.0f;
        uint32_t durationMs    = 0;
        static HapticsCommand constant(float low, float high, uint32_t ms) { return HapticsCommand{low, high, ms}; }
    };

    using GamepadEvent = Event;
    using Command      = HapticsCommand;

    enum class BackendMode { Off, Auto, Simulation, OS };

    explicit PathIOGamepad(BackendMode mode = BackendMode::Off, int deviceId = 0);
    ~PathIOGamepad();

    // ---- PathSpaceBase overrides ----
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& /*path*/, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& /*notificationPath*/) -> void override;
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;

private:
    void enqueue_(Event const& ev);
    void notifyTargets_();

    // Haptics (command) API (internal)
    bool                      hapticsSupported() const noexcept;
    std::optional<Error>      applyHaptics(Command const& cmd);

    static uint64_t nowNs_();
    static float    clamp01_(float v);
    static bool     endsWith_(std::string const& s, std::string const& suffix);

    BackendMode                mode_{BackendMode::Off};
    int                        deviceId_{0};

    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>               queue_;
    std::string                     mountPrefix_;
    std::optional<Command>          lastHaptics_;
};

} // namespace SP
