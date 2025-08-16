#pragma once
#include "layer/PathIO.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <typeinfo>

namespace SP {

/**
 * PathIOPointerMixer — aggregates pointer events (mouse/tablet/pen) from multiple sources.
 *
 * Characteristics:
 * - Path-agnostic: can be mounted anywhere in a parent PathSpace.
 * - Typed out()/take(): serves PointerEvent with peek (non-pop) and pop semantics and supports blocking waits.
 * - Simulation API: feed events from tests or platform backends via simulate* helpers or simulateEvent(Event).
 * - Concurrency: thread-safe enqueue/peek/pop with condition_variable-based blocking reads.
 * - Notifications: when mounted with a shared context, simulateEvent will wake waiters via notifyAll().
 *
 * Notes:
 * - This mixer does not enforce a single "active" source by default; it merges all events by arrival order.
 * - If you need per-source selection/priority, add policy methods later (e.g., setSourcePriority, filterSource).
 */
class PathIOPointerMixer final : public PathIO {
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

        // Relative deltas (Move)
        int               dx          = 0;
        int               dy          = 0;

        // Absolute coordinates (AbsoluteMove)
        int               x           = -1;
        int               y           = -1;

        // Buttons and wheel
        PointerButton     button      = PointerButton::Left;
        int               wheel       = 0;  // positive/negative ticks

        // Monotonic timestamp in nanoseconds for ordering/merging
        uint64_t          timestampNs = 0;
    };

    using PointerEvent = Event;

    PathIOPointerMixer() = default;

    // ---- Simulation/back-end API (thread-safe) ----

    // Enqueue an event from a given source. Notifies waiters if mounted with a shared context.
    void simulateEvent(Event const& ev) {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            queue_.push_back(ev);
        }
        cv_.notify_all();
        if (auto ctx = this->getContext()) {
            ctx->notifyAll();
        }
    }

    // Relative move (dx, dy) from sourceId
    void simulateMove(int dx, int dy, int sourceId = 0) {
        Event ev;
        ev.sourceId = sourceId;
        ev.type     = PointerEventType::Move;
        ev.dx       = dx;
        ev.dy       = dy;
        ev.timestampNs = nowNs_();
        simulateEvent(ev);
    }

    // Absolute move (x, y) from sourceId
    void simulateAbsolute(int x, int y, int sourceId = 0) {
        Event ev;
        ev.sourceId = sourceId;
        ev.type     = PointerEventType::AbsoluteMove;
        ev.x        = x;
        ev.y        = y;
        ev.timestampNs = nowNs_();
        simulateEvent(ev);
    }

    // Button down/up from sourceId
    void simulateButtonDown(PointerButton button, int sourceId = 0) {
        Event ev;
        ev.sourceId = sourceId;
        ev.type     = PointerEventType::ButtonDown;
        ev.button   = button;
        ev.timestampNs = nowNs_();
        simulateEvent(ev);
    }
    void simulateButtonUp(PointerButton button, int sourceId = 0) {
        Event ev;
        ev.sourceId = sourceId;
        ev.type     = PointerEventType::ButtonUp;
        ev.button   = button;
        ev.timestampNs = nowNs_();
        simulateEvent(ev);
    }

    // Wheel ticks (+/-) from sourceId
    void simulateWheel(int ticks, int sourceId = 0) {
        Event ev;
        ev.sourceId = sourceId;
        ev.type     = PointerEventType::Wheel;
        ev.wheel    = ticks;
        ev.timestampNs = nowNs_();
        simulateEvent(ev);
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

    // ---- PathSpaceBase override: typed read/take ----

    // Serve typed PointerEvent with peek/pop and optional blocking semantics.
    // - If options.doPop is true: pop the front event into obj; otherwise peek without consuming.
    // - If the queue is empty:
    //    * If options.doBlock is false: return NoObjectFound.
    //    * If options.doBlock is true: wait until timeout for an event to arrive; return Timeout on expiry.
    auto out(Iterator const& /*path*/,
             InputMetadata const& inputMetadata,
             Out const& options,
             void* obj) -> std::optional<Error> override {
        // Type-check: only support Event payloads here
        if (inputMetadata.typeInfo != &typeid(Event)) {
            return Error{Error::Code::InvalidType, "PathIOPointerMixer only supports PointerEvent"};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Null output pointer for PathIOPointerMixer::out"};
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

        // No event and non-blocking read requested
        if (!options.doBlock) {
            return Error{Error::Code::NoObjectFound, "No pointer event available"};
        }

        // Blocking path: wait until an event is available or timeout expires
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);

        {
            std::unique_lock<std::mutex> lk(mutex_);
            // Wait predicate: queue is non-empty or deadline reached
            if (!cv_.wait_until(lk, deadline, [this] { return !queue_.empty(); })) {
                return Error{Error::Code::Timeout, "Timed out waiting for pointer event"};
            }
            // Event is available now
            if (queue_.empty()) {
                // Rare race: event consumed by another thread before we reacquired the lock
                return Error{Error::Code::NoObjectFound, "No pointer event available after wake"};
            }
            Event ev = queue_.front();
            if (options.doPop) {
                queue_.pop_front();
            }
            *reinterpret_cast<Event*>(obj) = ev;
        }
        return std::nullopt;
    }

protected:
    // Derived implementations (future) can use these for custom blocking behavior/predicates
    template <class Pred>
    bool waitFor(std::chrono::steady_clock::time_point deadline, Pred&& pred) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_until(lk, deadline, std::forward<Pred>(pred));
    }

private:
    static uint64_t nowNs_() {
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
    }

private:
    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>               queue_;
};

} // namespace SP