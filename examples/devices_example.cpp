#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIODeviceDiscovery.hpp>
#include <pathspace/layer/PathIOMice.hpp>
#include <pathspace/layer/PathIOKeyboard.hpp>

// Optional macOS backend skeletons (compile with -DENABLE_PATHIO_MACOS=ON in CMake to define PATHIO_BACKEND_MACOS)
// #include <pathspace/layer/macos/PathIO_macos.hpp>

// If you want this example to simulate plug/unplug and input events without OS backends,
// compile with -DDEVICES_EXAMPLE_SIMULATE=1
#ifndef DEVICES_EXAMPLE_SIMULATE
#define DEVICES_EXAMPLE_SIMULATE 1
#endif

using namespace SP;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};

static void sigint_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Split multi-line string into non-empty lines
static std::vector<std::string> split_lines(std::string const& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// Read a path into std::string from a PathSpace-like object; returns empty optional on error.
template <typename Space>
static std::optional<std::string> read_string(Space& sp, std::string const& path) {
    auto r = sp.read<std::string>(path);
    if (!r.has_value()) return std::nullopt;
    return r.value();
}

// Device discovery poller: produce a class->deviceIds map based on the mounted discovery provider
static std::map<std::string, std::set<int>> read_discovery_snapshot(PathSpace& space, std::string const& mount = "/dev") {
    std::map<std::string, std::set<int>> snapshot;

    auto classesOpt = read_string(space, mount);
    if (!classesOpt) return snapshot;

    for (auto const& cls : split_lines(*classesOpt)) {
        auto idsOpt = read_string(space, mount + "/" + cls);
        if (!idsOpt) continue;
        std::set<int> ids;
        for (auto const& s : split_lines(*idsOpt)) {
            try {
                ids.insert(std::stoi(s));
            } catch (...) {
                // ignore malformed line
            }
        }
        snapshot.emplace(cls, std::move(ids));
    }
    return snapshot;
}

// Print diff between previous and current discovery snapshots, and print metadata for added devices
static void print_discovery_diff(PathSpace& space,
                                 std::map<std::string, std::set<int>> const& prev,
                                 std::map<std::string, std::set<int>> const& curr,
                                 std::string const& mount = "/dev") {
    // For stable iteration
    std::set<std::string> allClasses;
    for (auto const& kv : prev) allClasses.insert(kv.first);
    for (auto const& kv : curr) allClasses.insert(kv.first);

    for (auto const& cls : allClasses) {
        auto pit = prev.find(cls);
        auto cit = curr.find(cls);
        std::set<int> prevIds = (pit == prev.end()) ? std::set<int>{} : pit->second;
        std::set<int> currIds = (cit == curr.end()) ? std::set<int>{} : cit->second;

        // Added
        for (int id : currIds) {
            if (!prevIds.contains(id)) {
                std::cout << "[devices] plug-in: " << cls << "/" << id << "\n";
                // Print meta/capabilities if available
                if (auto meta = read_string(space, mount + "/" + cls + "/" + std::to_string(id) + "/meta")) {
                    std::cout << "  meta:\n";
                    for (auto const& line : split_lines(*meta)) {
                        std::cout << "    " << line << "\n";
                    }
                }
                if (auto caps = read_string(space, mount + "/" + cls + "/" + std::to_string(id) + "/capabilities")) {
                    std::cout << "  capabilities:\n";
                    for (auto const& line : split_lines(*caps)) {
                        std::cout << "    " << line << "\n";
                    }
                }
            }
        }
        // Removed
        for (int id : prevIds) {
            if (!currIds.contains(id)) {
                std::cout << "[devices] unplug: " << cls << "/" << id << "\n";
            }
        }
    }
}

int main() {
    std::signal(SIGINT, sigint_handler);

    auto space = std::make_shared<PathSpace>();

    // Mount discovery provider (simulation-backed)
    auto discovery = std::make_unique<PathIODeviceDiscovery>();
    auto* discRaw = discovery.get();
    {
        auto ret = space->insert<"/dev">(std::move(discovery));
        if (!ret.errors.empty()) {
            std::cerr << "Failed to mount discovery provider\n";
            return 1;
        }
    }

    // Mount a mice and a keyboard provider; providers are path-agnostic and do not care about these paths.
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

    std::cout << "Device example started. Ctrl-C to exit.\n";

#if DEVICES_EXAMPLE_SIMULATE
    // Simulate devices plugging/unplugging and event streams if no OS backend is present.
    std::thread simulator([&] {
        using SimDev = PathIODeviceDiscovery::SimDevice;

        // Plug-in mouse 0
        {
            SimDev m0;
            m0.id = 0; m0.vendor = "Acme"; m0.product = "FastMouse"; m0.connection = "USB";
            m0.capabilities = {"wheel", "buttons:3"};
            discRaw->addSimulatedDevice("mice", m0);
        }
        // Plug-in keyboard 0
        {
            SimDev k0;
            k0.id = 0; k0.vendor = "Acme"; k0.product = "ClickyKeys"; k0.connection = "USB";
            k0.capabilities = {"layout:us", "nkey-rollover"};
            discRaw->addSimulatedDevice("keyboards", k0);
        }

        // Generate a few mouse and keyboard events periodically
        int step = 0;
        while (g_running.load(std::memory_order_acquire) && step < 200) {
            miceRaw->simulateMove(+1, (step % 2 == 0 ? +1 : -1));
            if (step % 40 == 0) {
                kbRaw->simulateKeyDown(/*keycode*/65, /*mod*/Mod_Shift);
            } else if (step % 40 == 5) {
                kbRaw->simulateKeyUp(65, Mod_Shift);
                kbRaw->simulateText("A", Mod_Shift);
            }
            std::this_thread::sleep_for(50ms);
            ++step;
        }

        // Unplug devices
        discRaw->removeSimulatedDevice("mice", 0);
        discRaw->removeSimulatedDevice("keyboards", 0);
    });
#endif

    // Reader threads for events
    std::thread miceReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space->read<"/inputs/mice/0/events", PathIOMice::Event>(Block{250ms});
            if (r.has_value()) {
                auto const& e = *r;
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
            }
        }
    });

    std::thread keyboardReader([&] {
        while (g_running.load(std::memory_order_acquire)) {
            auto r = space->read<"/inputs/keyboards/0/events", PathIOKeyboard::Event>(Block{250ms});
            if (r.has_value()) {
                auto const& e = *r;
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
            }
        }
    });

    // Discovery diff loop
    auto prevSnapshot = read_discovery_snapshot(*space, "/dev");
    while (g_running.load(std::memory_order_acquire)) {
        auto curr = read_discovery_snapshot(*space, "/dev");
        print_discovery_diff(*space, prevSnapshot, curr, "/dev");
        prevSnapshot = std::move(curr);
        std::this_thread::sleep_for(500ms);
    }

    miceReader.join();
    keyboardReader.join();

#if DEVICES_EXAMPLE_SIMULATE
    simulator.join();
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