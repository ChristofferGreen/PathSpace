#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace SP::UI::Declarative {

struct WidgetEventTrellisOptions {
    std::chrono::milliseconds refresh_interval{std::chrono::milliseconds{250}};
    std::chrono::milliseconds idle_sleep{std::chrono::milliseconds{1}};
    std::string windows_root = "/system/widgets/runtime/windows";
    std::string events_root = "/system/widgets/runtime/events";
    std::string metrics_root = "/system/widgets/runtime/events/metrics";
    std::string log_root = "/system/widgets/runtime/events/log";
    std::string state_path = "/system/widgets/runtime/events/state/running";

    using HitTestOverride = std::function<SP::Expected<SP::UI::Runtime::Scene::HitTestResult>(
        PathSpace& space,
        std::string const& scene_path,
        float scene_x,
        float scene_y)>;

    HitTestOverride hit_test_override{};
};

/**
 * Create the widget event trellis worker if it is not already running.
 *
 * @return true when a new worker started, false if an existing worker was reused.
 */
[[nodiscard]] auto CreateWidgetEventTrellis(
    PathSpace& space,
    WidgetEventTrellisOptions const& options = {}) -> SP::Expected<bool>;

/**
 * Stop the widget event trellis worker if one is running.
 */
auto ShutdownWidgetEventTrellis(PathSpace& space) -> void;

} // namespace SP::UI::Declarative
