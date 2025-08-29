#pragma once
#include "PathSpace.hpp"

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
class PathIOMouse final : public PathSpaceBase {
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
    
    // Accept Event writes at ".../events" (relative to mount) and enqueue into the provider queue.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    
    // Cooperative shutdown: stop worker and join; also stop OS backend if active
    auto shutdown() -> void override;
    
    // Provider does not rely on external notifications
    auto notify(std::string const& notificationPath) -> void override;
    
    // ---- Construction hooks ----
    // Backend selection occurs in the constructor; background worker is stopped in the destructor.






    // Event production:
    // - Write MouseEvent at "events" (relative to mount) using insert<"/events">(Event)
    // - This provider enqueues events and wakes waiters accordingly

    // Introspection helpers removed:
    // - Prefer read/take with Out options for consumer-side access

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
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;

private:
    // Worker loop that sources events from the OS or simulates when no OS integration is available.
    void runLoop_() {
        while (running_.load(std::memory_order_acquire)) {
#if defined(PATHIO_BACKEND_MACOS)
            if (mode_ == BackendMode::Simulation) {
                // Minimal simulation placeholder
                {
                    Event ev{};
                    ev.type = MouseEventType::Move;
                    ev.dx = 1; ev.dy = 0;
                    std::lock_guard<std::mutex> lg(mutex_);
                    queue_.push_back(ev);
                }
                cv_.notify_all();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            } else {
                // OS-backed poll
                osPollOnce_();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
#else
            // Non-macOS: provide a light simulation when requested; otherwise idle
            if (mode_ == BackendMode::Simulation) {
                {
                    Event ev{};
                    ev.type = MouseEventType::Move;
                    ev.dx = 1; ev.dy = 0;
                    std::lock_guard<std::mutex> lg(mutex_);
                    queue_.push_back(ev);
                }
                cv_.notify_all();
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
                {
                    std::lock_guard<std::mutex> lg(self->mutex_);
                    self->queue_.push_back(ev);
                }
                self->cv_.notify_all();
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
                {
                    std::lock_guard<std::mutex> lg(self->mutex_);
                    self->queue_.push_back(ev);
                }
                self->cv_.notify_all();
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
                {
                    std::lock_guard<std::mutex> lg(self->mutex_);
                    self->queue_.push_back(ev);
                }
                self->cv_.notify_all();
                break;
            }
            case kCGEventScrollWheel: {
                Event ev;
                ev.type  = MouseEventType::Wheel;
                // Use vertical axis (Axis1); positive = up
                ev.wheel = static_cast<int>(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1));
                ev.timestampNs = CGEventGetTimestamp(event);
                {
                    std::lock_guard<std::mutex> lg(self->mutex_);
                    self->queue_.push_back(ev);
                }
                self->cv_.notify_all();
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
#if defined(PATHIO_BACKEND_MACOS)
        // Only initialize OS hooks when explicitly enabled (backend = OS)
        if (mode_ != BackendMode::OS) {
            fallbackActive_.store(false, std::memory_order_release);
            osReady_.store(false, std::memory_order_release);
            return;
        }

        CFRunLoopRef current = CFRunLoopGetCurrent();
        if (runLoopRef_ && runLoopRef_ != current) {
            osShutdown_();
        }
        if (eventTap_) {
            // Already initialized for this runloop
            return;
        }

        CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged) |
                           CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged) |
                           CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
                           CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
                           CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
                           CGEventMaskBit(kCGEventScrollWheel);

        eventTap_ = CGEventTapCreate(kCGHIDEventTap,
                                     kCGHeadInsertEventTap,
                                     kCGEventTapOptionDefault,
                                     mask,
                                     &MouseTapCallback,
                                     this);
        if (!eventTap_) {
            // Could not create tap (permissions?). Fall back to no OS sourcing; tests still function via insert()
            fallbackActive_.store(false, std::memory_order_release);
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
#else
        (void)path; // no-op on non-macOS builds
#endif
    }

    void osShutdown_() {
#if defined(PATHIO_BACKEND_MACOS)
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
#else
        // no-op when OS backend is disabled
#endif
    }

    // Poll once: service the current thread's runloop briefly
    void osPollOnce_() {
#if defined(PATHIO_BACKEND_MACOS)
        if (!osReady_.load(std::memory_order_acquire)) {
            osInit_();
        }
        if (mode_ != BackendMode::OS) {
            // Do nothing when not in OS mode; tests and apps can use insert("/events") to feed data
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        if (fallbackActive_.load(std::memory_order_acquire)) {
            // Permission-less fallback disabled by default
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
        if (runLoopRef_) {
            // Process pending events for a short slice without blocking indefinitely
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#else
        // Non-macOS / backend disabled: no-op
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
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