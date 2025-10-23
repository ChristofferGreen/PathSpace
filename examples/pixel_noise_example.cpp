#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <third_party/stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(PATHSPACE_ENABLE_UI)
int main() {
    std::cerr << "pixel_noise_example requires PATHSPACE_ENABLE_UI=ON.\n";
    return 1;
}
#elif !defined(__APPLE__)
int main() {
    std::cerr << "pixel_noise_example currently supports only macOS builds.\n";
    return 1;
}
#else

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <pathspace/ui/LocalWindowBridge.hpp>

using namespace SP;
using namespace SP::UI;
namespace Builders = SP::UI::Builders;
namespace UIScene = SP::UI::Scene;

namespace {

struct Options {
    int width = 1280;
    int height = 720;
    bool headless = false;
    bool capture_framebuffer = false;
    bool report_metrics = false;
    bool report_present_call_time = false;
    bool use_metal_backend = false;
    double present_refresh_hz = 60.0;
    std::size_t max_frames = 0;
    std::chrono::duration<double> report_interval = std::chrono::seconds{1};
    std::uint64_t seed = 0;
    std::optional<std::chrono::duration<double>> runtime_limit{};
    std::optional<double> budget_present_ms{};
    std::optional<double> budget_render_ms{};
    std::optional<double> min_fps{};
    std::optional<std::string> baseline_path{};
    std::optional<std::filesystem::path> frame_output_path{};
};

struct BaselineSummary {
    std::size_t frames = 0;
    double elapsed_seconds = 0.0;
    double average_fps = 0.0;
    double average_present_ms = 0.0;
    double average_render_ms = 0.0;
    double average_present_call_ms = 0.0;
    double total_present_ms = 0.0;
    double total_render_ms = 0.0;
};

struct TileSummary {
    std::size_t frames = 0;
    std::size_t progressive_frames = 0;
    double average_tiles_updated = 0.0;
    double average_tiles_dirty = 0.0;
    double average_tiles_total = 0.0;
    double average_tiles_skipped = 0.0;
    double average_tiles_copied = 0.0;
    double average_bytes_copied = 0.0;
    double average_progressive_jobs = 0.0;
    double average_progressive_workers = 0.0;
    double average_encode_jobs = 0.0;
    double average_encode_workers = 0.0;
    double average_rects_coalesced = 0.0;
    double average_skip_seq_odd = 0.0;
    double average_recopy_after_seq_change = 0.0;
    std::uint64_t last_tile_size = 0;
    std::uint64_t last_tiles_total = 0;
    std::uint64_t last_drawable_count = 0;
    bool last_tile_diagnostics_enabled = false;
};

auto format_double(double value) -> std::string {
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss << std::setprecision(6) << value;
    return oss.str();
}

auto severity_to_string(Builders::Diagnostics::PathSpaceError::Severity severity) -> std::string {
    using Severity = Builders::Diagnostics::PathSpaceError::Severity;
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Recoverable:
        return "recoverable";
    case Severity::Fatal:
        return "fatal";
    }
    return "unknown";
}

void write_frame_capture_png_or_exit(Builders::SoftwareFramebuffer const& framebuffer,
                                     std::filesystem::path const& output_path) {
    if (framebuffer.width <= 0 || framebuffer.height <= 0) {
        std::cerr << "pixel_noise_example: framebuffer capture has invalid dimensions "
                  << framebuffer.width << "x" << framebuffer.height << '\n';
        std::exit(1);
    }

    auto const format = framebuffer.pixel_format;
    bool const is_rgba = format == Builders::PixelFormat::RGBA8Unorm
                      || format == Builders::PixelFormat::RGBA8Unorm_sRGB;
    bool const is_bgra = format == Builders::PixelFormat::BGRA8Unorm
                      || format == Builders::PixelFormat::BGRA8Unorm_sRGB;
    if (!(is_rgba || is_bgra)) {
        std::cerr << "pixel_noise_example: framebuffer capture pixel format not supported for PNG export ("
                  << static_cast<int>(format) << ")\n";
        std::exit(1);
    }

    auto const parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec{};
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "pixel_noise_example: failed to create directory '" << parent.string()
                      << "' for frame capture: " << ec.message() << '\n';
            std::exit(1);
        }
    }

    auto const width = framebuffer.width;
    auto const height = framebuffer.height;
    auto const row_stride = framebuffer.row_stride_bytes != 0
        ? static_cast<std::size_t>(framebuffer.row_stride_bytes)
        : static_cast<std::size_t>(width) * 4;
    auto const packed_row_bytes = static_cast<std::size_t>(width) * 4;
    auto const required_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));
    if (framebuffer.pixels.size() < required_bytes) {
        std::cerr << "pixel_noise_example: framebuffer capture underrun (have "
                  << framebuffer.pixels.size() << " bytes, expected at least "
                  << required_bytes << ")\n";
        std::exit(1);
    }

    bool const needs_copy = is_bgra || row_stride != packed_row_bytes;
    std::vector<std::uint8_t> rgba_storage;
    std::uint8_t const* png_data = nullptr;
    int png_stride = 0;

    if (needs_copy) {
        rgba_storage.resize(packed_row_bytes * static_cast<std::size_t>(height));
        auto* dst = rgba_storage.data();
        auto const* src = framebuffer.pixels.data();
        for (int y = 0; y < height; ++y) {
            auto const* src_row = src + static_cast<std::size_t>(y) * row_stride;
            auto* dst_row = dst + static_cast<std::size_t>(y) * packed_row_bytes;
            if (is_bgra) {
                for (int x = 0; x < width; ++x) {
                    auto const src_offset = static_cast<std::size_t>(x) * 4;
                    dst_row[src_offset + 0] = src_row[src_offset + 2];
                    dst_row[src_offset + 1] = src_row[src_offset + 1];
                    dst_row[src_offset + 2] = src_row[src_offset + 0];
                    dst_row[src_offset + 3] = src_row[src_offset + 3];
                }
            } else {
                std::memcpy(dst_row, src_row, packed_row_bytes);
            }
        }
        png_data = rgba_storage.data();
        png_stride = static_cast<int>(packed_row_bytes);
    } else {
        png_data = framebuffer.pixels.data();
        png_stride = static_cast<int>(row_stride);
    }

    auto const path_string = output_path.string();
    if (stbi_write_png(path_string.c_str(), width, height, 4, png_data, png_stride) == 0) {
        std::cerr << "pixel_noise_example: failed to write PNG frame capture to '"
                  << path_string << "'\n";
        std::exit(1);
    }

    std::cout << "pixel_noise_example: saved frame capture to " << path_string << '\n';
}

void write_baseline_metrics(Options const& options,
                            BaselineSummary const& summary,
                            TileSummary const& tiles,
                            Builders::Diagnostics::TargetMetrics const& metrics,
                            std::string const& backend_kind,
                            std::filesystem::path const& output_path) {
    namespace fs = std::filesystem;

    fs::path path = output_path;
    if (path.empty()) {
        std::cerr << "pixel_noise_example: --write-baseline path is empty\n";
        std::exit(1);
    }

    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        if (!fs::create_directories(parent, ec) && ec) {
            std::cerr << "pixel_noise_example: failed to create baseline directory '"
                      << parent.string() << "': " << ec.message() << '\n';
            std::exit(1);
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        std::cerr << "pixel_noise_example: failed to open baseline file '"
                  << path.string() << "' for writing\n";
        std::exit(1);
    }

    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now_time);
#else
    gmtime_r(&now_time, &tm);
#endif
    char timestamp_buffer[32]{};
    std::string timestamp{"unknown"};
    if (std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%dT%H:%M:%SZ", &tm) != 0) {
        timestamp.assign(timestamp_buffer);
    }

    out << std::boolalpha;
    out << "{\n";
    out << "  \"generatedAt\": " << std::quoted(timestamp) << ",\n";
    out << "  \"command\": {\n";
    out << "    \"width\": " << options.width << ",\n";
    out << "    \"height\": " << options.height << ",\n";
    out << "    \"headless\": " << options.headless << ",\n";
    out << "    \"captureFramebuffer\": " << options.capture_framebuffer << ",\n";
    out << "    \"presentRefreshHz\": " << format_double(options.present_refresh_hz) << ",\n";
    out << "    \"maxFrames\": " << options.max_frames << ",\n";
    out << "    \"seed\": " << options.seed << ",\n";
    out << "    \"runtimeLimitSeconds\": ";
    if (options.runtime_limit) {
        out << format_double(options.runtime_limit->count());
    } else {
        out << "null";
    }
    out << ",\n";
    out << "    \"budgetPresentMs\": ";
    if (options.budget_present_ms) {
        out << format_double(*options.budget_present_ms);
    } else {
        out << "null";
    }
    out << ",\n";
    out << "    \"budgetRenderMs\": ";
    if (options.budget_render_ms) {
        out << format_double(*options.budget_render_ms);
    } else {
        out << "null";
    }
    out << ",\n";
    out << "    \"minFps\": ";
    if (options.min_fps) {
        out << format_double(*options.min_fps);
    } else {
        out << "null";
    }
    out << ",\n";
    out << "    \"backendKind\": " << std::quoted(backend_kind) << "\n";
    out << "  },\n";

    out << "  \"summary\": {\n";
    out << "    \"frames\": " << summary.frames << ",\n";
    out << "    \"elapsedSeconds\": " << format_double(summary.elapsed_seconds) << ",\n";
    out << "    \"averageFps\": " << format_double(summary.average_fps) << ",\n";
    out << "    \"averagePresentMs\": " << format_double(summary.average_present_ms) << ",\n";
    out << "    \"averageRenderMs\": " << format_double(summary.average_render_ms) << ",\n";
    out << "    \"averagePresentCallMs\": " << format_double(summary.average_present_call_ms) << ",\n";
    out << "    \"totalPresentMs\": " << format_double(summary.total_present_ms) << ",\n";
    out << "    \"totalRenderMs\": " << format_double(summary.total_render_ms) << "\n";
    out << "  },\n";

    out << "  \"tileStats\": {\n";
    out << "    \"frames\": " << tiles.frames << ",\n";
    out << "    \"progressiveFrames\": " << tiles.progressive_frames << ",\n";
    out << "    \"tileSize\": " << tiles.last_tile_size << ",\n";
    out << "    \"tilesTotal\": " << tiles.last_tiles_total << ",\n";
    out << "    \"drawableCount\": " << tiles.last_drawable_count << ",\n";
    out << "    \"tileDiagnosticsEnabled\": " << tiles.last_tile_diagnostics_enabled << ",\n";
    out << "    \"averageTilesUpdated\": " << format_double(tiles.average_tiles_updated) << ",\n";
    out << "    \"averageTilesDirty\": " << format_double(tiles.average_tiles_dirty) << ",\n";
    out << "    \"averageTilesTotal\": " << format_double(tiles.average_tiles_total) << ",\n";
    out << "    \"averageTilesSkipped\": " << format_double(tiles.average_tiles_skipped) << ",\n";
    out << "    \"averageTilesCopied\": " << format_double(tiles.average_tiles_copied) << ",\n";
    out << "    \"averageBytesCopied\": " << format_double(tiles.average_bytes_copied) << ",\n";
    out << "    \"averageProgressiveJobs\": " << format_double(tiles.average_progressive_jobs) << ",\n";
    out << "    \"averageProgressiveWorkers\": " << format_double(tiles.average_progressive_workers) << ",\n";
    out << "    \"averageEncodeJobs\": " << format_double(tiles.average_encode_jobs) << ",\n";
    out << "    \"averageEncodeWorkers\": " << format_double(tiles.average_encode_workers) << ",\n";
    out << "    \"averageRectsCoalesced\": " << format_double(tiles.average_rects_coalesced) << ",\n";
    out << "    \"averageSkipSeqOdd\": " << format_double(tiles.average_skip_seq_odd) << ",\n";
    out << "    \"averageRecopyAfterSeqChange\": " << format_double(tiles.average_recopy_after_seq_change) << ",\n";
    out << "    \"backendKind\": " << std::quoted(backend_kind) << "\n";
    out << "  },\n";

    out << "  \"residency\": {\n";
    out << "    \"cpuBytes\": " << metrics.cpu_bytes << ",\n";
    out << "    \"cpuSoftBytes\": " << metrics.cpu_soft_bytes << ",\n";
    out << "    \"cpuHardBytes\": " << metrics.cpu_hard_bytes << ",\n";
    out << "    \"gpuBytes\": " << metrics.gpu_bytes << ",\n";
    out << "    \"gpuSoftBytes\": " << metrics.gpu_soft_bytes << ",\n";
    out << "    \"gpuHardBytes\": " << metrics.gpu_hard_bytes << ",\n";
    out << "    \"cpuSoftBudgetRatio\": " << format_double(metrics.cpu_soft_budget_ratio) << ",\n";
    out << "    \"cpuHardBudgetRatio\": " << format_double(metrics.cpu_hard_budget_ratio) << ",\n";
    out << "    \"gpuSoftBudgetRatio\": " << format_double(metrics.gpu_soft_budget_ratio) << ",\n";
    out << "    \"gpuHardBudgetRatio\": " << format_double(metrics.gpu_hard_budget_ratio) << ",\n";
    out << "    \"cpuSoftExceeded\": " << metrics.cpu_soft_exceeded << ",\n";
    out << "    \"cpuHardExceeded\": " << metrics.cpu_hard_exceeded << ",\n";
    out << "    \"gpuSoftExceeded\": " << metrics.gpu_soft_exceeded << ",\n";
    out << "    \"gpuHardExceeded\": " << metrics.gpu_hard_exceeded << ",\n";
    out << "    \"cpuStatus\": " << std::quoted(metrics.cpu_residency_status) << ",\n";
    out << "    \"gpuStatus\": " << std::quoted(metrics.gpu_residency_status) << ",\n";
    out << "    \"overallStatus\": " << std::quoted(metrics.residency_overall_status) << ",\n";
    out << "    \"backendKind\": " << std::quoted(metrics.backend_kind) << ",\n";
    out << "    \"usedMetalTexture\": " << metrics.used_metal_texture << ",\n";
    out << "    \"lastError\": " << std::quoted(metrics.last_error) << ",\n";
    out << "    \"lastErrorCode\": " << metrics.last_error_code << ",\n";
    out << "    \"lastErrorRevision\": " << metrics.last_error_revision << ",\n";
    out << "    \"lastErrorSeverity\": " << std::quoted(severity_to_string(metrics.last_error_severity)) << ",\n";
    out << "    \"lastErrorTimestampNs\": " << metrics.last_error_timestamp_ns << ",\n";
    out << "    \"lastErrorDetail\": " << std::quoted(metrics.last_error_detail) << "\n";
    out << "  }\n";
    out << "}\n";
}

struct NoiseState {
    explicit NoiseState(std::uint64_t seed_value)
        : rng(static_cast<std::mt19937::result_type>(seed_value))
        , channel_dist(0, 255)
        , frame_index(0)
    {}

    std::mt19937 rng;
    std::uniform_int_distribution<int> channel_dist;
    std::uint64_t frame_index;
};

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running.store(false, std::memory_order_release);
}

template <typename T>
auto expect_or_exit(SP::Expected<T> value, char const* context) -> T {
    if (value) {
        return std::move(*value);
    }
    auto const& err = value.error();
    std::cerr << "pixel_noise_example: " << context << " failed";
    if (err.message.has_value()) {
        std::cerr << ": " << *err.message;
    } else {
        std::cerr << " (code " << static_cast<int>(err.code) << ')';
    }
    std::cerr << '\n';
    std::exit(1);
}

template <typename T>
void replace_value_or_exit(PathSpace& space,
                           std::string const& path,
                           T const& value,
                           char const* context) {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        std::cerr << "pixel_noise_example: " << context << " failed to clear old value";
        if (error.message.has_value()) {
            std::cerr << ": " << *error.message;
        }
        std::cerr << '\n';
        std::exit(1);
    }

    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        auto const& err = inserted.errors.front();
        std::cerr << "pixel_noise_example: " << context << " insert failed";
        if (err.message.has_value()) {
            std::cerr << ": " << *err.message;
        }
        std::cerr << '\n';
        std::exit(1);
    }
}

inline void expect_or_exit(SP::Expected<void> value, char const* context) {
    if (value) {
        return;
    }
    auto const& err = value.error();
    std::cerr << "pixel_noise_example: " << context << " failed";
    if (err.message.has_value()) {
        std::cerr << ": " << *err.message;
    } else {
        std::cerr << " (code " << static_cast<int>(err.code) << ')';
    }
    std::cerr << '\n';
    std::exit(1);
}

auto parse_int(std::string_view text, char const* label) -> int {
    try {
        size_t consumed = 0;
        int value = std::stoi(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return value;
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_size(std::string_view text, char const* label) -> std::size_t {
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return static_cast<std::size_t>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_seconds(std::string_view text, char const* label) -> std::chrono::duration<double> {
    try {
        size_t consumed = 0;
        double value = std::stod(std::string{text}, &consumed);
        if (consumed != text.size() || value <= 0.0) {
            throw std::invalid_argument{"expected positive number"};
        }
        return std::chrono::duration<double>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_seed(std::string_view text) -> std::uint64_t {
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(std::string{text}, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument{"trailing characters"};
        }
        return static_cast<std::uint64_t>(value);
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid seed '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_minutes(std::string_view text, char const* label) -> std::chrono::duration<double> {
    auto seconds = parse_seconds(text, label);
    return std::chrono::duration<double>(seconds.count() * 60.0);
}

auto parse_positive_double(std::string_view text, char const* label) -> double {
    try {
        size_t consumed = 0;
        double value = std::stod(std::string{text}, &consumed);
        if (consumed != text.size() || value < 0.0) {
            throw std::invalid_argument{"expected non-negative number"};
        }
        return value;
    } catch (std::exception const& ex) {
        std::cerr << "pixel_noise_example: invalid " << label << " '" << text << "': " << ex.what() << '\n';
        std::exit(1);
    }
}

auto parse_options(int argc, char** argv) -> Options {
    Options opts{};
    opts.seed = static_cast<std::uint64_t>(std::random_device{}());

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--windowed") {
            opts.headless = false;
        } else if (arg.rfind("--width=", 0) == 0) {
            opts.width = parse_int(arg.substr(8), "width");
        } else if (arg.rfind("--height=", 0) == 0) {
            opts.height = parse_int(arg.substr(9), "height");
        } else if (arg.rfind("--frames=", 0) == 0) {
            opts.max_frames = parse_size(arg.substr(9), "frames");
        } else if (arg.rfind("--report-interval=", 0) == 0) {
            opts.report_interval = parse_seconds(arg.substr(18), "report interval");
        } else if (arg.rfind("--seed=", 0) == 0) {
            opts.seed = parse_seed(arg.substr(7));
        } else if (arg.rfind("--present-refresh=", 0) == 0) {
            opts.present_refresh_hz = parse_positive_double(arg.substr(18), "present refresh");
        } else if (arg == "--capture-framebuffer") {
            opts.capture_framebuffer = true;
        } else if (arg == "--report-metrics") {
            opts.report_metrics = true;
        } else if (arg == "--report-extended") {
            opts.report_metrics = true;
            opts.report_present_call_time = true;
        } else if (arg == "--present-call-metric") {
            opts.report_present_call_time = true;
        } else if (arg == "--metal" || arg == "--backend=metal" || arg == "--backend=Metal2D") {
            opts.use_metal_backend = true;
        } else if (arg == "--software" || arg == "--backend=software" || arg == "--backend=Software2D") {
            opts.use_metal_backend = false;
        } else if (arg.rfind("--runtime-minutes=", 0) == 0) {
            opts.runtime_limit = parse_minutes(arg.substr(18), "runtime minutes");
        } else if (arg.rfind("--budget-present-ms=", 0) == 0) {
            constexpr std::string_view prefix = "--budget-present-ms=";
            opts.budget_present_ms = parse_positive_double(arg.substr(prefix.size()), "budget present ms");
        } else if (arg.rfind("--budget-render-ms=", 0) == 0) {
            constexpr std::string_view prefix = "--budget-render-ms=";
            opts.budget_render_ms = parse_positive_double(arg.substr(prefix.size()), "budget render ms");
        } else if (arg.rfind("--min-fps=", 0) == 0) {
            constexpr std::string_view prefix = "--min-fps=";
            opts.min_fps = parse_positive_double(arg.substr(prefix.size()), "min fps");
        } else if (arg.rfind("--write-baseline=", 0) == 0) {
            constexpr std::string_view prefix = "--write-baseline=";
            auto path = arg.substr(prefix.size());
            if (path.empty()) {
                std::cerr << "pixel_noise_example: --write-baseline requires a non-empty path\n";
                std::exit(1);
            }
            opts.baseline_path = std::string{path};
        } else if (arg.rfind("--write-frame=", 0) == 0) {
            constexpr std::string_view prefix = "--write-frame=";
            auto path = arg.substr(prefix.size());
            if (path.empty()) {
                std::cerr << "pixel_noise_example: --write-frame requires a non-empty path\n";
                std::exit(1);
            }
            opts.frame_output_path = std::filesystem::path{std::string{path}};
            opts.capture_framebuffer = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: pixel_noise_example [options]\n"
                      << "Options:\n"
                      << "  --width=<pixels>          Surface width (default 1280)\n"
                      << "  --height=<pixels>         Surface height (default 720)\n"
                      << "  --frames=<count>          Stop after N presented frames\n"
                      << "  --report-interval=<sec>   Stats print interval (default 1.0)\n"
                      << "  --present-refresh=<hz>    Limit window presents to this rate (default 60, 0=every frame)\n"
                      << "  --report-metrics          Print FPS/render metrics every interval\n"
                      << "  --report-extended         Metrics plus Window::Present call timing\n"
                      << "  --present-call-metric     Track Window::Present duration (pairs well with --report-metrics)\n"
                      << "  --backend=<software|metal> Select renderer backend (default software)\n"
                      << "  --metal                   Shortcut for --backend=metal\n"
                      << "  --software                Shortcut for --backend=software\n"
                      << "  --runtime-minutes=<min>   Stop after the given number of minutes\n"
                      << "  --budget-present-ms=<ms>  Fail if avg present time exceeds this budget\n"
                      << "  --budget-render-ms=<ms>   Fail if avg render time exceeds this budget\n"
                      << "  --min-fps=<fps>           Fail if average FPS drops below this threshold\n"
                      << "  --write-baseline=<path>   Persist JSON baseline metrics to the given path\n"
                      << "  --write-frame=<png>       Capture the first presented frame to the given PNG path\n"
                      << "  --headless                Skip local window presentation\n"
                      << "  --windowed                Show the local window while computing frames (default)\n"
                      << "  --capture-framebuffer     Enable framebuffer capture in the present policy\n"
                      << "  --seed=<value>            PRNG seed\n";
            std::exit(0);
        } else {
            std::cerr << "pixel_noise_example: unknown option '" << arg << "'\n";
            std::cerr << "Use --help to see available options.\n";
            std::exit(1);
        }
    }

    if (opts.width <= 0 || opts.height <= 0) {
        std::cerr << "pixel_noise_example: width and height must be positive.\n";
        std::exit(1);
    }

    return opts;
}

auto make_identity_transform() -> UIScene::Transform {
    UIScene::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto build_background_bucket(int width, int height) -> UIScene::DrawableBucketSnapshot {
    UIScene::DrawableBucketSnapshot bucket{};
    constexpr std::uint64_t kDrawableId = 0xC0FFEE01u;

    bucket.drawable_ids = {kDrawableId};
    bucket.world_transforms = {make_identity_transform()};

    UIScene::BoundingSphere sphere{};
    sphere.center = {static_cast<float>(width) * 0.5f,
                     static_cast<float>(height) * 0.5f,
                     0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0]
                              + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres = {sphere};

    UIScene::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {static_cast<float>(width),
               static_cast<float>(height),
               0.0f};
    bucket.bounds_boxes = {box};
    bucket.bounds_box_valid = {1};

    bucket.layers = {0};
    bucket.z_values = {0.0f};
    bucket.material_ids = {0};
    bucket.pipeline_flags = {0};
    bucket.visibility = {1};
    bucket.command_offsets = {0};
    bucket.command_counts = {1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices.clear();
    bucket.layer_indices.clear();
    bucket.clip_nodes.clear();
    bucket.clip_head_indices = {-1};
    bucket.authoring_map = {
        UIScene::DrawableAuthoringMapEntry{
            kDrawableId,
            "pixel_noise/background",
            0,
            0,
        }
    };
    bucket.drawable_fingerprints = {kDrawableId};

    UIScene::RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = static_cast<float>(width);
    rect.max_y = static_cast<float>(height);
    rect.color = {0.0f, 0.0f, 0.0f, 1.0f};

    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset,
                &rect,
                sizeof(UIScene::RectCommand));
    bucket.command_kinds = {
        static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect),
    };

    return bucket;
}

struct SceneSetup {
    Builders::ScenePath scene;
    std::uint64_t revision = 0;
};

auto publish_scene(PathSpace& space,
                   SP::App::AppRootPathView root,
                   int width,
                   int height) -> SceneSetup {
    Builders::SceneParams scene_params{};
    scene_params.name = "pixel_noise_scene";
    scene_params.description = "Pixel noise perf harness scene";

    auto scene_path = expect_or_exit(Builders::Scene::Create(space, root, scene_params),
                                     "create scene");

    UIScene::SceneSnapshotBuilder builder{space, root, scene_path};
    auto bucket = build_background_bucket(width, height);

    UIScene::SnapshotPublishOptions publish{};
    publish.metadata.author = "pixel_noise_example";
    publish.metadata.tool_version = "pixel_noise_example";
    publish.metadata.created_at = std::chrono::system_clock::now();
    publish.metadata.drawable_count = bucket.drawable_ids.size();
    publish.metadata.command_count = bucket.command_counts.size();

    auto revision = expect_or_exit(builder.publish(publish, bucket),
                                   "publish scene snapshot");

    return SceneSetup{
        .scene = scene_path,
        .revision = revision,
    };
}

void present_to_local_window(Builders::Window::WindowPresentResult const& present,
                             int width,
                             int height,
                             bool headless) {
    if (headless) {
        return;
    }

    Builders::App::PresentToLocalWindowOptions options{};
    options.allow_framebuffer = false;
    options.warn_when_metal_texture_unshared = false;
    auto dispatched = Builders::App::PresentToLocalWindow(present,
                                                          width,
                                                          height,
                                                          options);
    if (!dispatched.presented && !present.stats.skipped) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::cerr << "pixel_noise_example: IOSurface unavailable; "
                         "skipping presentation to avoid CPU blit.\n";
        }
    }
}

struct HookGuard {
    HookGuard() = default;
    HookGuard(HookGuard const&) = delete;
    HookGuard& operator=(HookGuard const&) = delete;
    HookGuard(HookGuard&& other) noexcept
        : active(other.active) {
        other.active = false;
    }
    HookGuard& operator=(HookGuard&& other) noexcept {
        if (this != &other) {
            if (active) {
                Builders::Window::TestHooks::ResetBeforePresentHook();
            }
            active = other.active;
            other.active = false;
        }
        return *this;
    }
    ~HookGuard() {
        if (active) {
            Builders::Window::TestHooks::ResetBeforePresentHook();
        }
    }

private:
    bool active = true;
};

auto install_noise_hook(std::shared_ptr<NoiseState> state) -> HookGuard {
    Builders::Window::TestHooks::SetBeforePresentHook(
        [state = std::move(state)](PathSurfaceSoftware& surface,
                                   PathWindowView::PresentPolicy& /*policy*/,
                                   std::vector<std::size_t>& dirty_tiles) mutable {
            auto desc = surface.desc();
            auto width = std::max(0, desc.size_px.width);
            auto height = std::max(0, desc.size_px.height);
            if (width == 0 || height == 0) {
                return;
            }

            auto buffer = surface.staging_span();
            auto stride = surface.row_stride_bytes();
            if (buffer.size() < static_cast<std::size_t>(height) * stride
                || stride == 0) {
                return;
            }

            auto const start = std::chrono::steady_clock::now();
            auto worker_count = std::max<int>(1, static_cast<int>(std::thread::hardware_concurrency()));
            worker_count = std::min(worker_count, std::max(1, height));

            std::vector<std::uint64_t> seeds(static_cast<std::size_t>(worker_count));
            for (int i = 0; i < worker_count; ++i) {
                auto hi = static_cast<std::uint64_t>(state->rng());
                auto lo = static_cast<std::uint64_t>(state->rng());
                seeds[static_cast<std::size_t>(i)] = (hi << 32)
                                                     ^ lo
                                                     ^ (static_cast<std::uint64_t>(state->frame_index + 1) << 17)
                                                     ^ static_cast<std::uint64_t>(i);
            }

            auto rows_per_worker = (height + worker_count - 1) / worker_count;
            std::vector<std::thread> workers;
            workers.reserve(static_cast<std::size_t>(worker_count));

            for (int worker = 0; worker < worker_count; ++worker) {
                int row_begin = worker * rows_per_worker;
                int row_end = std::min(height, row_begin + rows_per_worker);
                if (row_begin >= row_end) {
                    break;
                }
                workers.emplace_back([row_begin,
                                      row_end,
                                      width,
                                      stride,
                                      buffer_data = buffer.data(),
                                      seed = seeds[static_cast<std::size_t>(worker)]]() {
                    std::uniform_int_distribution<int> dist(0, 255);
                    std::seed_seq seq{
                        static_cast<std::uint32_t>(seed & 0xFFFFFFFFu),
                        static_cast<std::uint32_t>((seed >> 32) & 0xFFFFFFFFu)};
                    std::mt19937 rng(seq);
                    for (int y = row_begin; y < row_end; ++y) {
                        auto* row = buffer_data + static_cast<std::size_t>(y) * stride;
                        for (int x = 0; x < width; ++x) {
                            auto channel0 = static_cast<std::uint32_t>(dist(rng));
                            auto channel1 = static_cast<std::uint32_t>(dist(rng));
                            auto channel2 = static_cast<std::uint32_t>(dist(rng));
                            std::uint32_t noise = channel0
                                                  | (channel1 << 8)
                                                  | (channel2 << 16)
                                                  | 0xFF000000u;
                            std::memcpy(row + static_cast<std::size_t>(x) * 4u, &noise, sizeof(noise));
                        }
                    }
                });
            }

            for (auto& worker : workers) {
                worker.join();
            }

            auto const finish = std::chrono::steady_clock::now();
            auto render_ms = std::chrono::duration<double, std::milli>(finish - start).count();

            ++state->frame_index;
            PathSurfaceSoftware::FrameInfo info{};
            info.frame_index = state->frame_index;
            info.revision = state->frame_index;
            info.render_ms = render_ms;
            surface.publish_buffered_frame(info);

            dirty_tiles.clear();
        });

    return HookGuard{};
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);

#if !defined(PATHSPACE_UI_METAL)
    if (options.use_metal_backend) {
        std::cerr << "pixel_noise_example: --backend=metal requested, but this build lacks PATHSPACE_UI_METAL support.\n";
        return 1;
    }
#else
    if (options.use_metal_backend && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        if (::setenv("PATHSPACE_ENABLE_METAL_UPLOADS", "1", 1) != 0) {
            std::cerr << "pixel_noise_example: warning: failed to set PATHSPACE_ENABLE_METAL_UPLOADS=1; Metal uploads may remain disabled.\n";
        }
    }
#endif

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/pixel_noise_example"};
    SP::App::AppRootPathView app_root_view{app_root.getPath()};

    auto scene_setup = publish_scene(space, app_root_view, options.width, options.height);

    Builders::App::BootstrapParams bootstrap_params{};
    bootstrap_params.renderer.name = options.use_metal_backend ? "noise_renderer_metal" : "noise_renderer";
    bootstrap_params.renderer.kind = options.use_metal_backend
                                         ? Builders::RendererKind::Metal2D
                                         : Builders::RendererKind::Software2D;
    bootstrap_params.renderer.description = options.use_metal_backend
                                                ? "pixel noise renderer (Metal2D)"
                                                : "pixel noise renderer";

    bootstrap_params.surface.name = "noise_surface";
    bootstrap_params.surface.desc.size_px.width = options.width;
    bootstrap_params.surface.desc.size_px.height = options.height;
    bootstrap_params.surface.desc.pixel_format = Builders::PixelFormat::RGBA8Unorm_sRGB;
    bootstrap_params.surface.desc.color_space = Builders::ColorSpace::sRGB;
    bootstrap_params.surface.desc.premultiplied_alpha = true;
#if defined(PATHSPACE_UI_METAL)
    if (options.use_metal_backend) {
        bootstrap_params.surface.desc.metal.storage_mode = Builders::MetalStorageMode::Shared;
        bootstrap_params.surface.desc.metal.texture_usage = static_cast<std::uint8_t>(Builders::MetalTextureUsage::ShaderRead)
                                                            | static_cast<std::uint8_t>(Builders::MetalTextureUsage::RenderTarget);
        bootstrap_params.surface.desc.metal.iosurface_backing = true;
    }
#endif

    bootstrap_params.window.name = "noise_window";
    bootstrap_params.window.title = "PathSpace Pixel Noise";
    bootstrap_params.window.width = options.width;
    bootstrap_params.window.height = options.height;
    bootstrap_params.window.scale = 1.0f;
    bootstrap_params.window.background = "#101218";

    bootstrap_params.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    bootstrap_params.present_policy.capture_framebuffer = options.capture_framebuffer;
    bootstrap_params.present_policy.auto_render_on_present = true;
    bootstrap_params.present_policy.vsync_align = false;

    auto bootstrap = expect_or_exit(Builders::App::Bootstrap(space,
                                                             app_root_view,
                                                             scene_setup.scene,
                                                             bootstrap_params),
                                    "bootstrap application");

    expect_or_exit(Builders::Surface::SetScene(space, bootstrap.surface, scene_setup.scene),
                   "bind scene to surface");

    auto noise_state = std::make_shared<NoiseState>(options.seed);
    auto hook_guard = install_noise_hook(noise_state);

    auto target_field = std::string(bootstrap.surface.getPath()) + "/target";
    auto target_relative = expect_or_exit(space.read<std::string, std::string>(target_field),
                                          "read surface target");
    auto target_absolute = expect_or_exit(SP::App::resolve_app_relative(app_root_view, target_relative),
                                          "resolve surface target");

    int current_surface_width = bootstrap.surface_desc.size_px.width;
    int current_surface_height = bootstrap.surface_desc.size_px.height;

    if (!options.headless) {
        SP::UI::SetLocalWindowCallbacks({});
        SP::UI::InitLocalWindowWithSize(options.width,
                                        options.height,
                                        "PathSpace Pixel Noise");
    }

    std::cout << "pixel_noise_example: width=" << options.width
              << " height=" << options.height
              << " seed=" << options.seed
              << " backend=" << (options.use_metal_backend ? "Metal2D" : "Software2D")
              << (options.headless ? " headless" : " windowed")
              << (options.capture_framebuffer ? " capture" : "")
              << (options.report_metrics ? " metrics" : "")
              << (options.report_present_call_time ? " present-call" : "")
              << '\n';

    auto now = std::chrono::steady_clock::now();
    auto last_report = now;
    auto start_time = now;
    auto present_interval = options.present_refresh_hz > 0.0
                                ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(1.0 / options.present_refresh_hz))
                                : std::chrono::steady_clock::duration::zero();
    auto last_window_present = start_time;
    std::size_t frames_since_report = 0;
    double accumulated_present_ms = 0.0;
    double accumulated_render_ms = 0.0;
    double interval_present_call_ms = 0.0;
    double total_present_call_ms = 0.0;
    std::size_t interval_present_call_samples = 0;
    std::size_t total_present_call_samples = 0;
    std::size_t total_presented = 0;
    double total_present_ms_sum = 0.0;
    double total_render_ms_sum = 0.0;
    long double total_tiles_updated = 0.0L;
    long double total_tiles_dirty = 0.0L;
    long double total_tiles_total = 0.0L;
    long double total_tiles_skipped = 0.0L;
    long double total_tiles_copied = 0.0L;
    long double total_bytes_copied = 0.0L;
    long double total_progressive_jobs = 0.0L;
    long double total_progressive_workers = 0.0L;
    long double total_encode_jobs = 0.0L;
    long double total_encode_workers = 0.0L;
    long double total_rects_coalesced = 0.0L;
    long double total_skip_seq_odd = 0.0L;
    long double total_recopy_after_seq_change = 0.0L;
    std::size_t progressive_present_frames = 0;
    std::uint64_t last_tile_size = 0;
    std::uint64_t last_tiles_total = 0;
    std::uint64_t last_drawable_count = 0;
    bool last_tile_diagnostics_enabled = false;
    std::string last_backend_kind;
    bool track_present_call_time = options.report_present_call_time || options.baseline_path.has_value();
    bool frame_written = false;

    while (g_running.load(std::memory_order_acquire)) {
        if (options.max_frames != 0 && total_presented >= options.max_frames) {
            break;
        }

        if (!options.headless) {
            SP::UI::PollLocalWindow();
            if (SP::UI::LocalWindowQuitRequested()) {
                std::cout << "pixel_noise_example: quit shortcut requested, exiting loop.\n";
                break;
            }
            int window_width = 0;
            int window_height = 0;
            SP::UI::GetLocalWindowContentSize(&window_width, &window_height);
            if (window_width <= 0 || window_height <= 0) {
                std::cout << "pixel_noise_example: window closed, exiting loop.\n";
                break;
            }

            if ((window_width != current_surface_width || window_height != current_surface_height)) {
                Builders::App::ResizeSurfaceOptions resize_options{};
                resize_options.submit_dirty_rect = false;
                expect_or_exit(Builders::App::UpdateSurfaceSize(space,
                                                                bootstrap,
                                                                window_width,
                                                                window_height,
                                                                resize_options),
                               "resize surface");
                current_surface_width = window_width;
                current_surface_height = window_height;
                options.width = window_width;
                options.height = window_height;
            }
        }

        if (options.runtime_limit) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= *options.runtime_limit) {
                std::cout << "pixel_noise_example: runtime limit reached ("
                          << std::chrono::duration_cast<std::chrono::seconds>(*options.runtime_limit).count()
                          << " seconds), exiting loop.\n";
                break;
            }
        }

        std::chrono::steady_clock::time_point present_call_start{};
        if (track_present_call_time) {
            present_call_start = std::chrono::steady_clock::now();
        }
        auto present = Builders::Window::Present(space,
                                                 bootstrap.window,
                                                 bootstrap.view_name);
        if (track_present_call_time) {
            auto present_call_finish = std::chrono::steady_clock::now();
            double call_ms = std::chrono::duration<double, std::milli>(present_call_finish - present_call_start).count();
            total_present_call_ms += call_ms;
            ++total_present_call_samples;
            if (options.report_metrics) {
                interval_present_call_ms += call_ms;
                ++interval_present_call_samples;
            }
        }
        if (!present) {
            auto const& err = present.error();
            std::cerr << "pixel_noise_example: present failed";
            if (err.message.has_value()) {
                std::cerr << ": " << *err.message;
            } else {
                std::cerr << " (code " << static_cast<int>(err.code) << ')';
            }
            std::cerr << '\n';
            break;
        }

        if (!options.headless) {
            auto current_time = std::chrono::steady_clock::now();
            bool should_present_window = options.present_refresh_hz <= 0.0
                || (present_interval.count() == 0)
                || ((current_time - last_window_present) >= present_interval);
            if (should_present_window) {
                present_to_local_window(*present,
                                        options.width,
                                        options.height,
                                        false);
                last_window_present = current_time;
            }
        }

        if (present->stats.presented) {
            ++total_presented;
            total_present_ms_sum += present->stats.present_ms;
            total_render_ms_sum += present->stats.frame.render_ms;
            total_tiles_updated += static_cast<long double>(present->stats.progressive_tiles_updated);
            total_tiles_dirty += static_cast<long double>(present->stats.progressive_tiles_dirty);
            total_tiles_total += static_cast<long double>(present->stats.progressive_tiles_total);
            total_tiles_skipped += static_cast<long double>(present->stats.progressive_tiles_skipped);
            total_tiles_copied += static_cast<long double>(present->stats.progressive_tiles_copied);
            total_bytes_copied += static_cast<long double>(present->stats.progressive_bytes_copied);
            total_progressive_jobs += static_cast<long double>(present->stats.progressive_jobs);
            total_progressive_workers += static_cast<long double>(present->stats.progressive_workers_used);
            total_encode_jobs += static_cast<long double>(present->stats.encode_jobs);
            total_encode_workers += static_cast<long double>(present->stats.encode_workers_used);
            total_rects_coalesced += static_cast<long double>(present->stats.progressive_rects_coalesced);
            total_skip_seq_odd += static_cast<long double>(present->stats.progressive_skip_seq_odd);
            total_recopy_after_seq_change += static_cast<long double>(present->stats.progressive_recopy_after_seq_change);
            if (present->stats.used_progressive) {
                ++progressive_present_frames;
            }
            last_tile_size = present->stats.progressive_tile_size;
            last_tiles_total = present->stats.progressive_tiles_total;
            last_drawable_count = present->stats.drawable_count;
            last_tile_diagnostics_enabled = present->stats.progressive_tile_diagnostics_enabled;
            if (!present->stats.backend_kind.empty()) {
                last_backend_kind = present->stats.backend_kind;
            }
            if (options.report_metrics) {
                ++frames_since_report;
                accumulated_present_ms += present->stats.present_ms;
                accumulated_render_ms += present->stats.frame.render_ms;
            }
        }

        if (options.frame_output_path && !frame_written && present->stats.presented) {
            auto framebuffer_capture = expect_or_exit(
                Builders::Diagnostics::ReadSoftwareFramebuffer(
                    space,
                    SP::ConcretePathStringView{target_absolute.getPath()}),
                "read software framebuffer");
            write_frame_capture_png_or_exit(framebuffer_capture, *options.frame_output_path);
            frame_written = true;
        }

        if (options.report_metrics) {
            now = std::chrono::steady_clock::now();
            if (now - last_report >= options.report_interval) {
                double seconds = std::chrono::duration<double>(now - last_report).count();
                double fps = seconds > 0.0 ? static_cast<double>(frames_since_report) / seconds : 0.0;
                double avg_present = frames_since_report > 0
                                         ? accumulated_present_ms / static_cast<double>(frames_since_report)
                                         : 0.0;
                double avg_render = frames_since_report > 0
                                        ? accumulated_render_ms / static_cast<double>(frames_since_report)
                                        : 0.0;

                std::cout << std::fixed << std::setprecision(2)
                          << "[pixel_noise_example] "
                          << "frames=" << total_presented
                          << " fps=" << fps
                          << " avgPresentMs=" << avg_present
                          << " avgRenderMs=" << avg_render;
                if (options.report_present_call_time && interval_present_call_samples > 0) {
                    double avg_call = interval_present_call_ms / static_cast<double>(interval_present_call_samples);
                    std::cout << " avgPresentCallMs=" << avg_call;
                }
                std::cout << " lastFrameIndex=" << present->stats.frame.frame_index
                          << " lastPresentMs=" << present->stats.present_ms
                          << " lastRenderMs=" << present->stats.frame.render_ms
                          << '\n';
                std::cout.unsetf(std::ios::floatfield);

                frames_since_report = 0;
                accumulated_present_ms = 0.0;
                accumulated_render_ms = 0.0;
                interval_present_call_ms = 0.0;
                interval_present_call_samples = 0;
                last_report = now;
            }
        }
    }

    std::cout << "pixel_noise_example: presented " << total_presented << " frames.\n";

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    double avg_present_ms = total_presented > 0
                                ? total_present_ms_sum / static_cast<double>(total_presented)
                                : 0.0;
    double avg_render_ms = total_presented > 0
                               ? total_render_ms_sum / static_cast<double>(total_presented)
                               : 0.0;
    double avg_fps = (elapsed_seconds > 0.0 && total_presented > 0)
                         ? static_cast<double>(total_presented) / elapsed_seconds
                         : 0.0;
    double avg_present_call_ms = (total_present_call_samples > 0)
                                     ? total_present_call_ms / static_cast<double>(total_present_call_samples)
                                     : 0.0;

    BaselineSummary baseline_summary{};
    baseline_summary.frames = total_presented;
    baseline_summary.elapsed_seconds = elapsed_seconds;
    baseline_summary.average_fps = avg_fps;
    baseline_summary.average_present_ms = avg_present_ms;
    baseline_summary.average_render_ms = avg_render_ms;
    baseline_summary.average_present_call_ms = avg_present_call_ms;
    baseline_summary.total_present_ms = total_present_ms_sum;
    baseline_summary.total_render_ms = total_render_ms_sum;

    TileSummary tile_summary{};
    tile_summary.frames = total_presented;
    tile_summary.progressive_frames = progressive_present_frames;
    tile_summary.last_tile_size = last_tile_size;
    tile_summary.last_tiles_total = last_tiles_total;
    tile_summary.last_drawable_count = last_drawable_count;
    tile_summary.last_tile_diagnostics_enabled = last_tile_diagnostics_enabled;
    if (total_presented > 0) {
        auto frames_ld = static_cast<long double>(total_presented);
        tile_summary.average_tiles_updated = static_cast<double>(total_tiles_updated / frames_ld);
        tile_summary.average_tiles_dirty = static_cast<double>(total_tiles_dirty / frames_ld);
        tile_summary.average_tiles_total = static_cast<double>(total_tiles_total / frames_ld);
        tile_summary.average_tiles_skipped = static_cast<double>(total_tiles_skipped / frames_ld);
        tile_summary.average_tiles_copied = static_cast<double>(total_tiles_copied / frames_ld);
        tile_summary.average_bytes_copied = static_cast<double>(total_bytes_copied / frames_ld);
        tile_summary.average_progressive_jobs = static_cast<double>(total_progressive_jobs / frames_ld);
        tile_summary.average_progressive_workers = static_cast<double>(total_progressive_workers / frames_ld);
        tile_summary.average_encode_jobs = static_cast<double>(total_encode_jobs / frames_ld);
        tile_summary.average_encode_workers = static_cast<double>(total_encode_workers / frames_ld);
        tile_summary.average_rects_coalesced = static_cast<double>(total_rects_coalesced / frames_ld);
        tile_summary.average_skip_seq_odd = static_cast<double>(total_skip_seq_odd / frames_ld);
        tile_summary.average_recopy_after_seq_change = static_cast<double>(total_recopy_after_seq_change / frames_ld);
    }

    std::cout << std::fixed << std::setprecision(3)
              << "pixel_noise_example: summary frames=" << total_presented
              << " fps=" << avg_fps
              << " avgPresentMs=" << avg_present_ms
              << " avgRenderMs=" << avg_render_ms;
    if (options.report_present_call_time && total_present_call_samples > 0) {
        std::cout << " avgPresentCallMs=" << avg_present_call_ms;
    }
    std::cout << '\n';
    std::cout.unsetf(std::ios::floatfield);

    if (options.report_present_call_time && total_present_call_samples > 0) {
        std::cout << std::fixed << std::setprecision(3)
                  << "pixel_noise_example: avgPresentCallMs="
                  << avg_present_call_ms
                  << " over " << total_present_call_samples << " samples\n";
        std::cout.unsetf(std::ios::floatfield);
    }

    bool budget_failed = false;
    if ((options.min_fps || options.budget_present_ms || options.budget_render_ms) && total_presented == 0) {
        std::cerr << "pixel_noise_example: no frames presented; unable to evaluate performance budgets.\n";
        budget_failed = true;
    }
    if (total_presented > 0) {
        if (options.min_fps && (avg_fps + 1e-6) < *options.min_fps) {
            std::cerr << "pixel_noise_example: average FPS " << avg_fps
                      << " below min-fps budget " << *options.min_fps << '\n';
            budget_failed = true;
        }
        if (options.budget_present_ms && (avg_present_ms - 1e-6) > *options.budget_present_ms) {
            std::cerr << "pixel_noise_example: avg present "
                      << avg_present_ms << "ms exceeds budget "
                      << *options.budget_present_ms << "ms\n";
            budget_failed = true;
        }
        if (options.budget_render_ms && (avg_render_ms - 1e-6) > *options.budget_render_ms) {
            std::cerr << "pixel_noise_example: avg render "
                      << avg_render_ms << "ms exceeds budget "
                      << *options.budget_render_ms << "ms\n";
            budget_failed = true;
        }
    }

    if (options.baseline_path) {
        if (budget_failed) {
            std::cerr << "pixel_noise_example: skipping baseline write because budgets failed\n";
        } else {
            auto metrics = expect_or_exit(
                Builders::Diagnostics::ReadTargetMetrics(space,
                                                         SP::ConcretePathStringView{target_absolute.getPath()}),
                "read target metrics");
            std::string backend = !last_backend_kind.empty() ? last_backend_kind : metrics.backend_kind;
            if (backend.empty()) {
                backend = options.use_metal_backend ? "Metal2D" : "Software2D";
            }
            tile_summary.last_tile_size = last_tile_size != 0 ? last_tile_size : metrics.progressive_tile_size;
            tile_summary.last_tiles_total = last_tiles_total != 0 ? last_tiles_total : metrics.progressive_tiles_total;
            tile_summary.last_drawable_count = last_drawable_count != 0 ? last_drawable_count : metrics.drawable_count;
            tile_summary.last_tile_diagnostics_enabled = last_tile_diagnostics_enabled || metrics.progressive_tile_diagnostics_enabled;
            write_baseline_metrics(options,
                                   baseline_summary,
                                   tile_summary,
                                   metrics,
                                   backend,
                                   std::filesystem::path{*options.baseline_path});
            std::cout << "pixel_noise_example: baseline metrics written to "
                      << *options.baseline_path << '\n';
        }
    }

    return budget_failed ? 2 : 0;
}

#endif // PATHSPACE_ENABLE_UI / __APPLE__
