#pragma once

// macOS PathIO backend skeletons
//
// This header provides skeleton classes for macOS device backends that
// feed events into PathIOMice / PathIOKeyboard. They are intentionally
// header-only placeholders that compile on all platforms, but only define
// the backend classes when PATHIO_BACKEND_MACOS is enabled.
//
// To enable on macOS:
// - Configure the build with -DPATHIO_BACKEND_MACOS=1 (or set ENABLE_PATHIO_MACOS=ON in CMake)
// - Provide .mm/.cpp implementation files that integrate with CoreGraphics
//   (CGEventTap) and/or IOKit HID to translate OS events into the simulateEvent()
//   calls on the respective base classes.
// - Keep CI using the simulation-only providers; guard real backends with
//   platform checks and optional CMake flags.

#include "layer/PathIOMice.hpp"
#include "layer/PathIOKeyboard.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace SP {

#ifdef PATHIO_BACKEND_MACOS

// PathIOMiceMacOS — macOS backend that sources mouse events via CGEventTap or IOKit HID.
// Notes:
// - Derives from PathIOMice and pushes events via simulateEvent(...).
// - start() spins up a background thread to install event taps/managers.
// - stop() tears down the backend and joins the thread.
// - This is only a skeleton; platform integration belongs in a .mm/.cpp file.
class PathIOMiceMacOS final : public PathIOMice {
public:
    PathIOMiceMacOS() = default;
    ~PathIOMiceMacOS() { stop(); }

    // Start capturing mouse events from the OS (idempotent).
    // Returns true if the backend transitions to running or is already running.
    bool start() {
        bool expected = false;
        if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            worker_ = std::thread([this] { this->runLoop_(); });
            return true;
        }
        return true;
    }

    // Stop capturing mouse events and join the worker (safe to call multiple times).
    void stop() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            // Signal the OS event loop/tap to stop (to be implemented in .mm)
            // e.g., post CFRunLoopStop to the run loop owning the tap.
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    // Worker loop (to be implemented in .mm/.cpp):
    // - Install CGEventTap / IOHIDManager
    // - Translate system events into PathIOMice::simulateEvent(...)
    // - Pump run loop until 'running_' becomes false
    void runLoop_() {
        // Placeholder loop; real implementation lives in the platform source.
        while (running_.load(std::memory_order_acquire)) {
            // Sleep briefly to avoid busy loop in the skeleton.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    std::atomic<bool> running_{false};
    std::thread       worker_;
};

// PathIOKeyboardMacOS — macOS backend that sources keyboard events via CGEventTap or IOKit HID.
// Notes:
// - Derives from PathIOKeyboard and pushes events via simulateEvent(...).
// - start() spins up a background thread to install event taps/managers.
// - stop() tears down the backend and joins the thread.
// - This is only a skeleton; platform integration belongs in a .mm/.cpp file.
class PathIOKeyboardMacOS final : public PathIOKeyboard {
public:
    PathIOKeyboardMacOS() = default;
    ~PathIOKeyboardMacOS() { stop(); }

    // Start capturing keyboard events from the OS (idempotent).
    bool start() {
        bool expected = false;
        if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            worker_ = std::thread([this] { this->runLoop_(); });
            return true;
        }
        return true;
    }

    // Stop capturing keyboard events and join the worker (safe to call multiple times).
    void stop() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            // Signal the OS event loop/tap to stop (to be implemented in .mm)
            // e.g., post CFRunLoopStop to the run loop owning the tap.
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    // Worker loop (to be implemented in .mm/.cpp):
    // - Install CGEventTap / IOHIDManager
    // - Translate system events into PathIOKeyboard::simulateEvent(...)
    // - Pump run loop until 'running_' becomes false
    void runLoop_() {
        // Placeholder loop; real implementation lives in the platform source.
        while (running_.load(std::memory_order_acquire)) {
            // Sleep briefly to avoid busy loop in the skeleton.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    std::atomic<bool> running_{false};
    std::thread       worker_;
};

#endif // PATHIO_BACKEND_MACOS

} // namespace SP