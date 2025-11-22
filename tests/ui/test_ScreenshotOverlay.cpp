#include "third_party/doctest.h"

#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <third_party/stb_image.h>
#include <third_party/stb_image_write.h>

namespace {

auto unique_png_path(std::string_view prefix) -> std::filesystem::path {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string filename(prefix);
    filename.push_back('_');
    filename.append(std::to_string(stamp));
    filename.append(".png");
    return temp_dir / filename;
}

void write_png(std::filesystem::path const& path,
               std::span<const std::uint8_t> pixels,
               int width,
               int height) {
    auto row_bytes = width * 4;
    REQUIRE(stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), row_bytes) != 0);
}

auto load_png_rgba(std::filesystem::path const& path,
                   int& width,
                   int& height) -> std::vector<std::uint8_t> {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::vector<std::uint8_t> buffer(std::istreambuf_iterator<char>(file), {});
    REQUIRE_FALSE(buffer.empty());
    int components = 0;
    auto* data = stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &width, &height, &components, 4);
    REQUIRE(data != nullptr);
    auto total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    std::vector<std::uint8_t> pixels(data, data + total);
    stbi_image_free(data);
    return pixels;
}

} // namespace

TEST_SUITE("ScreenshotOverlay") {
TEST_CASE("overlays region onto png") {
    auto screenshot_path = unique_png_path("overlay");
    int width = 4;
    int height = 4;
    std::vector<std::uint8_t> base(width * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto idx = (y * width + x) * 4;
            base[idx + 0] = 10;
            base[idx + 1] = 20;
            base[idx + 2] = 30;
            base[idx + 3] = 255;
        }
    }
    write_png(screenshot_path, base, width, height);

    std::vector<std::uint8_t> overlay = base;
    for (int y = 1; y < 3; ++y) {
        for (int x = 1; x < 3; ++x) {
            auto idx = (y * width + x) * 4;
            overlay[idx + 0] = 200;
            overlay[idx + 1] = 50;
            overlay[idx + 2] = 80;
            overlay[idx + 3] = 255;
        }
    }

    SP::UI::Screenshot::OverlayImageView view{
        .width = width,
        .height = height,
        .pixels = std::span<const std::uint8_t>(overlay.data(), overlay.size()),
    };
    SP::UI::Screenshot::OverlayRegion region{
        .left = 1,
        .top = 1,
        .right = 3,
        .bottom = 3,
    };

    auto result = SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, view, region);
    CHECK(result);

    int decoded_width = 0;
    int decoded_height = 0;
    auto decoded = load_png_rgba(screenshot_path, decoded_width, decoded_height);
    REQUIRE(decoded_width == width);
    REQUIRE(decoded_height == height);

    auto channel = [&](int x, int y, int c) -> std::uint8_t {
        auto offset = (y * width + x) * 4 + c;
        return decoded[offset];
    };

    CHECK(channel(0, 0, 0) == 10);
    CHECK(channel(1, 1, 0) == 200);
    CHECK(channel(2, 2, 2) == 80);
    CHECK(channel(1, 1, 3) == 255);
    CHECK(channel(0, 3, 1) == 20);
    std::error_code ec;
    std::filesystem::remove(screenshot_path, ec);
}

TEST_CASE("rejects dimension mismatch") {
    auto screenshot_path = unique_png_path("overlay_mismatch");
    int width = 2;
    int height = 2;
    std::vector<std::uint8_t> base(width * height * 4, 180);
    write_png(screenshot_path, base, width, height);

    std::vector<std::uint8_t> overlay(static_cast<std::size_t>(width + 1) * static_cast<std::size_t>(height) * 4u, 255);
    SP::UI::Screenshot::OverlayImageView view{
        .width = width + 1,
        .height = height,
        .pixels = std::span<const std::uint8_t>(overlay.data(), overlay.size()),
    };
    SP::UI::Screenshot::OverlayRegion region{
        .left = 0,
        .top = 0,
        .right = width,
        .bottom = height,
    };

    auto result = SP::UI::Screenshot::OverlayRegionOnPng(screenshot_path, view, region);
    CHECK_FALSE(result);

    std::error_code ec;
    std::filesystem::remove(screenshot_path, ec);
}
}
