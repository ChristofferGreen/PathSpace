#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/runtime/TelemetryControl.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

auto wait_for_bool(SP::PathSpace& space,
                   std::string const& path,
                   bool expected,
                   std::chrono::milliseconds timeout = 1000ms) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool saw_value = false;
    bool last_value = false;
    std::optional<SP::Error> last_error;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = space.read<bool, std::string>(path);
        if (value && *value == expected) {
            return true;
        }
        if (value) {
            saw_value = true;
            last_value = *value;
        } else {
            last_error = value.error();
        }
        std::this_thread::sleep_for(5ms);
    }
    if (last_error) {
        if (last_error->message) {
            INFO("wait_for_bool(" << path << ") last_error="
                 << static_cast<int>(last_error->code)
                 << " message=" << *last_error->message);
        } else {
            INFO("wait_for_bool(" << path << ") last_error="
                 << static_cast<int>(last_error->code));
        }
    } else if (saw_value) {
        INFO("wait_for_bool(" << path << ") last_value="
             << (last_value ? "true" : "false"));
    } else {
        INFO("wait_for_bool(" << path << ") saw no values");
    }
    return false;
}

auto wait_for_uint(SP::PathSpace& space,
                   std::string const& path,
                   std::uint32_t expected,
                   std::chrono::milliseconds timeout = 1000ms) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = space.read<std::uint32_t, std::string>(path);
        if (value && *value == expected) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void ensure_device_defaults(SP::PathSpace& space, std::string const& device) {
    space.insert(device + "/config/push/enabled", false);
    space.insert(device + "/config/push/telemetry_enabled", false);
    space.insert(device + "/config/push/rate_limit_hz", 240u);
    space.insert(device + "/config/push/max_queue", 256u);
}

} // namespace

TEST_SUITE("runtime.telemetrycontrol") {
TEST_CASE("TelemetryControl toggles telemetry state via command queues") {
    SP::PathSpace space;
    SP::Runtime::TelemetryControlOptions options;
    options.block_timeout = 1ms;
    options.idle_sleep = 1ms;

    auto drain_logs = [&](char const* label) {
        while (true) {
            auto log_entry = space.take<std::string, std::string>(options.log_path);
            if (!log_entry) {
                break;
            }
            INFO(label << ": " << *log_entry);
        }
    };

    auto started = SP::Runtime::CreateTelemetryControl(space, options);
    if (!started) {
        FAIL_CHECK(started.error().message.value_or("CreateTelemetryControl failed"));
        return;
    }
    REQUIRE(*started);
    CHECK(wait_for_bool(space, options.state_path, true));

    SP::Runtime::TelemetryToggleCommand enable_cmd{true};
    auto enable_insert = space.insert(options.telemetry_start_queue, enable_cmd);
    REQUIRE(enable_insert.errors.empty());
    drain_logs("after-enable-insert");
    CHECK(wait_for_bool(space, options.telemetry_toggle_path, true));

    SP::Runtime::TelemetryToggleCommand disable_cmd{false};
    auto disable_insert = space.insert(options.telemetry_stop_queue, disable_cmd);
    REQUIRE(disable_insert.errors.empty());
    drain_logs("after-disable-insert");
    CHECK(wait_for_bool(space, options.telemetry_toggle_path, false));

    SP::Runtime::ShutdownTelemetryControl(space);
}

TEST_CASE("TelemetryControl applies subscriber commands") {
    SP::PathSpace space;
    SP::Runtime::TelemetryControlOptions options;
    options.block_timeout = 1ms;
    options.idle_sleep = 1ms;

    auto drain_logs = [&](char const* label) {
        while (true) {
            auto log_entry = space.take<std::string, std::string>(options.log_path);
            if (!log_entry) {
                break;
            }
            INFO(label << ": " << *log_entry);
        }
    };

    auto device = std::string{"/system/devices/in/pointer/default"};
    ensure_device_defaults(space, device);

    auto started = SP::Runtime::CreateTelemetryControl(space, options);
    if (!started) {
        FAIL_CHECK(started.error().message.value_or("CreateTelemetryControl failed"));
        return;
    }
    REQUIRE(*started);

    SP::Runtime::DevicePushCommand command;
    command.device = device;
    command.subscriber = "telemetry_test";
    command.enable = true;
    command.set_telemetry = true;
    command.telemetry_enabled = true;
    auto command_insert = space.insert(options.push_command_queue, command);
    REQUIRE(command_insert.errors.empty());
    drain_logs("after-subscribe-command");

    CHECK(wait_for_bool(space, device + "/config/push/enabled", true));
    if (auto subscriber_value = space.read<bool, std::string>(device + "/config/push/subscribers/telemetry_test"); subscriber_value) {
        INFO("subscriber value before wait: " << (*subscriber_value ? "true" : "false"));
    } else {
        INFO("subscriber read error: "
             << subscriber_value.error().message.value_or("unknown"));
    }
    CHECK(wait_for_bool(space, device + "/config/push/subscribers/telemetry_test", true));
    drain_logs("final-subscriber");
    CHECK(wait_for_bool(space, device + "/config/push/telemetry_enabled", true));

    SP::Runtime::ShutdownTelemetryControl(space);
}

TEST_CASE("TelemetryControl throttles multiple devices") {
    SP::PathSpace space;
    SP::Runtime::TelemetryControlOptions options;
    options.block_timeout = 1ms;
    options.idle_sleep = 1ms;

    auto pointer_device = std::string{"/system/devices/in/pointer/default"};
    auto keyboard_device = std::string{"/system/devices/in/keyboard/builtin"};
    ensure_device_defaults(space, pointer_device);
    ensure_device_defaults(space, keyboard_device);

    auto started = SP::Runtime::CreateTelemetryControl(space, options);
    if (!started) {
        FAIL_CHECK(started.error().message.value_or("CreateTelemetryControl failed"));
        return;
    }
    REQUIRE(*started);

    SP::Runtime::DeviceThrottleCommand throttle;
    throttle.device = "*";
    throttle.set_rate_limit = true;
    throttle.rate_limit_hz = 480;
    throttle.set_max_queue = true;
    throttle.max_queue = 32;
    space.insert(options.throttle_command_queue, throttle);

    CHECK(wait_for_uint(space, pointer_device + "/config/push/rate_limit_hz", 480u));
    CHECK(wait_for_uint(space, keyboard_device + "/config/push/rate_limit_hz", 480u));
    CHECK(wait_for_uint(space, pointer_device + "/config/push/max_queue", 32u));
    CHECK(wait_for_uint(space, keyboard_device + "/config/push/max_queue", 32u));

    SP::Runtime::ShutdownTelemetryControl(space);
}
}
