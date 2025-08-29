#pragma once
#include "PathSpace.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <atomic>
#include <thread>
#include <ostream>
#if defined(PATHIO_BACKEND_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace SP {

// Common keyboard modifier bitmask (optional usage by clients/tests)
enum KeyModifier : uint32_t {
    Mod_None   = 0,
    Mod_Shift  = 1u << 0,
    Mod_Ctrl   = 1u << 1,
    Mod_Alt    = 1u << 2,
    Mod_Meta   = 1u << 3, // Cmd on macOS / Windows key on Windows
};

// High-level keyboard event kinds
enum class KeyEventType {
    KeyDown,
    KeyUp,
    Text // UTF-8 text input (composition resolved)
};

// Event structure produced by keyboard devices/backends
struct KeyboardEvent {
    int                                     deviceId  = 0;
    KeyEventType                            type      = KeyEventType::KeyDown;

    // Key code for KeyDown/KeyUp (platform/HID dependent; semantic mapping left to clients)
    int                                     keycode   = 0;

    // Modifier state snapshot for the event
    uint32_t                                modifiers = Mod_None;

    // UTF-8 text payload for Text events (unused for KeyDown/KeyUp)
    std::string                             text;

    // Monotonic timestamp in nanoseconds for ordering/merging
    uint64_t                                timestampNs = 0;

    // Stream operator (hidden friend) for convenient logging/printing
    friend std::ostream& operator<<(std::ostream& os, KeyboardEvent const& e) {
        switch (e.type) {
            case KeyEventType::KeyDown:
                return os << "[text] key down code=" << e.keycode << " mods=" << e.modifiers;
            case KeyEventType::KeyUp:
                return os << "[text] key up code=" << e.keycode << " mods=" << e.modifiers;
            case KeyEventType::Text:
                return os << "[text] \"" << e.text << "\" mods=" << e.modifiers;
        }
        return os;
    }
    };

/**
 * PathIOKeyboard — concrete IO provider for keyboard devices.
 *
 * Notes:
 * - This class does not know where it is mounted in a parent PathSpace.
 * - It exposes a thread-safe simulated event queue API to feed events from tests or
 *   platform backends (macOS, etc). Writes are unsupported; reads use out() to deliver KeyboardEvent.
 * - out() returns KeyboardEvent with peek-or-pop semantics (Out.doPop) and optional blocking (Out.doBlock).
 */
class PathIOKeyboard final : public PathSpaceBase {
public:
    using Event = KeyboardEvent;

    enum class BackendMode { Off, Auto, Simulation, OS };

    explicit PathIOKeyboard(BackendMode mode = BackendMode::Off) {
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

    ~PathIOKeyboard() {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Backend lifecycle is managed by constructor/destructor; start() removed.

    // Backend lifecycle is managed by constructor/destructor; stop() removed.

    // Accept Event writes at ".../events" (relative to mount) and enqueue into the provider queue.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;



    // Cooperative shutdown: stop worker and join if running
    auto shutdown() -> void override;

    // No-op notify (provider uses its own condition variable and notifies via context in out paths if needed)
    auto notify(std::string const& notificationPath) -> void override;

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

    // Key down/up
    void simulateKeyDown(int keycode, uint32_t modifiers = Mod_None, int deviceId = 0) {
        Event ev;
        ev.deviceId  = deviceId;
        ev.type      = KeyEventType::KeyDown;
        ev.keycode   = keycode;
        ev.modifiers = modifiers;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }
    void simulateKeyUp(int keycode, uint32_t modifiers = Mod_None, int deviceId = 0) {
        Event ev;
        ev.deviceId  = deviceId;
        ev.type      = KeyEventType::KeyUp;
        ev.keycode   = keycode;
        ev.modifiers = modifiers;
        auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
        ev.timestampNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
        simulateEvent(ev);
    }

    // Text input (UTF-8)
    void simulateText(std::string textUtf8, uint32_t modifiers = Mod_None, int deviceId = 0) {
        Event ev;
        ev.deviceId  = deviceId;
        ev.type      = KeyEventType::Text;
        ev.text      = std::move(textUtf8);
        ev.modifiers = modifiers;
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
    // Serve typed keyboard events with peek/pop and optional blocking semantics.
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
                static bool key_down = false;
                if (!key_down) {
                    this->simulateKeyDown(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = true;
                } else {
                    this->simulateKeyUp(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    this->simulateText("A", /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                // OS-backed: service event tap
                osPollOnce_();
            }
#else
            if (mode_ == BackendMode::Simulation) {
                static bool key_down = false;
                if (!key_down) {
                    this->simulateKeyDown(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = true;
                } else {
                    this->simulateKeyUp(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    this->simulateText("A", /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
        }
    }

private:
    std::atomic<bool>              running_{false};
    std::thread                    worker_;
    BackendMode                    mode_{BackendMode::Off};

#if defined(PATHIO_BACKEND_MACOS)
    // macOS CGEventTap backend for keyboard events
    static CGEventRef KeyTapCallback(CGEventTapProxy /*proxy*/, CGEventType type, CGEventRef event, void* refcon) {
        auto self = static_cast<PathIOKeyboard*>(refcon);
        if (!self) return event;

        // Re-enable tap if it gets disabled
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            if (self->eventTap_) CGEventTapEnable(self->eventTap_, true);
            return event;
        }

        switch (type) {
            case kCGEventKeyDown: {
                int keycode = static_cast<int>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
                self->simulateKeyDown(keycode, /*modifiers=*/Mod_None, /*deviceId=*/0);

                // Attempt to extract UTF-8 text for Text events
                // Extract Unicode text from the key event (permission-less, local)
                UniChar buffer[8] = {};
                UniCharCount actual = 0;
                CGEventKeyboardGetUnicodeString(event, (UniCharCount)(sizeof(buffer) / sizeof(buffer[0])), &actual, buffer);
                if (actual > 0) {
                    CFStringRef s = CFStringCreateWithCharacters(kCFAllocatorDefault, buffer, actual);
                    if (s) {
                        CFIndex len = CFStringGetLength(s);
                        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
                        std::string utf8;
                        utf8.resize(static_cast<size_t>(maxSize), '\0');
                        CFIndex used = 0;
                        if (CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false,
                                             reinterpret_cast<UInt8*>(&utf8[0]), maxSize - 1, &used) && used > 0) {
                            utf8.resize(static_cast<size_t>(used));
                            self->simulateText(std::move(utf8), Mod_None, /*deviceId=*/0);
                        }
                        CFRelease(s);
                    }
                }
                break;
            }
            case kCGEventKeyUp: {
                int keycode = static_cast<int>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
                self->simulateKeyUp(keycode, /*modifiers=*/Mod_None, /*deviceId=*/0);
                break;
            }
            default:
                break;
        }
        return event;
    }

    void osInit_() {
        CFRunLoopRef current = CFRunLoopGetCurrent();
        if (runLoopRef_ && runLoopRef_ != current) {
            osShutdown_();
        }
        if (eventTap_) {
            // Already initialized for this runloop
            return;
        }

        CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);

        eventTap_ = CGEventTapCreate(kCGHIDEventTap,
                                     kCGHeadInsertEventTap,
                                     kCGEventTapOptionDefault,
                                     mask,
                                     &KeyTapCallback,
                                     this);
        if (!eventTap_) {
            // Could not create tap (permissions?). Fall back to simulation so example still functions.
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
        // Disable fallback simulation on shutdown
        fallbackActive_.store(false, std::memory_order_release);
    }

    // Service the runloop briefly to process pending events (with simulation fallback)
    void osPollOnce_() {
        if (!osReady_.load(std::memory_order_acquire)) {
            osInit_();
        }

        // Permission-less fallback: simulate minimal key activity if tap could not be created
        if (fallbackActive_.load(std::memory_order_acquire)) {
            static bool key_down = false;
            if (!key_down) {
                // Directly enqueue a KeyDown event for 'A' with Shift modifier
                {
                    std::lock_guard<std::mutex> lg(mutex_);
                    Event ev{};
                    ev.deviceId    = 0;
                    ev.type        = KeyEventType::KeyDown;
                    ev.keycode     = 65; // 'A'
                    ev.modifiers   = Mod_Shift;
                    ev.timestampNs = 0;
                    queue_.push_back(ev);
                }
                cv_.notify_all();
                key_down = true;
            } else {
                // Enqueue KeyUp and Text events for 'A'
                {
                    std::lock_guard<std::mutex> lg(mutex_);
                    Event up{};
                    up.deviceId    = 0;
                    up.type        = KeyEventType::KeyUp;
                    up.keycode     = 65; // 'A'
                    up.modifiers   = Mod_Shift;
                    up.timestampNs = 0;
                    queue_.push_back(up);

                    Event text{};
                    text.deviceId    = 0;
                    text.type        = KeyEventType::Text;
                    text.text        = "A";
                    text.modifiers   = Mod_Shift;
                    text.timestampNs = 0;
                    queue_.push_back(text);
                }
                cv_.notify_all();
                key_down = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }

        if (runLoopRef_) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // macOS event tap state
    CFMachPortRef       eventTap_ = nullptr;
    CFRunLoopSourceRef  eventSrc_ = nullptr;
    CFRunLoopRef        runLoopRef_  = nullptr;
    std::atomic<bool>   osReady_{false};
    std::atomic<bool>   fallbackActive_{false};
#endif

    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>               queue_;
};

} // namespace SP