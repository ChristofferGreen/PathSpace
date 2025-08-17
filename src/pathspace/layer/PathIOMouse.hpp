#pragma once
#include "layer/PathIO.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <atomic>
#include <thread>

namespace SP {

// Mouse button identifiers (USB HID-like mapping for convenience)
enum class MouseButton : int {
    Left   = 1,
    Right  = 2,
    Middle = 3,
    Button4 = 4,
    Button5 = 5
};

// High-level mouse event kinds
enum class MouseEventType {
    Move,
    ButtonDown,
    ButtonUp,
    Wheel,
    AbsoluteMove
};

// Event structure produced by mouse devices/backends
struct MouseEvent {
    int                                     deviceId = 0;
    MouseEventType                          type     = MouseEventType::Move;

    // Relative deltas (Move)
    int                                     dx       = 0;
    int                                     dy       = 0;

    // Absolute coordinates (AbsoluteMove)
    int                                     x        = -1;
    int                                     y        = -1;

    // Buttons and wheel
    MouseButton                             button   = MouseButton::Left;
    int                                     wheel    = 0; // positive/negative ticks

    // Monotonic timestamp in nanoseconds for ordering/merging
    uint64_t                                 timestampNs = 0;
};

/**
 * PathIOMouse — concrete IO provider for mouse devices.
 *
 * Notes:
 * - This class does not know anything about where it is mounted in a parent PathSpace.
 * - It exposes a thread-safe simulated event queue API to feed events from tests or
 *   platform backends (macOS, etc). The out()/in() behavior is inherited from PathIO.
 * - A future implementation will override out() to deliver MouseEvent directly
 *   (peek or pop depending on the Out options), and use notify(...) to wake waiters.
 */
class PathIOMouse final : public PathIO {
public:
    using Event = MouseEvent;

    enum class BackendMode { Off, Auto, Simulation, OS };

    explicit PathIOMouse(BackendMode mode = BackendMode::Off) {
        mode_ = mode;
#if defined(PATHIO_BACKEND_MACOS)
        if (mode_ == BackendMode::Auto) {
            mode_ = BackendMode::OS;
        }
        if (mode_ == BackendMode::OS) {
            osInit_();
        }
#else
        if (mode_ == BackendMode::Auto) {
            mode_ = BackendMode::Simulation;
        }
#endif
        if (mode_ != BackendMode::Off) {
            running_.store(true, std::memory_order_release);
            worker_ = std::thread([this] { this->runLoop_(); });
        }
    }
    ~PathIOMouse() {
#if defined(PATHIO_BACKEND_MACOS)
        if (running_.load(std::memory_order_acquire) && mode_ == BackendMode::OS) {
            osShutdown_();
        }
#endif
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // ---- Construction hooks ----
    // Backend selection occurs in the constructor; background worker is stopped in the destructor.






    // ---- Simulation API (thread-safe) ----

    // Enqueue a generic event (from tests or platform backends)
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

    // Relative move (dx, dy)
    void simulateMove(int dx, int dy, int deviceId = 0) {
        Event ev;
        ev.deviceId = deviceId;
        ev.type     = MouseEventType::Move;
        ev.dx       = dx;
        ev.dy       = dy;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }

    // Absolute move (x, y)
    void simulateAbsolute(int x, int y, int deviceId = 0) {
        Event ev;
        ev.deviceId = deviceId;
        ev.type     = MouseEventType::AbsoluteMove;
        ev.x        = x;
        ev.y        = y;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }

    // Button down/up
    void simulateButtonDown(MouseButton button, int deviceId = 0) {
        Event ev;
        ev.deviceId = deviceId;
        ev.type     = MouseEventType::ButtonDown;
        ev.button   = button;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }
    void simulateButtonUp(MouseButton button, int deviceId = 0) {
        Event ev;
        ev.deviceId = deviceId;
        ev.type     = MouseEventType::ButtonUp;
        ev.button   = button;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }

    // Wheel ticks (+/-)
    void simulateWheel(int ticks, int deviceId = 0) {
        Event ev;
        ev.deviceId = deviceId;
        ev.type     = MouseEventType::Wheel;
        ev.wheel    = ticks;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }

    // ---- Introspection helpers ----

    // Number of pending simulated events
    size_t pending() const {
        std::lock_guard<std::mutex> lg(mutex_);
        return queue_.size();
    }

    // Peek at the front event (does not pop). Returns std::nullopt if empty.
    std::optional<Event> peek() const {
        std::lock_guard<std::mutex> lg(mutex_);
        if (queue_.empty()) return std::nullopt;
        return queue_.front();
    }

    // Pop the front event if any; returns it or std::nullopt if empty.
    std::optional<Event> pop() {
        std::lock_guard<std::mutex> lg(mutex_);
        if (queue_.empty()) return std::nullopt;
        Event ev = queue_.front();
        queue_.pop_front();
        return ev;
    }

    // Clear all pending events
    void clear() {
        std::lock_guard<std::mutex> lg(mutex_);
        queue_.clear();
    }

protected:
    // Derived implementations (future) can use these for blocking waits
    template <class Pred>
    bool waitFor(std::chrono::steady_clock::time_point deadline, Pred&& pred) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_until(lk, deadline, std::forward<Pred>(pred));
    }

public:
    // Serve typed mouse events with peek/pop and optional blocking semantics.
    // - If options.doPop is true: pop the front event into obj; otherwise peek without consuming.
    // - If the queue is empty:
    //    * If options.doBlock is false: return NoObjectFound.
    //    * If options.doBlock is true: wait until timeout for an event to arrive; return Timeout on expiry.
    auto out(Iterator const& /*path*/, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override {
        // Type-check: only support MouseEvent payloads here
        if (inputMetadata.typeInfo != &typeid(Event)) {
            return Error{Error::Code::InvalidType, "Mouse provider only supports MouseEvent"};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Null output pointer"};
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
            return Error{Error::Code::NoObjectFound, "No mouse event available"};
        }

        // Blocking path: wait until an event is available or timeout expires
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);

        bool ready = waitFor(deadline, [&] { return !queue_.empty(); });
        if (!ready) {
            return Error{Error::Code::Timeout, "Timed out waiting for mouse event"};
        }

        // Event is available now
        {
            std::lock_guard<std::mutex> lg(mutex_);
            if (queue_.empty()) {
                // Rare race: event consumed by another thread before we reacquired the lock
                return Error{Error::Code::NoObjectFound, "No mouse event available after wake"};
            }
            Event ev = queue_.front();
            if (options.doPop) {
                queue_.pop_front();
            }
            *reinterpret_cast<Event*>(obj) = ev;
        }
        return std::nullopt;
    }

private:
    // Worker loop that sources events from the OS or simulates when no OS integration is available.
    void runLoop_() {
        while (running_.load(std::memory_order_acquire)) {
#if defined(PATHIO_BACKEND_MACOS)
            if (mode_ == BackendMode::Simulation) {
                // Minimal simulation placeholder
                this->simulateMove(1, 0, /*deviceId=*/0);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            } else {
                // OS-backed poll (stubbed; implement with CGEventTap/IOKit HID)
                osPollOnce_();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
#else
            // Non-macOS: provide a light simulation when requested; otherwise idle
            if (mode_ == BackendMode::Simulation) {
                this->simulateMove(1, 0, /*deviceId=*/0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
        }
    }

private:
    std::atomic<bool>         running_{false};
    std::thread               worker_;
    BackendMode               mode_{BackendMode::Off};

#if defined(PATHIO_BACKEND_MACOS)
    // macOS OS hook stubs (to be implemented with CGEventTap / IOKit HID)
    void osInit_() {}
    void osShutdown_() {}
    void osPollOnce_() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
#endif

    mutable std::mutex        mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>         queue_;
};

} // namespace SP