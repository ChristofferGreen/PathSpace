#pragma once

#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace PathSpaceExamples {

struct CommandLineOptions {
    int width = 1280;
    int height = 800;
    bool headless = false;
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> screenshot_compare_path;
    std::optional<std::filesystem::path> screenshot_diff_path;
    std::optional<std::filesystem::path> screenshot_metrics_path;
    double screenshot_max_mean_error = 0.0015;
    bool screenshot_require_present = false;
    bool screenshot_force_software = false;
    std::optional<std::filesystem::path> export_html_dir;
    bool gpu_smoke = false;
    std::optional<std::filesystem::path> gpu_texture_path;
    SP::UI::Screenshot::BaselineMetadata baseline_metadata;
    bool serve_html = false;
    int serve_html_port = 8080;
    std::string serve_html_host{"127.0.0.1"};
    std::string serve_html_view{"web"};
    std::string serve_html_target{"paint_web"};
    std::string serve_html_user{"demo"};
    std::string serve_html_password{"demo"};
    bool serve_html_allow_unauthenticated = false;
};

auto RunPaintExample(CommandLineOptions options) -> int;
auto ParsePaintExampleCommandLine(int argc, char** argv) -> CommandLineOptions;

} // namespace PathSpaceExamples
