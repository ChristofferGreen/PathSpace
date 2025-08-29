#include <atomic>

#include <csignal>
#include <iostream>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/layer/io/PathIOGamepad.hpp>

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

// Minimal aliases to keep the example readable and close to the sketch
using PointerDeviceEvent = PathIOMouse::Event;
using TextInputDeviceEvent = PathIOKeyboard::Event;
using GamepadEvent = PathIOGamepad::Event;
// Stream events via operator<<; no custom print overloads
#if defined(__APPLE__)
namespace SP {
    void PSInitLocalEventWindow(PathIOMouse*, PathIOKeyboard*);
    void PSPollLocalEventWindow();
    void PSInitGameControllerInput(PathIOGamepad*);
}
#endif

// Print helpers: accept Expected<T> and only print when an event is available
namespace SP {
static inline std::ostream& operator<<(std::ostream& os, Error const& err) {
    os << "[error]";
    if (err.message.has_value()) {
        os << " " << *err.message;
    } else {
        os << " code=" << static_cast<int>(err.code);
    }
    return os;
}
template <typename T>
static inline std::ostream& operator<<(std::ostream& os, Expected<T> const& e) {
    if (e.has_value()) {
        return os << *e;
    }
    return os << e.error();
}
} // namespace SP

// Device initialization kept minimal: mount providers at the sketch paths without spinning up threads
static inline void initialize_devices(PathSpace& space) {
#if defined(__APPLE__)
    // Use local window to forward events without global permissions
    auto mouse = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Off);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Off);
    auto gamepad = std::make_unique<PathIOGamepad>(PathIOGamepad::BackendMode::Auto);
    PathIOMouse* mousePtr = mouse.get();
    PathIOKeyboard* keyboardPtr = keyboard.get();
    PathIOGamepad* gamepadPtr = gamepad.get();

    space.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    space.insert<"/system/devices/in/text/default">(std::move(keyboard));
    space.insert<"/system/devices/in/gamepad/default">(std::move(gamepad));

    PSInitLocalEventWindow(mousePtr, keyboardPtr);
    PSInitGameControllerInput(gamepadPtr);
#else
    auto mouse = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Auto);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Auto);
    auto gamepad = std::make_unique<PathIOGamepad>(PathIOGamepad::BackendMode::Auto);

    space.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    space.insert<"/system/devices/in/text/default">(std::move(keyboard));
    space.insert<"/system/devices/in/gamepad/default">(std::move(gamepad));
#endif
}

int main() {
    PathSpace space;
    initialize_devices(space);
    // Issue a sample rumble command on the default gamepad (ignored if unsupported)
    {
        SP::PathIOGamepad::HapticsCommand rumble = SP::PathIOGamepad::HapticsCommand::constant(0.25f, 0.5f, 200);
        space.insert<"/system/devices/in/gamepad/default/rumble">(rumble);
    }
    while(true) {
#if defined(__APPLE__)
        SP::PSPollLocalEventWindow();
#endif
        bool printed = false;
        if (auto pe = space.take<"/system/devices/in/pointer/default/events", PointerDeviceEvent>(); pe.has_value()) {
            std::cout << *pe << std::endl;
            printed = true;
        }
        if (auto ke = space.take<"/system/devices/in/text/default/events", TextInputDeviceEvent>(); ke.has_value()) {
            std::cout << *ke << std::endl;
            printed = true;
        }
        if (auto ge = space.take<"/system/devices/in/gamepad/default/events", GamepadEvent>(); ge.has_value()) {
            std::cout << *ge << std::endl;
            printed = true;
        }
        if (!printed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
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
