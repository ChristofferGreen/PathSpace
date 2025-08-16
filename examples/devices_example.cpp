#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIOMice.hpp>
#include <pathspace/layer/PathIOKeyboard.hpp>

// Optional macOS backend skeletons (compile with -DENABLE_PATHIO_MACOS=ON in CMake to define PATHIO_BACKEND_MACOS)
#ifdef PATHIO_BACKEND_MACOS
#include <pathspace/layer/macos/PathIO_macos.hpp>
#endif

using namespace SP;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};

static void sigint_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Simple inactivity tracker for detecting unplug based on no events over a window
struct ActivityTracker {
    std::atomic<int>  events{0};
    std::atomic<int>  idleTicks{0};
    std::atomic<bool> plugged{false};
};

int main() {
    std::signal(SIGINT, sigint_handler);

    auto space = std::make_shared<PathSpace>();

    // Mount mice and keyboard providers (path-agnostic providers; paths chosen by this example)
    auto mice = std::make_unique<PathIOMice>();
    auto* miceRaw = mice.get();
    {
        auto ret = space->insert<"/inputs/mice/0">(std::move(mice));
        if (!ret.errors.empty()) {
            std::cerr << "Failed to mount mice provider\n";
            return 1;
        }
    }

    auto keyboard = std::make_unique<PathIOKeyboard>();
    auto* kbRaw = keyboard.get();
    {
        auto ret = space->insert<"/inputs/keyboards/0">(std::move(keyboard));
        if (!ret.errors.empty()) {
            std::cerr << "Failed to mount keyboard provider\n";
            return 1;
        }
    }

#ifdef PATHIO_BACKEND_MACOS
    // Optionally start macOS backends if available (replace with real integrations as needed)
    PathIOMiceMacOS miceOs;
    PathIOKeyboardMacOS kbOs;
    miceOs.start();
    kbOs.start();
    std::cout << "[info] macOS backends started (PATHIO_BACKEND_MACOS)\n";
#endif

    std::cout << "Device example (no simulation). Ctrl-C to exit.\n";

    // Track "plugged" based on first-seen event; declare "unplug" if idle for N ticks.
    ActivityTracker miceAct, kbAct;

    // Reader thread: mice
    std::thread miceReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space->read<"/inputs/mice/0/events", PathIOMice::Event>(Block{250ms});
            if (r.has_value()) {
                auto const& e = *r;
                if (!miceAct.plugged.exchange(true, std::memory_order_acq_rel)) {
                    std::cout << "[plug-in] mice/0\n";
                }
                miceAct.events.fetch_add(1, std::memory_order_acq_rel);
                miceAct.idleTicks.store(0, std::memory_order_release);

                switch (e.type) {
                    case MouseEventType::Move:
                        std::cout << "[mouse] move dx=" << e.dx << " dy=" << e.dy << "\n";
                        break;
                    case MouseEventType::AbsoluteMove:
                        std::cout << "[mouse] abs x=" << e.x << " y=" << e.y << "\n";
                        break;
                    case MouseEventType::ButtonDown:
                        std::cout << "[mouse] button down " << static_cast<int>(e.button) << "\n";
                        break;
                    case MouseEventType::ButtonUp:
                        std::cout << "[mouse] button up " << static_cast<int>(e.button) << "\n";
                        break;
                    case MouseEventType::Wheel:
                        std::cout << "[mouse] wheel " << e.wheel << "\n";
                        break;
                }
            } else {
                // no event within block window
                miceAct.idleTicks.fetch_add(1, std::memory_order_acq_rel);
                if (miceAct.plugged.load(std::memory_order_acquire) && miceAct.idleTicks.load(std::memory_order_acquire) > 20) {
                    miceAct.plugged.store(false, std::memory_order_release);
                    std::cout << "[unplug] mice/0 (inactivity)\n";
                }
            }
        }
    });

    // Reader thread: keyboard
    std::thread keyboardReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space->read<"/inputs/keyboards/0/events", PathIOKeyboard::Event>(Block{250ms});
            if (r.has_value()) {
                auto const& e = *r;
                if (!kbAct.plugged.exchange(true, std::memory_order_acq_rel)) {
                    std::cout << "[plug-in] keyboards/0\n";
                }
                kbAct.events.fetch_add(1, std::memory_order_acq_rel);
                kbAct.idleTicks.store(0, std::memory_order_release);

                switch (e.type) {
                    case KeyEventType::KeyDown:
                        std::cout << "[key] down code=" << e.keycode << " mods=" << e.modifiers << "\n";
                        break;
                    case KeyEventType::KeyUp:
                        std::cout << "[key] up code=" << e.keycode << " mods=" << e.modifiers << "\n";
                        break;
                    case KeyEventType::Text:
                        std::cout << "[key] text \"" << e.text << "\" mods=" << e.modifiers << "\n";
                        break;
                }
            } else {
                kbAct.idleTicks.fetch_add(1, std::memory_order_acq_rel);
                if (kbAct.plugged.load(std::memory_order_acquire) && kbAct.idleTicks.load(std::memory_order_acquire) > 20) {
                    kbAct.plugged.store(false, std::memory_order_release);
                    std::cout << "[unplug] keyboards/0 (inactivity)\n";
                }
            }
        }
    });

    // Idle monitor to print periodic summaries (optional)
    std::thread monitor([&] {
        while (g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(2s);
            auto m = miceAct.events.exchange(0, std::memory_order_acq_rel);
            auto k = kbAct.events.exchange(0, std::memory_order_acq_rel);
            if (m || k) {
                std::cout << "[summary] mice events=" << m << " key events=" << k << "\n";
            }
        }
    });

    miceReader.join();
    keyboardReader.join();
    monitor.join();

#ifdef PATHIO_BACKEND_MACOS
    kbOs.stop();
    miceOs.stop();
#endif

    std::cout << "Exiting device example.\n";
    return 0;
}

/*
Build notes:

- This example depends on the library target "PathSpace". Add an examples section to your CMake
  to build it conditionally (e.g., -DBUILD_PATHSPACE_EXAMPLES=ON):

  option(BUILD_PATHSPACE_EXAMPLES "Build PathSpace examples" OFF)
  if(BUILD_PATHSPACE_EXAMPLES)
      add_executable(devices_example examples/devices_example.cpp)
      target_link_libraries(devices_example PRIVATE PathSpace)
      if(APPLE AND ENABLE_PATHIO_MACOS)
          target_compile_definitions(devices_example PRIVATE PATHIO_BACKEND_MACOS=1)
      endif()
  endif()

- Run: ./devices_example
  - Ctrl-C to exit.
  - With -DDEVICES_EXAMPLE_SIMULATE=1 (default here), it will simulate plug/unplug and events.
  - When OS backends are implemented, mount those providers instead and remove the simulation flag.

*/