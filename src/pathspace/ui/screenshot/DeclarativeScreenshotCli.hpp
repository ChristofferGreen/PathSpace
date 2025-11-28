#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace SP::UI::Screenshot {

struct DeclarativeScreenshotCliOptions {
    std::optional<std::filesystem::path> output_png;
    std::optional<std::filesystem::path> baseline_png;
    std::optional<std::filesystem::path> diff_png;
    std::optional<std::filesystem::path> metrics_json;
    std::optional<std::string> telemetry_root;
    double max_mean_error = 0.0015;
    bool require_present = false;
    bool force_software = false;
    bool allow_software_fallback = false;
    bool wait_for_runtime_metrics = true;
    bool mark_dirty_before_publish = true;
    SP::UI::Screenshot::BaselineMetadata baseline_metadata;
};

void RegisterDeclarativeScreenshotCliOptions(SP::Examples::CLI::ExampleCli& cli,
                                             DeclarativeScreenshotCliOptions& options);

void ApplyDeclarativeScreenshotEnvOverrides(DeclarativeScreenshotCliOptions& options);

[[nodiscard]] bool DeclarativeScreenshotRequested(DeclarativeScreenshotCliOptions const& options);

auto CaptureDeclarativeScreenshotIfRequested(
    SP::PathSpace& space,
    SP::UI::ScenePath const& scene,
    SP::UI::WindowPath const& window,
    std::string_view view_name,
    int width,
    int height,
    std::string_view telemetry_namespace,
    DeclarativeScreenshotCliOptions const& options,
    std::function<SP::Expected<void>()> pose = {},
    std::function<void(SP::UI::Screenshot::DeclarativeScreenshotOptions&)> configure = {})
    -> SP::Expected<void>;

} // namespace SP::UI::Screenshot

