#pragma once
#include "PathSpace.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <typeinfo>
#include <string>

namespace SP {

/**
 * PathIOPointerMixer — aggregates pointer events (mouse/tablet/pen) from multiple sources.
 *
 * Characteristics:
 * - Path-agnostic: can be mounted anywhere in a parent PathSpace.
 * - Typed out()/take(): serves PointerEvent with peek (non-pop) and pop semantics and supports blocking waits.
 * - Production: feed events via insert at "events" (relative to mount) into this provider.
 * - Concurrency: thread-safe enqueue/peek/pop with condition_variable-based blocking reads.
 * - Notifications: when mounted with a shared context, event inserts will wake waiters via the provider/PathSpace context.
 *
 * Notes:
 * - This mixer does not enforce a single "active" source by default; it merges all events by arrival order.
 * - If you need per-source selection/priority, add policy methods later (e.g., setSourcePriority, filterSource).
 */
class PathIOPointerMixer final : public PathSpaceBase {
public:
    enum class PointerEventType {
        Move,         // Relative (dx, dy)
        AbsoluteMove, // Absolute (x, y)
        ButtonDown,
        ButtonUp,
        Wheel
    };

    enum class PointerButton : int {
        Left   = 1,
        Right  = 2,
        Middle = 3,
        Button4 = 4,
        Button5 = 5
    };

    struct Event {
        int               sourceId    = 0;  // Which upstream device produced this event
        PointerEventType  type        = PointerEventType::Move;
        int               dx          = 0;  // Relative deltas (Move)
        int               dy          = 0;
        int               x           = -1; // Absolute coordinates (AbsoluteMove)
        int               y           = -1;
        PointerButton     button      = PointerButton::Left; // Buttons and wheel
        int               wheel       = 0;  // positive/negative ticks
        uint64_t          timestampNs = 0;  // Monotonic timestamp in nanoseconds for ordering/merging
    };

    using PointerEvent = Event;

    PathIOPointerMixer() = default;

    // ---- Production write API (PathSpaceBase override) ----
    // Accept Event writes at ".../events" (relative to mount) and enqueue into the mixer queue.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;

    // ---- PathSpaceBase override: typed read/take ----
    // Serve typed PointerEvent with peek/pop and optional blocking semantics.
    // - If options.doPop is true: pop the front event into obj; otherwise peek without consuming.
    // - If the queue is empty:
    //    * If options.doBlock is false: return NoObjectFound.
    //    * If options.doBlock is true: wait until timeout for an event to arrive; return Timeout on expiry.
    auto out(Iterator const& path,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override;

    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;

protected:
    // Derived implementations (future) can use these for custom blocking behavior/predicates
    template <class Pred>
    bool waitFor(std::chrono::steady_clock::time_point deadline, Pred&& pred) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_until(lk, deadline, std::forward<Pred>(pred));
    }

private:
    static uint64_t nowNs_();

private:
    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>               queue_;
};

} // namespace SP