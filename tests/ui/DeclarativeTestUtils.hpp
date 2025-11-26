#pragma once

#include <pathspace/core/Error.hpp>
#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>

#include "../../examples/declarative_example_shared.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace DeclarativeTestUtils {

inline auto read_env_timeout_override() -> std::optional<std::chrono::milliseconds> {
    if (const char* env_ms = std::getenv("PATHSPACE_TEST_TIMEOUT_MS")) {
        char* endptr = nullptr;
        long value = std::strtol(env_ms, &endptr, 10);
        if (endptr != env_ms && value > 0) {
            return std::chrono::milliseconds{value};
        }
    }
    if (const char* env = std::getenv("PATHSPACE_TEST_TIMEOUT")) {
        char* endptr = nullptr;
        long value = std::strtol(env, &endptr, 10);
        if (endptr != env && value > 0) {
            return std::chrono::milliseconds{value * 1000};
        }
    }
    return std::nullopt;
}

inline auto scaled_timeout(std::chrono::milliseconds fallback,
                           double scale = 1.0,
                           std::chrono::milliseconds max_timeout = std::chrono::milliseconds{20000})
    -> std::chrono::milliseconds {
    auto base = fallback;
    if (auto env_override = read_env_timeout_override()) {
        base = *env_override;
    }
    auto scaled = static_cast<long long>(std::llround(static_cast<double>(base.count()) * scale));
    if (scaled < fallback.count()) {
        scaled = fallback.count();
    }
    if (scaled > max_timeout.count()) {
        scaled = max_timeout.count();
    }
    return std::chrono::milliseconds{scaled};
}

inline auto full_fuzz_enabled() -> bool {
    return std::getenv("PATHSPACE_FULL_FUZZ") != nullptr;
}

inline int scaled_iterations(int default_iterations,
                             int min_iterations = 1,
                             double scale = 0.5) {
    if (full_fuzz_enabled()) {
        return default_iterations;
    }
    auto timeout_override = read_env_timeout_override();
    if (!timeout_override) {
        return default_iterations;
    }
    if (timeout_override->count() <= 1500) {
        auto scaled = static_cast<int>(std::llround(static_cast<double>(default_iterations) * scale));
        if (scaled < min_iterations) {
            scaled = min_iterations;
        }
        return scaled;
    }
    return default_iterations;
}

inline auto read_metric(SP::PathSpace& space, std::string const& metric_path)
    -> SP::Expected<std::uint64_t> {
    auto value = space.read<std::uint64_t, std::string>(metric_path);
    if (value) {
        return value;
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoSuchPath
        || error.code == SP::Error::Code::NoObjectFound) {
        return std::uint64_t{0};
    }
    return std::unexpected(error);
}

inline auto wait_for_metric_condition(SP::PathSpace& space,
                                      std::string const& metric_path,
                                      std::chrono::milliseconds timeout,
                                      std::function<bool(std::uint64_t)> predicate)
    -> SP::Expected<void> {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto current = read_metric(space, metric_path);
        if (!current) {
            return std::unexpected(current.error());
        }
        if (predicate(*current)) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     std::string{"metric "} + metric_path + " did not satisfy predicate"});
}

inline auto wait_for_metric_at_least(SP::PathSpace& space,
                                     std::string const& metric_path,
                                     std::uint64_t target,
                                     std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return wait_for_metric_condition(
        space,
        metric_path,
        timeout,
        [target](std::uint64_t value) { return value >= target; });
}

inline auto wait_for_metric_equal(SP::PathSpace& space,
                                  std::string const& metric_path,
                                  std::uint64_t target,
                                  std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return wait_for_metric_condition(
        space,
        metric_path,
        timeout,
        [target](std::uint64_t value) { return value == target; });
}

template <typename Value>
inline auto take_with_retry(SP::PathSpace& space,
                            std::string const& path,
                            std::chrono::milliseconds per_attempt,
                            std::chrono::milliseconds total_timeout) -> SP::Expected<Value> {
    auto const deadline = std::chrono::steady_clock::now() + total_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = space.take<Value>(path, SP::Out{} & SP::Block{per_attempt});
        if (result) {
            return result;
        }
        auto const& error = result.error();
        if (error.code != SP::Error::Code::Timeout) {
            return std::unexpected(error);
        }
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "take_with_retry exceeded timeout"});
}

constexpr std::string_view kInputWidgetsProcessedMetric =
    "/system/widgets/runtime/input/metrics/widgets_processed_total";
constexpr std::string_view kWidgetEventsPointerMetric =
    "/system/widgets/runtime/events/metrics/pointer_events_total";
constexpr std::string_view kWidgetEventsButtonMetric =
    "/system/widgets/runtime/events/metrics/button_events_total";
constexpr std::string_view kWidgetEventsOpsMetric =
    "/system/widgets/runtime/events/metrics/widget_ops_total";

inline auto format_error(std::string_view context, SP::Error const& error) -> std::string {
    std::string message{context};
    message.append(" code=");
    message.append(std::to_string(static_cast<int>(error.code)));
    message.append(" message=");
    if (error.message) {
        message.append(*error.message);
    } else {
        message.append("<none>");
    }
    return message;
}

inline auto ensure_scene_ready(SP::PathSpace& space,
                               SP::UI::Builders::ScenePath const& scene,
                               SP::UI::Builders::WindowPath const& window,
                               std::string const& view_name,
                               PathSpaceExamples::DeclarativeReadinessOptions options)
    -> SP::Expected<PathSpaceExamples::DeclarativeReadinessResult> {
    options.widget_timeout = scaled_timeout(options.widget_timeout, 1.0);
    options.revision_timeout = scaled_timeout(options.revision_timeout, 1.0);
    options.runtime_metrics_timeout = scaled_timeout(options.runtime_metrics_timeout, 1.0);
    return PathSpaceExamples::ensure_declarative_scene_ready(space, scene, window, view_name, options);
}

} // namespace DeclarativeTestUtils
