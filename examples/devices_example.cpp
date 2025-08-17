#include <atomic>

#include <csignal>
#include <iostream>


#include <thread>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIOMouse.hpp>
#include <pathspace/layer/PathIOKeyboard.hpp>


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

    auto space = PathSpace();

    // Mount mice and keyboard providers (path-agnostic providers; paths chosen by this example)
    // Prefer macOS backends when available, otherwise use generic providers.
    auto mouse = std::make_unique<PathIOMouse>();
    {
        auto ret = space.insert<"/inputs/mouse/0">(std::move(mouse));
        if (!ret.errors.empty()) {
            std::cerr << "Failed to mount mouse provider\n";
            return 1;
        }
    }

    auto keyboard = std::make_unique<PathIOKeyboard>();
    {
        auto ret = space.insert<"/inputs/keyboards/0">(std::move(keyboard));
        if (!ret.errors.empty()) {
            std::cerr << "Failed to mount keyboard provider\n";
            return 1;
        }
    }

    std::cout << "[info] input backends ready\n";

    std::cout << "Device example (no simulation). Ctrl-C to exit.\n";

    // Track "plugged" based on first-seen event; declare "unplug" if idle for N ticks.
    ActivityTracker mouseAct, kbAct;

    // Reader thread: mouse
    // Track an accumulated pointer position from relative moves; reset when unplugged
    std::atomic<int> mouseX{0};
    std::atomic<int> mouseY{0};
    std::thread mouseReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space.read<"/inputs/mouse/0/events", PathIOMouse::Event>(Block{250ms});
            if (r.has_value()) {
                auto const& e = *r;
                if (!mouseAct.plugged.exchange(true, std::memory_order_acq_rel)) {
                    std::cout << "[plug-in] mouse/0\n";
                }
                mouseAct.events.fetch_add(1, std::memory_order_acq_rel);
                mouseAct.idleTicks.store(0, std::memory_order_release);

                switch (e.type) {
                    case MouseEventType::Move: {
                        // Update accumulated coordinates based on relative deltas
                        int nx = mouseX.load(std::memory_order_acquire) + e.dx;
                        int ny = mouseY.load(std::memory_order_acquire) + e.dy;
                        mouseX.store(nx, std::memory_order_release);
                        mouseY.store(ny, std::memory_order_release);
                        std::cout << "[mouse] move dx=" << e.dx << " dy=" << e.dy
                                  << " | pos=(" << nx << "," << ny << ")\n";
                        break;
                    }
                    case MouseEventType::AbsoluteMove: {
                        // Set absolute coordinates directly
                        mouseX.store(e.x, std::memory_order_release);
                        mouseY.store(e.y, std::memory_order_release);
                        std::cout << "[mouse] abs x=" << e.x << " y=" << e.y
                                  << " | pos=(" << e.x << "," << e.y << ")\n";
                        break;
                    }
                    case MouseEventType::ButtonDown:
                        std::cout << "[mouse] button down " << static_cast<int>(e.button)
                                  << " | pos=(" << mouseX.load() << "," << mouseY.load() << ")\n";
                        break;
                    case MouseEventType::ButtonUp:
                        std::cout << "[mouse] button up " << static_cast<int>(e.button)
                                  << " | pos=(" << mouseX.load() << "," << mouseY.load() << ")\n";
                        break;
                    case MouseEventType::Wheel:
                        std::cout << "[mouse] wheel " << e.wheel
                                  << " | pos=(" << mouseX.load() << "," << mouseY.load() << ")\n";
                        break;
                }
            } else {
                // no event within block window
                mouseAct.idleTicks.fetch_add(1, std::memory_order_acq_rel);
                if (mouseAct.plugged.load(std::memory_order_acquire) && mouseAct.idleTicks.load(std::memory_order_acquire) > 20) {
                    mouseAct.plugged.store(false, std::memory_order_release);
                    // Reset accumulated position on unplug for clarity
                    mouseX.store(0, std::memory_order_release);
                    mouseY.store(0, std::memory_order_release);
                    std::cout << "[unplug] mouse/0 (inactivity)\n";
                }
            }
        }
    });

    // Reader thread: keyboard
    std::thread keyboardReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space.read<"/inputs/keyboards/0/events", PathIOKeyboard::Event>(Block{250ms});
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
            auto m = mouseAct.events.exchange(0, std::memory_order_acq_rel);
            auto k = kbAct.events.exchange(0, std::memory_order_acq_rel);
            if (m || k) {
                std::cout << "[summary] mouse events=" << m << " key events=" << k << "\n";
            }
        }
    });

    mouseReader.join();
    keyboardReader.join();
    monitor.join();

    // Backends lifecycle is automatic (constructor/destructor)

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
