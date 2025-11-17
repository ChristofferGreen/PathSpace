#pragma once

#include <pathspace/PathSpace.hpp>

#include <chrono>

namespace SP::UI::Declarative {

struct InputTaskOptions {
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{4}};
    std::size_t max_actions_per_widget = 64;
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

} // namespace SP::UI::Declarative
