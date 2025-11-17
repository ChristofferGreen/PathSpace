#pragma once

#include <pathspace/PathSpace.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace SP::Runtime {

struct TelemetryToggleCommand {
    bool enable = true;
};

struct DevicePushCommand {
    std::string device;         // Absolute path or pattern ("*" / prefix*)
    std::string subscriber = "io_trellis";
    bool enable = true;
    bool touch_push_enabled = true;
    bool set_telemetry = false;
    bool telemetry_enabled = false;
};

struct DeviceThrottleCommand {
    std::string device;                // Absolute path, prefix*, or "*"
    std::uint32_t rate_limit_hz = 0;   // Applies when set_rate_limit == true
    bool set_rate_limit = false;
    std::uint32_t max_queue = 0;       // Applies when set_max_queue == true
    bool set_max_queue = false;
};

struct TelemetryControlOptions {
    std::string telemetry_toggle_path = "/_system/telemetry/io/events_enabled";
    std::string telemetry_start_queue = "/_system/telemetry/start/queue";
    std::string telemetry_stop_queue = "/_system/telemetry/stop/queue";
    std::string push_command_queue = "/_system/io/push/subscriptions/queue";
    std::string throttle_command_queue = "/_system/io/push/throttle/queue";
    std::string log_path = "/_system/telemetry/log/errors/queue";
    std::string devices_root = "/system/devices/in";
    std::string state_path = "/_system/telemetry/io/state/running";
    std::chrono::milliseconds idle_sleep{std::chrono::milliseconds{5}};
    std::chrono::milliseconds block_timeout{std::chrono::milliseconds{25}};
};

[[nodiscard]] auto CreateTelemetryControl(PathSpace& space,
                                          TelemetryControlOptions const& options = {})
    -> SP::Expected<bool>;

auto ShutdownTelemetryControl(PathSpace& space) -> void;

} // namespace SP::Runtime
