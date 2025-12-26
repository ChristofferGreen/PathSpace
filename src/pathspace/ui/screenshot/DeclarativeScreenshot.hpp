#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/SceneReadiness.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
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
    std::optional<std::string> view_name;
    std::optional<std::string> theme_override;
    std::string capture_mode = "next_present"; // next_present | frame_index | deadline_ns
    std::optional<std::uint64_t> capture_frame_index;
    std::optional<std::chrono::nanoseconds> capture_deadline;
    bool require_present = false;
    bool force_publish = true;
    bool wait_for_runtime_metrics = true;
    bool mark_dirty_before_publish = true;
    bool force_software = false;
    bool allow_software_fallback = false;
    bool present_when_force_software = false;
    bool enable_capture_framebuffer = true;
    bool present_before_capture = true;
    bool verify_output_matches_framebuffer = true;
    std::optional<double> verify_max_mean_error;
    std::chrono::milliseconds slot_timeout{std::chrono::milliseconds{3000}};
    std::chrono::milliseconds token_timeout{std::chrono::milliseconds{500}};
    std::chrono::milliseconds readiness_timeout{std::chrono::milliseconds{3000}};
    std::chrono::milliseconds publish_timeout{std::chrono::milliseconds{2000}};
    std::chrono::milliseconds present_timeout{std::chrono::milliseconds{2000}};
    SP::UI::Declarative::DeclarativeReadinessOptions readiness_options{};
    SP::UI::Screenshot::BaselineMetadata baseline_metadata;
    std::function<SP::Expected<void>(std::filesystem::path const& output_png,
                                     std::optional<std::filesystem::path> const& baseline_png)> postprocess_png;
};

auto CaptureDeclarative(SP::PathSpace& space,
                        SP::UI::ScenePath const& scene,
                        SP::UI::WindowPath const& window,
                        DeclarativeScreenshotOptions const& options = {})
    -> SP::Expected<ScreenshotResult>;

auto CaptureDeclarativeSimple(SP::PathSpace& space,
                              SP::UI::ScenePath const& scene,
                              SP::UI::WindowPath const& window,
                              std::filesystem::path const& output_png,
                              std::optional<int> width = std::nullopt,
                              std::optional<int> height = std::nullopt)
    -> SP::Expected<ScreenshotResult>;

} // namespace SP::UI::Screenshot
