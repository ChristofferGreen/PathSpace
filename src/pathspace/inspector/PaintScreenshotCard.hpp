#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP {

class PathSpace;

namespace Inspector {

struct PaintScreenshotManifest {
    std::optional<std::int64_t> revision;
    std::optional<std::string>   tag;
    std::optional<std::string>   sha256;
    std::optional<int>           width;
    std::optional<int>           height;
    std::optional<std::string>   renderer;
    std::optional<std::string>   captured_at;
    std::optional<std::string>   commit;
    std::optional<std::string>   notes;
    std::optional<double>        tolerance;
};

struct PaintScreenshotRun {
    std::optional<std::int64_t>  timestamp_ns;
    std::optional<std::string>   timestamp_iso;
    std::optional<std::string>   status;
    std::optional<bool>          hardware_capture;
    std::optional<bool>          require_present;
    std::optional<double>        mean_error;
    std::optional<std::uint32_t> max_channel_delta;
    std::optional<std::string>   screenshot_path;
    std::optional<std::string>   diff_path;
    std::optional<std::string>   tag;
    std::optional<std::int64_t>  manifest_revision;
    std::optional<std::string>   renderer;
    std::optional<int>           width;
    std::optional<int>           height;
    std::optional<std::string>   sha256;
    bool                         ok = false;
};

enum class PaintScreenshotSeverity {
    MissingData = 0,
    WaitingForCapture,
    Healthy,
    Attention,
};

struct PaintScreenshotCard {
    PaintScreenshotManifest          manifest;
    std::optional<PaintScreenshotRun> last_run;
    std::vector<PaintScreenshotRun>   recent_runs;
    PaintScreenshotSeverity           severity = PaintScreenshotSeverity::MissingData;
    std::string                       summary;
};

struct PaintScreenshotCardOptions {
    std::string diagnostics_root = "/diagnostics/ui/paint_example/screenshot_baseline";
    std::optional<std::filesystem::path> fallback_json;
    std::size_t max_runs = 10;
};

auto BuildPaintScreenshotCard(PathSpace& space,
                              PaintScreenshotCardOptions const& options = {})
    -> Expected<PaintScreenshotCard>;

auto LoadPaintScreenshotRunsFromJson(std::filesystem::path const& path,
                                     std::size_t max_runs)
    -> Expected<std::vector<PaintScreenshotRun>>;

auto BuildPaintScreenshotCardFromRuns(std::vector<PaintScreenshotRun> runs,
                                      PaintScreenshotCardOptions const& options = {})
    -> PaintScreenshotCard;

auto SerializePaintScreenshotCard(PaintScreenshotCard const& card, int indent = 2) -> std::string;

} // namespace Inspector
} // namespace SP
