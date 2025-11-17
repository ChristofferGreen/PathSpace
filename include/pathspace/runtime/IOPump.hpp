#pragma once

#include <pathspace/PathSpace.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace SP::Runtime {

struct IoPumpOptions {
    std::chrono::microseconds block_timeout{std::chrono::microseconds{200}};
    std::chrono::milliseconds idle_sleep{std::chrono::milliseconds{1}};
    std::chrono::milliseconds subscription_refresh{std::chrono::milliseconds{250}};
    std::string windows_root = "/system/widgets/runtime/windows";
    std::string events_root = "/system/widgets/runtime/events";
    std::string metrics_root = "/system/widgets/runtime/input/metrics";
    std::string state_path = "/system/widgets/runtime/io/state/running";
    bool fanout_unmatched_to_global = true;
    std::shared_ptr<std::atomic<bool>> stop_flag{};
};

[[nodiscard]] auto CreateIOPump(PathSpace& space,
                                IoPumpOptions const& options = {}) -> SP::Expected<bool>;

auto ShutdownIOPump(PathSpace& space) -> void;

[[nodiscard]] auto MakeRuntimeWindowToken(std::string_view window_path) -> std::string;

} // namespace SP::Runtime
