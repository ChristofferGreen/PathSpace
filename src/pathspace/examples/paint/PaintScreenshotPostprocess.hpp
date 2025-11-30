#pragma once

#include <pathspace/core/Error.hpp>
#include <pathspace/examples/paint/PaintControls.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace SP::Examples::PaintScreenshot {

namespace PaintControlsNS = SP::Examples::PaintControls;

struct SoftwareImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

auto ReadImagePng(std::filesystem::path const& input_path) -> SP::Expected<SoftwareImage>;

auto WriteImagePng(SoftwareImage const& image, std::filesystem::path const& output_path) -> SP::Expected<void>;

auto OverlayStrokesOntoPng(std::filesystem::path const& screenshot_path,
                           SoftwareImage const& strokes,
                           PaintControlsNS::PaintLayoutMetrics const& layout) -> SP::Expected<void>;

auto ApplyControlsBackgroundOverlay(std::filesystem::path const& screenshot_path,
                                    PaintControlsNS::PaintLayoutMetrics const& layout,
                                    int screenshot_width,
                                    int screenshot_height,
                                    std::optional<std::filesystem::path> const& baseline_png) -> SP::Expected<void>;

auto ApplyControlsShadowOverlay(std::filesystem::path const& screenshot_path,
                                PaintControlsNS::PaintLayoutMetrics const& layout,
                                int screenshot_width,
                                int screenshot_height) -> SP::Expected<void>;

auto MakePostprocessHook(PaintControlsNS::PaintLayoutMetrics layout,
                         int screenshot_width,
                         int screenshot_height,
                         std::shared_ptr<SoftwareImage const> strokes = {})
    -> std::function<SP::Expected<void>(std::filesystem::path const& output_png,
                                        std::optional<std::filesystem::path> const& baseline_png)>;

} // namespace SP::Examples::PaintScreenshot
