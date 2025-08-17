#pragma once
#include "layer/PathIO.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <atomic>
#include <thread>

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
};

/**
 * PathIOKeyboard — concrete IO provider for keyboard devices.
 *
 * Notes:
 * - This class does not know where it is mounted in a parent PathSpace.
 * - It exposes a thread-safe simulated event queue API to feed events from tests or
 *   platform backends (macOS, etc). The out()/in() behavior is inherited from PathIO.
 * - A future implementation will override out() to deliver KeyboardEvent directly
 *   (peek or pop depending on Out options), and use notify(...) to wake waiters.
 */
class PathIOKeyboard final : public PathIO {
public:
    using Event = KeyboardEvent;

    enum class BackendMode { Auto, Simulation, OS };

    explicit PathIOKeyboard(BackendMode mode = BackendMode::Auto) {
        mode_ = mode;
#if defined(PATHIO_BACKEND_MACOS)
        if (mode_ == BackendMode::Auto) {
            mode_ = BackendMode::OS;
        }
#else
        if (mode_ == BackendMode::Auto) {
            mode_ = BackendMode::Simulation;
        }
#endif
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { this->runLoop_(); });
    }

    ~PathIOKeyboard() {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Backend lifecycle is managed by constructor/destructor; start() removed.

    // Backend lifecycle is managed by constructor/destructor; stop() removed.

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
    auto out(Iterator const& /*path*/, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override {
        // Type-check: only support KeyboardEvent payloads here
        if (inputMetadata.typeInfo != &typeid(Event)) {
            return Error{Error::Code::InvalidType, "PathIOKeyboard only supports KeyboardEvent"};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Null output pointer for PathIOKeyboard::out"};
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
            return Error{Error::Code::NoObjectFound, "No keyboard event available"};
        }

        // Blocking path: wait until an event is available or timeout expires
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);

        bool ready = waitFor(deadline, [&] { return !queue_.empty(); });
        if (!ready) {
            return Error{Error::Code::Timeout, "Timed out waiting for keyboard event"};
        }

        // Event is available now
        {
            std::lock_guard<std::mutex> lg(mutex_);
            if (queue_.empty()) {
                // Rare race: event consumed by another thread before we reacquired the lock
                return Error{Error::Code::NoObjectFound, "No keyboard event available after wake"};
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
                // OS-backed poll (stub; wire to OS hooks when implemented)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    BackendMode                    mode_{BackendMode::Auto};

    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event>               queue_;
};

} // namespace SP