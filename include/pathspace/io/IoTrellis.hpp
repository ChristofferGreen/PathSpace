#pragma once

#include <pathspace/PathSpace.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace SP::IO {

struct IoTrellisOptions {
    std::string subscriber_name = "io_trellis";
    std::chrono::milliseconds event_wait_timeout{std::chrono::milliseconds{2}};
    std::chrono::milliseconds idle_sleep{std::chrono::milliseconds{2}};
    std::chrono::milliseconds discovery_interval{std::chrono::milliseconds{1000}};
    std::chrono::milliseconds telemetry_publish_interval{std::chrono::milliseconds{200}};
    std::chrono::milliseconds telemetry_poll_interval{std::chrono::milliseconds{250}};
    std::string telemetry_toggle_path = "/_system/telemetry/io/events_enabled";
    std::string metrics_root = "/system/io/events/metrics";
    bool enable_pointer = true;
    bool enable_keyboard = true;
    bool enable_gamepad = true;
};

struct IoTrellisImpl;

class IoTrellisHandle {
public:
    IoTrellisHandle() = default;
    ~IoTrellisHandle();

    IoTrellisHandle(IoTrellisHandle const&) = delete;
    auto operator=(IoTrellisHandle const&) -> IoTrellisHandle& = delete;

    IoTrellisHandle(IoTrellisHandle&&) noexcept;
    auto operator=(IoTrellisHandle&&) noexcept -> IoTrellisHandle&;

    void shutdown();
    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

private:
    explicit IoTrellisHandle(std::shared_ptr<IoTrellisImpl> impl);

    std::shared_ptr<IoTrellisImpl> impl_;

    friend auto CreateIOTrellis(PathSpace&,
                                IoTrellisOptions const&) -> SP::Expected<IoTrellisHandle>;
};

auto CreateIOTrellis(PathSpace& space,
                     IoTrellisOptions const& options = {}) -> SP::Expected<IoTrellisHandle>;

} // namespace SP::IO
