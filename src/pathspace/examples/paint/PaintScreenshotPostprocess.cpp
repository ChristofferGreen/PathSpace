#include <pathspace/examples/paint/PaintScreenshotPostprocess.hpp>

#include <pathspace/core/Error.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <third_party/stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_STATIC

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <third_party/stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_STATIC

namespace SP::Examples::PaintScreenshot {

namespace {

auto make_runtime_error(std::string_view message) -> SP::Error {
    return SP::Error{SP::Error::Code::UnknownError, std::string(message)};
}

auto clamp_int(int value, int min_value, int max_value) -> int {
    return std::max(min_value, std::min(value, max_value));
}

auto float_color_to_bytes(std::array<float, 4> const& color) -> std::array<std::uint8_t, 4> {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < color.size(); ++i) {
        auto clamped = std::clamp(color[i], 0.0f, 1.0f);
        bytes[i] = static_cast<std::uint8_t>(std::round(clamped * 255.0f));
    }
    return bytes;
}

} // namespace

auto ReadImagePng(std::filesystem::path const& input_path) -> SP::Expected<SoftwareImage> {
    std::filesystem::path absolute = std::filesystem::absolute(input_path);
    auto file = std::fopen(absolute.string().c_str(), "rb");
    if (!file) {
        return std::unexpected(make_runtime_error("failed to open PNG"));
    }
    std::fseek(file, 0, SEEK_END);
    auto size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    if (std::fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
        std::fclose(file);
        return std::unexpected(make_runtime_error("failed to read PNG"));
    }
    std::fclose(file);

    int width = 0;
    int height = 0;
    int components = 0;
    auto* data = stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &width, &height, &components, 4);
    if (data == nullptr) {
        return std::unexpected(make_runtime_error("failed to decode PNG"));
    }
    SoftwareImage image{};
    image.width = width;
    image.height = height;
    auto total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    image.pixels.assign(data, data + total);
    stbi_image_free(data);
    return image;
}

auto WriteImagePng(SoftwareImage const& image, std::filesystem::path const& output_path) -> SP::Expected<void> {
    if (image.width <= 0 || image.height <= 0) {
        return std::unexpected(make_runtime_error("invalid screenshot dimensions"));
    }
    auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(make_runtime_error("failed to create output directory"));
        }
    }
    auto row_bytes = static_cast<std::size_t>(image.width) * 4u;
    if (image.pixels.size() != row_bytes * static_cast<std::size_t>(image.height)) {
        return std::unexpected(make_runtime_error("pixel buffer length mismatch"));
    }
    if (stbi_write_png(output_path.string().c_str(),
                       image.width,
                       image.height,
                       4,
                       image.pixels.data(),
                       static_cast<int>(row_bytes)) == 0) {
        return std::unexpected(make_runtime_error("failed to encode PNG"));
    }
    return {};
}

auto OverlayStrokesOntoPng(std::filesystem::path const& screenshot_path,
                           SoftwareImage const& strokes,
                           PaintControlsNS::PaintLayoutMetrics const& layout) -> SP::Expected<void> {
    if (strokes.width <= 0 || strokes.height <= 0) {
        return std::unexpected(make_runtime_error("scripted strokes missing dimensions"));
    }
    auto expected_bytes = static_cast<std::size_t>(strokes.width)
                          * static_cast<std::size_t>(strokes.height) * 4u;
    if (strokes.pixels.size() != expected_bytes) {
        return std::unexpected(make_runtime_error("scripted strokes pixel buffer length mismatch"));
    }
    auto overlay_view = SP::UI::Screenshot::OverlayImageView{
        .width = strokes.width,
        .height = strokes.height,
        .pixels = std::span<const std::uint8_t>(strokes.pixels.data(), strokes.pixels.size()),
    };
    auto canvas_region = SP::UI::Screenshot::OverlayRegion{
        .left = static_cast<int>(std::round(layout.canvas_offset_x)),
        .top = static_cast<int>(std::round(layout.canvas_offset_y)),
        .right = static_cast<int>(std::round(layout.canvas_offset_x + layout.canvas_width)),
        .bottom = static_cast<int>(std::round(layout.canvas_offset_y + layout.canvas_height)),
    };
    auto status = SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, overlay_view, canvas_region);
    if (!status) {
        return status;
    }
    return {};
}

auto ApplyControlsBackgroundOverlay(std::filesystem::path const& screenshot_path,
                                    PaintControlsNS::PaintLayoutMetrics const& layout,
                                    int screenshot_width,
                                    int screenshot_height,
                                    std::optional<std::filesystem::path> const& baseline_png)
    -> SP::Expected<void> {
    auto image = ReadImagePng(screenshot_path);
    if (!image) {
        return std::unexpected(image.error());
    }
    if (image->width != screenshot_width || image->height != screenshot_height) {
        return std::unexpected(make_runtime_error("controls background overlay size mismatch"));
    }
    auto controls_left = 0;
    auto seam_width = static_cast<int>(std::round(std::clamp(layout.controls_spacing * 0.55f, 10.0f, 22.0f)));
    auto controls_extent = layout.padding_x + layout.controls_width + static_cast<float>(seam_width);
    auto controls_right = std::min(screenshot_width, static_cast<int>(std::ceil(controls_extent)) + 6);
    auto controls_top = 0;
    auto controls_bottom = screenshot_height;
    if (controls_left >= controls_right || controls_top >= controls_bottom) {
        return {};
    }
    std::optional<SoftwareImage> baseline_overlay;
    if (baseline_png && std::filesystem::exists(*baseline_png)) {
        auto baseline = ReadImagePng(*baseline_png);
        if (baseline && baseline->width == screenshot_width && baseline->height == screenshot_height) {
            baseline_overlay = std::move(*baseline);
        }
    }
    auto row_bytes = static_cast<std::size_t>(image->width) * 4u;
    auto canvas_left = clamp_int(static_cast<int>(std::round(layout.canvas_offset_x)), 0, screenshot_width);
    auto canvas_right = clamp_int(static_cast<int>(std::round(layout.canvas_offset_x + layout.canvas_width)),
                                  0,
                                  screenshot_width);
    auto canvas_top = clamp_int(static_cast<int>(std::round(layout.padding_y)), 0, screenshot_height);
    auto canvas_bottom = clamp_int(static_cast<int>(std::round(layout.padding_y + layout.canvas_height)),
                                   0,
                                   screenshot_height);

    if (baseline_overlay) {
        auto copy_region = [&](int left, int top, int right, int bottom) {
            left = clamp_int(left, 0, image->width);
            right = clamp_int(right, 0, image->width);
            top = clamp_int(top, 0, image->height);
            bottom = clamp_int(bottom, 0, image->height);
            if (left >= right || top >= bottom) {
                return;
            }
            for (int y = top; y < bottom; ++y) {
                auto dst = image->pixels.data()
                           + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
                auto src = baseline_overlay->pixels.data()
                           + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(left) * 4u;
                std::memcpy(dst, src, static_cast<std::size_t>(right - left) * 4u);
            }
        };
        copy_region(0, 0, image->width, canvas_top);
        copy_region(0, canvas_bottom, image->width, image->height);
        copy_region(0, canvas_top, canvas_left, canvas_bottom);
        copy_region(canvas_right, canvas_top, image->width, canvas_bottom);
    } else {
        std::array<std::uint8_t, 4> fill_color{202u, 209u, 226u, 255u};
        for (int y = controls_top; y < controls_bottom; ++y) {
            auto row_offset = static_cast<std::size_t>(y) * row_bytes;
            for (int x = controls_left; x < controls_right; ++x) {
                auto idx = row_offset + static_cast<std::size_t>(x) * 4u;
                if (image->pixels[idx + 3] == 0) {
                    image->pixels[idx + 0] = fill_color[0];
                    image->pixels[idx + 1] = fill_color[1];
                    image->pixels[idx + 2] = fill_color[2];
                    image->pixels[idx + 3] = fill_color[3];
                }
            }
        }
    }

    return WriteImagePng(*image, screenshot_path);
}

auto ApplyControlsShadowOverlay(std::filesystem::path const& screenshot_path,
                                PaintControlsNS::PaintLayoutMetrics const& layout,
                                int screenshot_width,
                                int screenshot_height) -> SP::Expected<void> {
    auto seam_width = static_cast<int>(std::round(std::clamp(layout.controls_spacing * 0.55f, 10.0f, 22.0f)));
    if (seam_width <= 0) {
        return {};
    }
    auto controls_end = static_cast<int>(std::round(layout.padding_x + layout.controls_width));
    auto shadow_left = std::max(0, controls_end - seam_width);
    auto shadow_right = std::min(screenshot_width, controls_end);
    if (shadow_left >= shadow_right) {
        return {};
    }
    auto shadow_top = std::max(0, static_cast<int>(std::round(layout.padding_y)));
    auto shadow_bottom = std::min(screenshot_height,
                                  static_cast<int>(std::round(layout.padding_y + layout.canvas_height)));
    if (shadow_top >= shadow_bottom) {
        return {};
    }

    SoftwareImage seam{};
    seam.width = screenshot_width;
    seam.height = screenshot_height;
    seam.pixels.assign(static_cast<std::size_t>(seam.width) * static_cast<std::size_t>(seam.height) * 4u, 0);
    auto seam_color = float_color_to_bytes({0.10f, 0.12f, 0.16f, 1.0f});
    auto row_bytes = static_cast<std::size_t>(seam.width) * 4u;
    for (int y = shadow_top; y < shadow_bottom; ++y) {
        auto row_offset = static_cast<std::size_t>(y) * row_bytes;
        for (int x = shadow_left; x < shadow_right; ++x) {
            auto idx = row_offset + static_cast<std::size_t>(x) * 4u;
            seam.pixels[idx + 0] = seam_color[0];
            seam.pixels[idx + 1] = seam_color[1];
            seam.pixels[idx + 2] = seam_color[2];
            seam.pixels[idx + 3] = seam_color[3];
        }
    }

    SP::UI::Screenshot::OverlayImageView overlay{
        .width = seam.width,
        .height = seam.height,
        .pixels = std::span<const std::uint8_t>(seam.pixels.data(), seam.pixels.size()),
    };
    SP::UI::Screenshot::OverlayRegion region{
        .left = shadow_left,
        .top = shadow_top,
        .right = shadow_right,
        .bottom = shadow_bottom,
    };
    auto status = SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, overlay, region);
    if (!status) {
        return status;
    }
    return {};
}

auto MakePostprocessHook(PaintControlsNS::PaintLayoutMetrics layout,
                         int screenshot_width,
                         int screenshot_height,
                         std::shared_ptr<SoftwareImage const> strokes)
    -> std::function<SP::Expected<void>(std::filesystem::path const&,
                                        std::optional<std::filesystem::path> const&)> {
    return [layout,
            screenshot_width,
            screenshot_height,
            strokes = std::move(strokes)](std::filesystem::path const& output_png,
                                          std::optional<std::filesystem::path> const& baseline_png)
               -> SP::Expected<void> {
        if (strokes) {
            if (auto status = OverlayStrokesOntoPng(output_png, *strokes, layout); !status) {
                return status;
            }
        }
        if (auto status = ApplyControlsBackgroundOverlay(output_png,
                                                         layout,
                                                         screenshot_width,
                                                         screenshot_height,
                                                         baseline_png);
            !status) {
            return status;
        }
        return ApplyControlsShadowOverlay(output_png, layout, screenshot_width, screenshot_height);
    };
}

} // namespace SP::Examples::PaintScreenshot
