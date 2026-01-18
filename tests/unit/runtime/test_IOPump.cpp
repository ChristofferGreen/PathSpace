#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/runtime/IOPump.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

auto write_vector(SP::PathSpace& space,
                  std::string const& path,
                  std::vector<std::string> const& values) -> void {
    auto existing = space.take<std::vector<std::string>, std::string>(path);
    while (existing) {
        existing = space.take<std::vector<std::string>, std::string>(path);
    }
    space.insert(path, values);
}

auto write_string(SP::PathSpace& space,
                  std::string const& path,
                  std::string const& value) -> void {
    auto existing = space.take<std::string, std::string>(path);
    while (existing) {
        existing = space.take<std::string, std::string>(path);
    }
    space.insert(path, value);
}

void ensure_window_entry(SP::PathSpace& space,
                         std::string const& window_path,
                         std::vector<std::string> pointer_devices,
                         std::vector<std::string> button_devices = {},
                         std::vector<std::string> text_devices = {}) {
    auto token = SP::Runtime::MakeRuntimeWindowToken(window_path);
    auto base = std::string{"/system/widgets/runtime/windows/"} + token;
    write_string(space, base + "/window", window_path);
    write_vector(space, base + "/subscriptions/pointer/devices", pointer_devices);
    write_vector(space, base + "/subscriptions/button/devices", button_devices);
    write_vector(space, base + "/subscriptions/text/devices", text_devices);
}

} // namespace

TEST_SUITE("runtime.iopump") {
TEST_CASE("IOPump routes subscribed pointer events") {
    SP::PathSpace space;
    auto window_path = std::string{"/system/applications/demo/windows/main"};
    ensure_window_entry(space,
                        window_path,
                        {"/system/devices/in/pointer/default"});

    SP::Runtime::IoPumpOptions options;
    options.block_timeout = 1ms;
    options.idle_sleep = 1ms;
    options.subscription_refresh = 10ms;

    auto started = SP::Runtime::CreateIOPump(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    SP::IO::PointerEvent event{};
    event.device_path = "/system/devices/in/pointer/default";
    event.motion.delta_x = 2.5f;
    space.insert(std::string{SP::IO::IoEventPaths::kPointerQueue}, event);

    auto queue_path = std::string{"/system/widgets/runtime/events/"}
        + SP::Runtime::MakeRuntimeWindowToken(window_path)
        + "/pointer/queue";

    auto routed = space.take<SP::IO::PointerEvent, std::string>(
        queue_path,
        SP::Out{} & SP::Block{50ms});
    CHECK(routed);
    CHECK(routed->device_path == event.device_path);
    CHECK(routed->motion.delta_x == doctest::Approx(2.5f));

    SP::Runtime::ShutdownIOPump(space);
}

TEST_CASE("IOPump falls back to global queues when no subscription exists") {
    SP::PathSpace space;
    auto window_path = std::string{"/system/applications/demo/windows/secondary"};
    ensure_window_entry(space, window_path, {});

    SP::Runtime::IoPumpOptions options;
    options.block_timeout = 1ms;
    options.idle_sleep = 1ms;
    options.subscription_refresh = 10ms;

    auto started = SP::Runtime::CreateIOPump(space, options);
    REQUIRE(started);

    SP::IO::PointerEvent event{};
    event.device_path = "/system/devices/in/pointer/unmatched";
    event.motion.delta_y = -4.0f;
    space.insert(std::string{SP::IO::IoEventPaths::kPointerQueue}, event);

    auto queue_path = std::string{"/system/widgets/runtime/events/global/pointer/queue"};
    auto routed = space.take<SP::IO::PointerEvent, std::string>(
        queue_path,
        SP::Out{} & SP::Block{50ms});
    CHECK(routed);
    CHECK(routed->device_path == event.device_path);
    CHECK(routed->motion.delta_y == doctest::Approx(-4.0f));

    SP::Runtime::ShutdownIOPump(space);
}
}
