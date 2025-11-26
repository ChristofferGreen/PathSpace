#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include <chrono>
#include <string_view>

namespace SP::UI::Declarative {

struct InputTaskOptions {
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{4}};
    std::size_t max_actions_per_widget = 64;
    std::chrono::milliseconds slow_handler_threshold{std::chrono::milliseconds{5}};
};

struct ManualPumpOptions {
    std::size_t max_actions_per_widget = 64;
    std::chrono::milliseconds slow_handler_threshold{std::chrono::milliseconds{5}};
    bool include_app_widgets = true;
    bool publish_window_metrics = true;
};

struct ManualPumpResult {
    std::size_t widgets_processed = 0;
    std::size_t actions_published = 0;
};

/**
 * Create the declarative input task for the provided PathSpace if it is not already running.
 *
 * @return true when a new worker was started, false if one was already running.
 */
auto CreateInputTask(PathSpace& space,
                     InputTaskOptions const& options = {}) -> SP::Expected<bool>;

/**
 * Stop the declarative input task for the provided PathSpace (no-op when not running).
 */
auto ShutdownInputTask(PathSpace& space) -> void;

/**
 * Synchronously pump the declarative widget queues for a specific window/view.
 *
 * Returns aggregated widget/action counts so tests can assert progress without
 * waiting for the background worker. When publish_window_metrics=true the
 * helper also records per-window/app counters under
 * `/system/widgets/runtime/input/{windows,apps}/.../metrics/\*`.
 */
auto PumpWindowWidgetsOnce(PathSpace& space,
                           SP::UI::Builders::WindowPath const& window,
                           std::string_view view_name,
                           ManualPumpOptions const& options = {}) -> SP::Expected<ManualPumpResult>;

} // namespace SP::UI::Declarative
