#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace SP::UI::Screenshot {

struct FramebufferView {
    std::span<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
};

struct OverlayRegion {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct OverlayImageView {
    int width = 0;
    int height = 0;
    std::span<const std::uint8_t> pixels;
};

struct BaselineMetadata {
    std::optional<int> manifest_revision;
    std::optional<std::string> tag;
    std::optional<std::string> sha256;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> renderer;
    std::optional<std::string> captured_at;
    std::optional<std::string> commit;
    std::optional<std::string> notes;
    std::optional<double> tolerance;
};

struct ScreenshotRequest {
    SP::PathSpace& space;
    SP::UI::WindowPath window_path;
    std::string view_name;
    int width = 0;
    int height = 0;
    std::filesystem::path output_png;
    std::optional<std::filesystem::path> baseline_png;
    std::optional<std::filesystem::path> diff_png;
    std::optional<std::filesystem::path> metrics_json;
    double max_mean_error = 0.0015;
    bool require_present = false;
    std::chrono::milliseconds present_timeout{1500};
    BaselineMetadata baseline_metadata;
    bool force_software = false;
    bool allow_software_fallback = false;
    bool present_when_force_software = false;
    std::span<std::uint8_t> provided_framebuffer;
    bool provided_framebuffer_is_hardware = false;
    bool verify_output_matches_framebuffer = false;
    std::optional<double> verify_max_mean_error;
    std::optional<std::string> theme_override;
    std::function<SP::Expected<void>(std::filesystem::path const& output_png,
                                     std::optional<std::filesystem::path> const& baseline_png)> postprocess_png;
};

struct ScreenshotResult {
    bool hardware_capture = false;
    bool matched_baseline = false;
    std::optional<double> mean_error;
    std::optional<std::uint32_t> max_channel_delta;
    std::filesystem::path artifact;
    std::optional<std::filesystem::path> diff_artifact;
    std::string status;
};

auto OverlayRegionOnPng(std::filesystem::path const& screenshot_path,
                        OverlayImageView const& overlay,
                        OverlayRegion region) -> SP::Expected<void>;

class ScreenshotService {
public:
    static auto Capture(ScreenshotRequest const& request) -> SP::Expected<ScreenshotResult>;
};

} // namespace SP::UI::Screenshot
