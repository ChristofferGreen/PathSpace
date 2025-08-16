/*
 Objective-C++ skeleton implementation for macOS PathIO backends.

 This file provides a minimal simulation loop that can be used on macOS to
 periodically generate input events for PathIOMiceMacOS and PathIOKeyboardMacOS.
 It does NOT wire into CGEventTap or IOKit HID yet — that integration should be
 added later in platform-specific code paths. For now, this offers a convenient
 way to exercise the providers and any layers (aliases, mixers) built on top.

 Build:
 - The contents of this file are compiled only when both __APPLE__ and
   PATHIO_BACKEND_MACOS are defined. Enable via CMake:
     -DENABLE_PATHIO_MACOS=ON
*/

#ifdef __APPLE__
#include "layer/macos/PathIO_macos.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace SP {
namespace macos {

#if defined(PATHIO_BACKEND_MACOS)

// Internal simulation state
namespace {
    std::atomic<bool> g_sim_running{false};
    std::thread       g_sim_thread;

    // Simple helper to sleep in milliseconds
    inline void sleep_ms(uint32_t ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    // Periodically generate a mix of mouse and keyboard events to exercise the providers.
    // - Mouse: a slow circular motion pattern using relative moves, periodic clicks, and wheel ticks.
    // - Keyboard: periodic keydown/keyup pairs (e.g., 'A' with Shift).
    void simulation_loop(PathIOMiceMacOS* mice, PathIOKeyboardMacOS* kb) {
        // Sanity guard
        if (!mice && !kb) return;

        // Start the providers (idempotent)
        if (mice) mice->start();
        if (kb)   kb->start();

        // Parameters for the motion/path
        // A simple circular motion with period over steps
        const double two_pi      = 6.283185307179586476925286766559;
        const int    period_steps = 120;     // 120 steps ~ 6 seconds at 50ms
        const int    sleep_period = 50;      // ms per iteration

        int step = 0;
        bool key_down = false;

        while (g_sim_running.load(std::memory_order_acquire)) {
            // Mouse simulation (relative)
            if (mice) {
                double t = (static_cast<double>(step % period_steps) / period_steps) * two_pi;
                // Small circle deltas
                int dx = static_cast<int>(std::round(2.0 * std::cos(t)));
                int dy = static_cast<int>(std::round(2.0 * std::sin(t)));

                if (dx != 0 || dy != 0) {
                    mice->simulateMove(dx, dy, /*deviceId=*/0);
                }

                // Every ~1.5s, simulate a left button click
                if (step % 30 == 0) {
                    mice->simulateButtonDown(MouseButton::Left, /*deviceId=*/0);
                }
                if (step % 30 == 2) {
                    mice->simulateButtonUp(MouseButton::Left, /*deviceId=*/0);
                }

                // Every ~3s, simulate a small wheel event
                if (step % 60 == 10) {
                    mice->simulateWheel(+1, /*deviceId=*/0);
                }
            }

            // Keyboard simulation: toggle Shift+'A' down/up sequence every ~2s
            if (kb) {
                if (step % 40 == 0 && !key_down) {
                    kb->simulateKeyDown(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = true;
                } else if (step % 40 == 5 && key_down) {
                    kb->simulateKeyUp(/*keycode=*/65 /* 'A' */, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    // Simulate text input for 'A'
                    kb->simulateText("A", /*modifiers=*/Mod_Shift, /*deviceId=*/0);
                    key_down = false;
                }
            }

            ++step;
            sleep_ms(sleep_period);
        }

        // Ensure providers are stopped at loop end
        if (kb)   kb->stop();
        if (mice) mice->stop();
    }
} // namespace (anonymous)

// Public API: start/stop a simple simulation for macOS providers.
// These are convenience helpers to exercise the providers without real OS hooks.
//
// Usage example:
//   PathIOMiceMacOS mice;
//   PathIOKeyboardMacOS kb;
//   SP::macos::start_simulation(&mice, &kb);
//   ... run tests / interact ...
//   SP::macos::stop_simulation();
//
inline void start_simulation(PathIOMiceMacOS* mice, PathIOKeyboardMacOS* kb) {
    bool expected = false;
    if (!g_sim_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // already running
        return;
    }
    g_sim_thread = std::thread([mice, kb] { simulation_loop(mice, kb); });
}

inline void stop_simulation() {
    bool expected = true;
    if (!g_sim_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        // already stopped
        return;
    }
    if (g_sim_thread.joinable()) {
        g_sim_thread.join();
    }
}

#else // PATHIO_BACKEND_MACOS

// If the backend macro is not enabled, provide no-op stubs so this file still compiles on Apple platforms
inline void start_simulation(PathIOMiceMacOS*, PathIOKeyboardMacOS*) {}
inline void stop_simulation() {}

#endif // PATHIO_BACKEND_MACOS

} // namespace macos
} // namespace SP

#else // __APPLE__

// Non-Apple platforms: provide empty TU so this file can exist cross-platform.
namespace SP { namespace macos {
    // No-op placeholders
} }

#endif // __APPLE__
