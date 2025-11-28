#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/SceneReadiness.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace SP::UI::Screenshot {

struct DeclarativeScreenshotOptions {
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::filesystem::path> output_png;
    std::optional<std::filesystem::path> baseline_png;
    std::optional<std::filesystem::path> diff_png;
    std::optional<std::filesystem::path> metrics_json;
    std::optional<double> max_mean_error;
    std::optional<std::string> telemetry_namespace;
    std::optional<std::string> telemetry_root;
    std::optional<std::string> view_name;
    bool require_present = false;
    bool force_publish = true;
    bool wait_for_runtime_metrics = true;
    bool mark_dirty_before_publish = true;
    bool force_software = false;
    bool allow_software_fallback = false;
    bool present_when_force_software = false;
    std::chrono::milliseconds readiness_timeout{std::chrono::milliseconds{3000}};
    std::chrono::milliseconds publish_timeout{std::chrono::milliseconds{2000}};
    std::chrono::milliseconds present_timeout{std::chrono::milliseconds{2000}};
    SP::UI::Declarative::DeclarativeReadinessOptions readiness_options{};
    SP::UI::Screenshot::BaselineMetadata baseline_metadata;
    std::optional<Hooks> hooks;
};

auto CaptureDeclarative(SP::PathSpace& space,
                        SP::UI::ScenePath const& scene,
                        SP::UI::WindowPath const& window,
                        DeclarativeScreenshotOptions const& options = {})
    -> SP::Expected<ScreenshotResult>;

} // namespace SP::UI::Screenshot
