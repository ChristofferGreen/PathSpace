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
#include <ostream>
#if defined(PATHIO_BACKEND_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

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

    // Stream operator (hidden friend) for convenient logging/printing
    friend std::ostream& operator<<(std::ostream& os, MouseEvent const& e) {
        switch (e.type) {
            case MouseEventType::Move:
                return os << "[pointer] move dx=" << e.dx << " dy=" << e.dy;
            case MouseEventType::AbsoluteMove:
                return os << "[pointer] abs x=" << e.x << " y=" << e.y;
            case MouseEventType::ButtonDown:
                return os << "[pointer] button down " << static_cast<int>(e.button);
            case MouseEventType::ButtonUp:
                return os << "[pointer] button up " << static_cast<int>(e.button);
            case MouseEventType::Wheel:
                return os << "[pointer] wheel " << e.wheel;
        }
        return os;
    }
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
        // Defer OS initialization to the worker thread's run loop (see osPollOnce_())
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
                // OS-backed poll
                osPollOnce_();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    // macOS OS event-tap backend (CGEventTap)
    static CGEventRef MouseTapCallback(CGEventTapProxy /*proxy*/, CGEventType type, CGEventRef event, void* refcon) {
        auto self = static_cast<PathIOMouse*>(refcon);
        if (!self) return event;

        // Re-enable tap if it gets disabled
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            if (self->eventTap_) CGEventTapEnable(self->eventTap_, true);
            return event;
        }

        // Map CG events to high-level mouse events
        switch (type) {
            case kCGEventMouseMoved:
            case kCGEventLeftMouseDragged:
            case kCGEventRightMouseDragged:
            case kCGEventOtherMouseDragged: {
                Event ev;
                ev.type = MouseEventType::Move;
                ev.dx   = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
                ev.dy   = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
                ev.timestampNs = CGEventGetTimestamp(event);
                self->simulateEvent(ev);
                break;
            }
            case kCGEventLeftMouseDown:
            case kCGEventRightMouseDown:
            case kCGEventOtherMouseDown: {
                Event ev;
                ev.type   = MouseEventType::ButtonDown;
                ev.button = (type == kCGEventLeftMouseDown)  ? MouseButton::Left
                          : (type == kCGEventRightMouseDown) ? MouseButton::Right
                                                            : MouseButton::Middle;
                ev.timestampNs = CGEventGetTimestamp(event);
                self->simulateEvent(ev);
                break;
            }
            case kCGEventLeftMouseUp:
            case kCGEventRightMouseUp:
            case kCGEventOtherMouseUp: {
                Event ev;
                ev.type   = MouseEventType::ButtonUp;
                ev.button = (type == kCGEventLeftMouseUp)  ? MouseButton::Left
                          : (type == kCGEventRightMouseUp) ? MouseButton::Right
                                                           : MouseButton::Middle;
                ev.timestampNs = CGEventGetTimestamp(event);
                self->simulateEvent(ev);
                break;
            }
            case kCGEventScrollWheel: {
                Event ev;
                ev.type  = MouseEventType::Wheel;
                // Use vertical axis (Axis1); positive = up
                ev.wheel = static_cast<int>(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1));
                ev.timestampNs = CGEventGetTimestamp(event);
                self->simulateEvent(ev);
                break;
            }
            default:
                break;
        }
        return event;
    }

    // Ensure the event tap is attached to the calling thread's runloop.
    // If an existing tap is bound to a different runloop, rebind it here.
    void osInit_() {
        CFRunLoopRef current = CFRunLoopGetCurrent();
        if (runLoopRef_ && runLoopRef_ != current) {
            osShutdown_();
        }
        if (eventTap_) {
            // Already initialized for this runloop
            return;
        }

        CGEventMask mask =
            CGEventMaskBit(kCGEventMouseMoved) |
            CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDragged) |
            CGEventMaskBit(kCGEventOtherMouseDragged) |
            CGEventMaskBit(kCGEventScrollWheel);

        eventTap_ = CGEventTapCreate(kCGSessionEventTap,
                                     kCGHeadInsertEventTap,
                                     kCGEventTapOptionDefault,
                                     mask,
                                     &MouseTapCallback,
                                     this);
        if (!eventTap_) {
            // Could not create tap (permissions?). Use position polling fallback (no special perms)
            fallbackActive_.store(true, std::memory_order_release);
            return;
        } else {
            fallbackActive_.store(false, std::memory_order_release);
        }

        eventSrc_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap_, 0);
        runLoopRef_  = current;

        if (eventSrc_ && runLoopRef_) {
            CFRunLoopAddSource(runLoopRef_, eventSrc_, kCFRunLoopCommonModes);
            CGEventTapEnable(eventTap_, true);
            osReady_.store(true, std::memory_order_release);
        }
    }

    void osShutdown_() {
        osReady_.store(false, std::memory_order_release);
        if (runLoopRef_ && eventSrc_) {
            CFRunLoopRemoveSource(runLoopRef_, eventSrc_, kCFRunLoopCommonModes);
        }
        if (eventSrc_) {
            CFRelease(eventSrc_);
            eventSrc_ = nullptr;
        }
        if (eventTap_) {
            CGEventTapEnable(eventTap_, false);
            CFRelease(eventTap_);
            eventTap_ = nullptr;
        }
        runLoopRef_ = nullptr;
        // Disable fallback polling
        fallbackActive_.store(false, std::memory_order_release);
    }

    // Poll once: service the current thread's runloop briefly
    void osPollOnce_() {
        if (!osReady_.load(std::memory_order_acquire)) {
            osInit_();
        }
        if (fallbackActive_.load(std::memory_order_acquire)) {
            // Permission-less fallback: poll global mouse location and emit AbsoluteMove
            CGPoint p = CGEventGetLocation(nullptr);
            // Throttle a bit
            static int lastX = std::numeric_limits<int>::min();
            static int lastY = std::numeric_limits<int>::min();
            int xi = static_cast<int>(p.x);
            int yi = static_cast<int>(p.y);
            if (xi != lastX || yi != lastY) {
                lastX = xi;
                lastY = yi;
                this->simulateAbsolute(xi, yi, /*deviceId=*/0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            return;
        }
        if (runLoopRef_) {
            // Process pending events for a short slice without blocking indefinitely
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // macOS event-tap state
    CFMachPortRef       eventTap_ = nullptr;
    CFRunLoopSourceRef  eventSrc_ = nullptr;
    CFRunLoopRef        runLoopRef_  = nullptr;
    std::atomic<bool>   osReady_{false};
    std::atomic<bool>   fallbackActive_{false};
#endif

    mutable std::mutex        mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>         queue_;
};

} // namespace SP