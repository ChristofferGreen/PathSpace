#include "ScreenshotService.hpp"

#include <pathspace/core/Error.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <third_party/stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

namespace SP::UI::Screenshot {

namespace {

struct ScreenshotImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

struct DiffStats {
    double mean_error = 0.0;
    std::uint32_t max_channel_delta = 0;
};

struct RunMetrics {
    std::string status;
    std::uint64_t timestamp_ns = 0;
    bool hardware_capture = false;
    bool require_present = false;
    std::optional<double> mean_error;
    std::optional<std::uint32_t> max_channel_delta;
    std::optional<std::string> screenshot_path;
    std::optional<std::string> diff_path;
};

auto now_timestamp_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto make_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::UnknownError, std::move(message)};
}

auto ensure_directory(std::filesystem::path const& path) -> bool {
    auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

auto write_png(std::span<const std::uint8_t> pixels,
               int width,
               int height,
               std::filesystem::path const& output_path) -> SP::Expected<void> {
    if (width <= 0 || height <= 0) {
        return std::unexpected(make_error("invalid screenshot dimensions"));
    }
    if (!ensure_directory(output_path)) {
        return std::unexpected(make_error("failed to create screenshot directory"));
    }
    auto row_bytes = static_cast<std::size_t>(width) * 4u;
    if (pixels.size() != row_bytes * static_cast<std::size_t>(height)) {
        return std::unexpected(make_error("screenshot pixel buffer has unexpected length"));
    }
    if (stbi_write_png(output_path.string().c_str(),
                       width,
                       height,
                       4,
                       pixels.data(),
                       static_cast<int>(row_bytes)) == 0) {
        return std::unexpected(make_error("failed to encode screenshot png"));
    }
    return {};
}

auto pack_framebuffer(std::vector<std::uint8_t> const& framebuffer,
                      int width,
                      int height) -> SP::Expected<std::vector<std::uint8_t>> {
    if (width <= 0 || height <= 0) {
        return std::unexpected(make_error("invalid framebuffer dimensions"));
    }
    auto row_pixels = static_cast<std::size_t>(width) * 4u;
    auto required_bytes = row_pixels * static_cast<std::size_t>(height);
    if (framebuffer.size() == required_bytes) {
        return framebuffer;
    }
    if (height <= 0) {
        return std::unexpected(make_error("invalid framebuffer height"));
    }
    if (framebuffer.size() % static_cast<std::size_t>(height) != 0) {
        return std::unexpected(make_error("framebuffer stride mismatch"));
    }
    auto stride = framebuffer.size() / static_cast<std::size_t>(height);
    if (stride < row_pixels) {
        return std::unexpected(make_error("framebuffer stride smaller than row size"));
    }
    std::vector<std::uint8_t> packed(required_bytes);
    for (int y = 0; y < height; ++y) {
        auto const* src = framebuffer.data() + static_cast<std::size_t>(y) * stride;
        auto* dst = packed.data() + static_cast<std::size_t>(y) * row_pixels;
        std::memcpy(dst, src, row_pixels);
    }
    return packed;
}

auto capture_present_frame(SP::PathSpace& space,
                           SP::UI::Builders::WindowPath const& window_path,
                           std::string const& view_name,
                           std::chrono::milliseconds timeout)
    -> std::optional<SP::UI::Builders::Window::WindowPresentResult> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto present = SP::UI::Builders::Window::Present(space, window_path, view_name);
        if (!present) {
            auto const& error = present.error();
            if (error.code == SP::Error::Code::NoObjectFound
                || error.code == SP::Error::Code::NoSuchPath) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            std::cerr << "ScreenshotService: Window::Present failed\n";
            return std::nullopt;
        }
        if (present->stats.skipped || present->framebuffer.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        return std::optional<SP::UI::Builders::Window::WindowPresentResult>{std::move(*present)};
    }
    std::cerr << "ScreenshotService: Window::Present timed out\n";
    return std::nullopt;
}

auto load_png_rgba(std::filesystem::path const& path) -> std::optional<ScreenshotImage> {
    auto absolute = path.string();
    std::ifstream file(absolute, std::ios::binary);
    if (!file) {
        std::cerr << "ScreenshotService: failed to open PNG '" << absolute << "'\n";
        return std::nullopt;
    }
    std::vector<std::uint8_t> buffer(std::istreambuf_iterator<char>(file), {});
    if (buffer.empty()) {
        std::cerr << "ScreenshotService: PNG '" << absolute << "' is empty\n";
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    int components = 0;
    auto* data =
        stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()), &width, &height, &components, 4);
    if (data == nullptr) {
        auto reason = stbi_failure_reason();
        std::cerr << "ScreenshotService: failed to decode PNG '" << absolute << "'";
        if (reason != nullptr) {
            std::cerr << " (" << reason << ")";
        }
        std::cerr << "\n";
        return std::nullopt;
    }
    auto total = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    ScreenshotImage image{};
    image.width = width;
    image.height = height;
    image.pixels.assign(data, data + total);
    stbi_image_free(data);
    return image;
}

auto compare_png(std::filesystem::path const& baseline_path,
                 std::filesystem::path const& capture_path,
                 std::optional<std::filesystem::path> const& diff_path)
    -> std::optional<DiffStats> {
    auto baseline = load_png_rgba(baseline_path);
    if (!baseline) {
        return std::nullopt;
    }
    auto capture = load_png_rgba(capture_path);
    if (!capture) {
        return std::nullopt;
    }
    if (baseline->width != capture->width || baseline->height != capture->height) {
        std::cerr << "ScreenshotService: baseline dimensions (" << baseline->width << "x"
                  << baseline->height << ") do not match capture (" << capture->width
                  << "x" << capture->height << ")\n";
        return std::nullopt;
    }

    DiffStats stats{};
    auto channel_count = static_cast<std::size_t>(baseline->width)
                         * static_cast<std::size_t>(baseline->height) * 4u;
    double total_error = 0.0;

    std::vector<std::uint8_t> diff_pixels;
    auto write_diff = diff_path.has_value() && channel_count > 0;
    if (write_diff) {
        diff_pixels.resize(channel_count);
    }

    for (std::size_t offset = 0; offset < channel_count; offset += 4) {
        std::uint8_t pixel_delta = 0;
        for (int channel = 0; channel < 4; ++channel) {
            auto delta = static_cast<std::uint8_t>(
                std::abs(static_cast<int>(baseline->pixels[offset + channel])
                         - static_cast<int>(capture->pixels[offset + channel])));
            stats.max_channel_delta = std::max<std::uint32_t>(stats.max_channel_delta, delta);
            total_error += static_cast<double>(delta) / 255.0;
            pixel_delta = std::max(pixel_delta, delta);
        }
        if (write_diff) {
            diff_pixels[offset + 0] = pixel_delta;
            diff_pixels[offset + 1] = pixel_delta;
            diff_pixels[offset + 2] = pixel_delta;
            diff_pixels[offset + 3] = 255;
        }
    }

    if (channel_count == 0) {
        stats.mean_error = 0.0;
    } else {
        stats.mean_error = total_error / static_cast<double>(channel_count);
    }

    if (write_diff) {
        if (stats.max_channel_delta == 0) {
            std::error_code ec;
            std::filesystem::remove(*diff_path, ec);
        } else {
            (void)write_png(std::span<const std::uint8_t>(diff_pixels.data(), diff_pixels.size()),
                            baseline->width,
                            baseline->height,
                            *diff_path);
        }
    }

    return stats;
}

auto replace_value(SP::PathSpace& space, std::string const& path, auto const& value) -> SP::Expected<void> {
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto make_path(std::string base, std::string_view leaf) -> std::string {
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(leaf.begin(), leaf.end());
    return base;
}

auto normalize_root(std::string const& root, std::string const& ns) -> std::string {
    if (root.empty()) {
        return ns.empty() ? std::string{"/diagnostics/ui/screenshot"} : "/diagnostics/ui/screenshot/" + ns;
    }
    std::string normalized = root;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (!ns.empty()) {
        normalized.push_back('/');
        normalized.append(ns);
    }
    return normalized;
}

void publish_baseline_metrics(SP::PathSpace& space,
                              std::string const& root,
                              BaselineMetadata const& metadata,
                              double tolerance) {
    auto baseline_root = make_path(root, "baseline");
    replace_value(space,
                  make_path(baseline_root, "manifest_revision"),
                  static_cast<std::int64_t>(metadata.manifest_revision.value_or(0)));
    replace_value(space, make_path(baseline_root, "tag"), metadata.tag.value_or(std::string{}));
    replace_value(space, make_path(baseline_root, "sha256"), metadata.sha256.value_or(std::string{}));
    replace_value(space,
                  make_path(baseline_root, "width"),
                  static_cast<std::int64_t>(metadata.width.value_or(0)));
    replace_value(space,
                  make_path(baseline_root, "height"),
                  static_cast<std::int64_t>(metadata.height.value_or(0)));
    replace_value(space, make_path(baseline_root, "renderer"), metadata.renderer.value_or(std::string{}));
    replace_value(space, make_path(baseline_root, "captured_at"), metadata.captured_at.value_or(std::string{}));
    replace_value(space, make_path(baseline_root, "commit"), metadata.commit.value_or(std::string{}));
    replace_value(space, make_path(baseline_root, "notes"), metadata.notes.value_or(std::string{}));
    replace_value(space, make_path(baseline_root, "tolerance"), metadata.tolerance.value_or(tolerance));
}

void record_last_run_metrics(SP::PathSpace& space,
                             std::string const& root,
                             RunMetrics const& metrics) {
    auto last_run_root = make_path(make_path(root, "baseline"), "last_run");
    replace_value(space,
                  make_path(last_run_root, "timestamp_ns"),
                  static_cast<std::int64_t>(metrics.timestamp_ns));
    replace_value(space, make_path(last_run_root, "status"), metrics.status);
    replace_value(space, make_path(last_run_root, "hardware_capture"), metrics.hardware_capture);
    replace_value(space, make_path(last_run_root, "require_present"), metrics.require_present);
    replace_value(space, make_path(last_run_root, "mean_error"), metrics.mean_error.value_or(0.0));
    replace_value(space,
                  make_path(last_run_root, "max_channel_delta"),
                  static_cast<std::int64_t>(metrics.max_channel_delta.value_or(0)));
    replace_value(space, make_path(last_run_root, "screenshot_path"), metrics.screenshot_path.value_or(std::string{}));
    replace_value(space, make_path(last_run_root, "diff_path"), metrics.diff_path.value_or(std::string{}));
}

void write_metrics_snapshot(std::filesystem::path const& path,
                            BaselineMetadata const& metadata,
                            RunMetrics const& metrics) {
    if (path.empty()) {
        return;
    }
    if (!ensure_directory(path)) {
        std::cerr << "ScreenshotService: failed to create metrics directory '"
                  << path.parent_path().string() << "'\n";
        return;
    }
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
        std::cerr << "ScreenshotService: failed to open metrics file '" << path.string() << "'\n";
        return;
    }
    auto format_string = [](std::optional<std::string> const& value) {
        if (!value || value->empty()) {
            return std::string{"null"};
        }
        std::string escaped;
        escaped.reserve(value->size() + 8);
        for (char ch : *value) {
            switch (ch) {
            case '\\':
                escaped.append("\\\\");
                break;
            case '"':
                escaped.append("\\\"");
                break;
            case '\n':
                escaped.append("\\n");
                break;
            case '\r':
                escaped.append("\\r");
                break;
            case '\t':
                escaped.append("\\t");
                break;
            default:
                escaped.push_back(ch);
                break;
            }
        }
        return std::string{"\""} + escaped + "\"";
    };
    auto format_int = [](std::optional<int> const& value) {
        if (!value) {
            return std::string{"null"};
        }
        return std::to_string(*value);
    };
    auto format_double = [](std::optional<double> const& value) {
        if (!value) {
            return std::string{"null"};
        }
        std::ostringstream out;
        out << std::setprecision(8) << *value;
        return out.str();
    };

    stream << "{\n"
           << "  \"baseline\": {\n"
           << "    \"manifest_revision\": " << format_int(metadata.manifest_revision) << ",\n"
           << "    \"tag\": " << format_string(metadata.tag) << ",\n"
           << "    \"sha256\": " << format_string(metadata.sha256) << ",\n"
           << "    \"width\": " << format_int(metadata.width) << ",\n"
           << "    \"height\": " << format_int(metadata.height) << ",\n"
           << "    \"renderer\": " << format_string(metadata.renderer) << ",\n"
           << "    \"captured_at\": " << format_string(metadata.captured_at) << ",\n"
           << "    \"commit\": " << format_string(metadata.commit) << ",\n"
           << "    \"notes\": " << format_string(metadata.notes) << ",\n"
           << "    \"tolerance\": " << format_double(metadata.tolerance) << "\n"
           << "  },\n"
           << "  \"run\": {\n"
           << "    \"status\": " << format_string(metrics.status) << ",\n"
           << "    \"timestamp_ns\": " << metrics.timestamp_ns << ",\n"
           << "    \"hardware_capture\": " << (metrics.hardware_capture ? "true" : "false") << ",\n"
           << "    \"require_present\": " << (metrics.require_present ? "true" : "false") << ",\n"
           << "    \"mean_error\": " << format_double(metrics.mean_error) << ",\n"
           << "    \"max_channel_delta\": "
           << (metrics.max_channel_delta ? std::to_string(*metrics.max_channel_delta) : "null") << ",\n"
           << "    \"screenshot_path\": " << format_string(metrics.screenshot_path) << ",\n"
           << "    \"diff_path\": " << format_string(metrics.diff_path) << "\n"
           << "  }\n"
           << "}\n";
}

auto unexpected_capture_failure(std::string_view reason) -> SP::Expected<ScreenshotResult> {
    return std::unexpected(make_error(std::string(reason)));
}

} // namespace

auto OverlayRegionOnPng(std::filesystem::path const& screenshot_path,
                        OverlayImageView const& overlay,
                        OverlayRegion region) -> SP::Expected<void> {
    if (overlay.width <= 0 || overlay.height <= 0) {
        return std::unexpected(make_error("overlay dimensions must be positive"));
    }
    auto expected_bytes = static_cast<std::size_t>(overlay.width)
                          * static_cast<std::size_t>(overlay.height) * 4u;
    if (overlay.pixels.size() != expected_bytes) {
        return std::unexpected(make_error("overlay pixel buffer length mismatch"));
    }
    auto screenshot = load_png_rgba(screenshot_path);
    if (!screenshot) {
        return std::unexpected(make_error("failed to load screenshot for overlay"));
    }
    if (screenshot->width != overlay.width || screenshot->height != overlay.height) {
        return std::unexpected(make_error("screenshot size mismatch during overlay"));
    }

    OverlayRegion clamped = region;
    clamped.left = std::clamp(clamped.left, 0, screenshot->width);
    clamped.top = std::clamp(clamped.top, 0, screenshot->height);
    clamped.right = std::clamp(clamped.right, clamped.left, screenshot->width);
    clamped.bottom = std::clamp(clamped.bottom, clamped.top, screenshot->height);
    if (clamped.left >= clamped.right || clamped.top >= clamped.bottom) {
        return std::unexpected(make_error("invalid overlay region"));
    }

    auto row_bytes = static_cast<std::size_t>(screenshot->width) * 4u;
    auto copy_bytes = static_cast<std::size_t>(clamped.right - clamped.left) * 4u;
    for (int y = clamped.top; y < clamped.bottom; ++y) {
        auto dst = screenshot->pixels.data()
                   + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(clamped.left) * 4u;
        auto src = overlay.pixels.data()
                   + static_cast<std::size_t>(y) * row_bytes + static_cast<std::size_t>(clamped.left) * 4u;
        std::memcpy(dst, src, copy_bytes);
    }

    return write_png(std::span<const std::uint8_t>(screenshot->pixels.data(), screenshot->pixels.size()),
                     screenshot->width,
                     screenshot->height,
                     screenshot_path);
}

auto ScreenshotService::Capture(ScreenshotRequest const& request) -> SP::Expected<ScreenshotResult> {
    if (request.width <= 0 || request.height <= 0) {
        return unexpected_capture_failure("invalid screenshot dimensions");
    }
    auto telemetry_root = normalize_root(request.telemetry_root, request.telemetry_namespace);
    publish_baseline_metrics(request.space, telemetry_root, request.baseline_metadata, request.max_mean_error);

    ScreenshotResult result{};
    result.artifact = request.output_png;
    result.diff_artifact = request.diff_png;

    RunMetrics run{};
    run.timestamp_ns = now_timestamp_ns();
    run.require_present = request.require_present;
    run.screenshot_path = request.output_png.string();
    if (request.diff_png) {
        run.diff_path = request.diff_png->string();
    }

    auto emit_metrics = [&](std::string_view status) {
        run.status.assign(status.begin(), status.end());
        run.hardware_capture = result.hardware_capture;
        run.mean_error = result.mean_error;
        run.max_channel_delta = result.max_channel_delta;
        record_last_run_metrics(request.space, telemetry_root, run);
        if (request.metrics_json) {
            write_metrics_snapshot(*request.metrics_json, request.baseline_metadata, run);
        }
    };

    if (request.hooks.ensure_ready) {
        auto ready = request.hooks.ensure_ready();
        if (!ready) {
            emit_metrics("ensure_ready_failed");
            return std::unexpected(ready.error());
        }
    }

    std::vector<std::uint8_t> capture_pixels;
    bool hardware_capture = false;
    if (!request.force_software) {
        auto present = capture_present_frame(request.space,
                                             request.window_path,
                                             request.view_name,
                                             request.present_timeout);
        if (present) {
            auto packed = pack_framebuffer(present->framebuffer, request.width, request.height);
            if (!packed) {
                std::cerr << "ScreenshotService: framebuffer packing failed\n";
            } else {
                capture_pixels = std::move(*packed);
                hardware_capture = true;
            }
        }
    }

    if (hardware_capture) {
        FramebufferView view{
            .pixels = std::span<std::uint8_t>(capture_pixels.data(), capture_pixels.size()),
            .width = request.width,
            .height = request.height,
        };
        if (request.hooks.postprocess_framebuffer) {
            auto post = request.hooks.postprocess_framebuffer(view);
            if (!post) {
                emit_metrics("postprocess_failed");
                return std::unexpected(post.error());
            }
        }
        auto write_status = write_png(view.pixels, view.width, view.height, request.output_png);
        if (!write_status) {
            emit_metrics("write_failed");
            return std::unexpected(write_status.error());
        }
        if (request.hooks.postprocess_png) {
            auto post_file = request.hooks.postprocess_png(request.output_png);
            if (!post_file) {
                emit_metrics("postprocess_failed");
                return std::unexpected(post_file.error());
            }
        }
    } else {
        if (request.require_present) {
            emit_metrics("capture_failed");
            return unexpected_capture_failure("hardware capture required but Window::Present failed");
        }
        if (request.hooks.fallback_writer) {
            auto fallback = request.hooks.fallback_writer();
            if (!fallback) {
                emit_metrics("fallback_failed");
                return std::unexpected(fallback.error());
            }
            if (request.hooks.postprocess_png) {
                auto post_file = request.hooks.postprocess_png(request.output_png);
                if (!post_file) {
                    emit_metrics("postprocess_failed");
                    return std::unexpected(post_file.error());
                }
            }
        } else {
            emit_metrics("fallback_unavailable");
            return unexpected_capture_failure("no hardware capture and no fallback writer available");
        }
    }

    result.hardware_capture = hardware_capture;

    if (request.baseline_png) {
        auto diff = compare_png(*request.baseline_png, request.output_png, request.diff_png);
        if (!diff) {
            emit_metrics("compare_failed");
            return unexpected_capture_failure("failed to compare screenshots");
        }
        result.mean_error = diff->mean_error;
        result.max_channel_delta = diff->max_channel_delta;
        if (diff->mean_error > request.max_mean_error) {
            emit_metrics("mismatch");
            return unexpected_capture_failure("screenshot differed from baseline");
        }
        result.matched_baseline = true;
        result.status = "match";
        emit_metrics("match");
    } else {
        result.status = "captured";
        emit_metrics("captured");
    }

    return result;
}

} // namespace SP::UI::Screenshot
