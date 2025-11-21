#include "inspector/PaintScreenshotCard.hpp"

#include "PathSpace.hpp"
#include "core/Error.hpp"

#include "nlohmann/json.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

namespace SP::Inspector {
namespace detail {

template <typename T>
auto path_join(std::string const& base, std::string_view leaf) -> std::string {
    if (base.empty()) {
        return std::string{leaf};
    }
    std::string path = base;
    if (path.back() != '/') {
        path.push_back('/');
    }
    path.append(leaf.begin(), leaf.end());
    return path;
}

template <typename T>
auto readOptional(PathSpace& space, std::string const& path) -> Expected<std::optional<T>> {
    auto value = space.template read<T, std::string>(path);
    if (!value) {
        if (value.error().code == Error::Code::NoObjectFound) {
            return std::optional<T>{};
        }
        return std::unexpected(value.error());
    }
    return std::optional<T>{std::move(*value)};
}

auto appendSummary(std::string& existing, std::string_view addition) {
    if (!existing.empty()) {
        existing.append("; ");
    }
    existing.append(addition.begin(), addition.end());
}

auto severityToString(PaintScreenshotSeverity severity) -> std::string_view {
    switch (severity) {
    case PaintScreenshotSeverity::MissingData:
        return "missing";
    case PaintScreenshotSeverity::WaitingForCapture:
        return "waiting";
    case PaintScreenshotSeverity::Healthy:
        return "healthy";
    case PaintScreenshotSeverity::Attention:
        return "attention";
    }
    return "missing";
}

auto classifySeverity(PaintScreenshotManifest const& manifest,
                      std::optional<PaintScreenshotRun> const& last_run) -> PaintScreenshotSeverity {
    if (!manifest.revision && !manifest.tag && !last_run) {
        return PaintScreenshotSeverity::MissingData;
    }
    if (!last_run || !last_run->status) {
        return PaintScreenshotSeverity::WaitingForCapture;
    }
    auto const status = *last_run->status;
    auto const success = (status == "match" || status == "captured");
    if (!success) {
        return PaintScreenshotSeverity::Attention;
    }
    if (manifest.tolerance && last_run->mean_error) {
        if (*last_run->mean_error > *manifest.tolerance + std::numeric_limits<double>::epsilon()) {
            return PaintScreenshotSeverity::Attention;
        }
    }
    return PaintScreenshotSeverity::Healthy;
}

auto makeError(Error::Code code, std::string_view message) -> Error {
    return Error{code, std::string{message}};
}

template <typename T>
auto write_optional(nlohmann::json& object, char const* key, std::optional<T> const& value) -> void {
    if (value) {
        object[key] = *value;
    } else {
        object[key] = nullptr;
    }
}

auto runToJson(PaintScreenshotRun const& run) -> nlohmann::json {
    nlohmann::json json;
    write_optional(json, "timestamp_ns", run.timestamp_ns);
    write_optional(json, "timestamp_iso", run.timestamp_iso);
    write_optional(json, "status", run.status);
    write_optional(json, "hardware_capture", run.hardware_capture);
    write_optional(json, "require_present", run.require_present);
    write_optional(json, "mean_error", run.mean_error);
    write_optional(json, "max_channel_delta", run.max_channel_delta);
    write_optional(json, "screenshot_path", run.screenshot_path);
    write_optional(json, "diff_path", run.diff_path);
    write_optional(json, "tag", run.tag);
    write_optional(json, "manifest_revision", run.manifest_revision);
    write_optional(json, "renderer", run.renderer);
    write_optional(json, "width", run.width);
    write_optional(json, "height", run.height);
    write_optional(json, "sha256", run.sha256);
    json["ok"] = run.ok;
    return json;
}

auto manifestToJson(PaintScreenshotManifest const& manifest) -> nlohmann::json {
    nlohmann::json json;
    write_optional(json, "revision", manifest.revision);
    write_optional(json, "tag", manifest.tag);
    write_optional(json, "sha256", manifest.sha256);
    write_optional(json, "width", manifest.width);
    write_optional(json, "height", manifest.height);
    write_optional(json, "renderer", manifest.renderer);
    write_optional(json, "captured_at", manifest.captured_at);
    write_optional(json, "commit", manifest.commit);
    write_optional(json, "notes", manifest.notes);
    write_optional(json, "tolerance", manifest.tolerance);
    return json;
}

template <typename T>
auto json_optional(nlohmann::json const& object, std::string_view key) -> std::optional<T> {
    auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    try {
        return it->get<T>();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace detail

auto BuildPaintScreenshotCard(PathSpace& space,
                              PaintScreenshotCardOptions const& options)
    -> Expected<PaintScreenshotCard> {
    PaintScreenshotCard card{};

    auto const manifest_root = options.diagnostics_root;
    auto const last_run_root = detail::path_join<std::string>(options.diagnostics_root, "last_run");

    auto revision = detail::readOptional<std::int64_t>(space, detail::path_join<std::string>(manifest_root, "manifest_revision"));
    if (!revision) {
        return std::unexpected(revision.error());
    }
    card.manifest.revision = *revision;

    auto tag = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "tag"));
    if (!tag) {
        return std::unexpected(tag.error());
    }
    card.manifest.tag = *tag;

    auto sha = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "sha256"));
    if (!sha) {
        return std::unexpected(sha.error());
    }
    card.manifest.sha256 = *sha;

    auto width = detail::readOptional<std::int64_t>(space, detail::path_join<std::string>(manifest_root, "width"));
    if (!width) {
        return std::unexpected(width.error());
    }
    if (width->has_value()) {
        card.manifest.width = static_cast<int>(**width);
    }

    auto height = detail::readOptional<std::int64_t>(space, detail::path_join<std::string>(manifest_root, "height"));
    if (!height) {
        return std::unexpected(height.error());
    }
    if (height->has_value()) {
        card.manifest.height = static_cast<int>(**height);
    }

    auto renderer = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "renderer"));
    if (!renderer) {
        return std::unexpected(renderer.error());
    }
    card.manifest.renderer = *renderer;

    auto captured_at = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "captured_at"));
    if (!captured_at) {
        return std::unexpected(captured_at.error());
    }
    card.manifest.captured_at = *captured_at;

    auto commit = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "commit"));
    if (!commit) {
        return std::unexpected(commit.error());
    }
    card.manifest.commit = *commit;

    auto notes = detail::readOptional<std::string>(space, detail::path_join<std::string>(manifest_root, "notes"));
    if (!notes) {
        return std::unexpected(notes.error());
    }
    card.manifest.notes = *notes;

    auto tolerance = detail::readOptional<double>(space, detail::path_join<std::string>(manifest_root, "tolerance"));
    if (!tolerance) {
        return std::unexpected(tolerance.error());
    }
    card.manifest.tolerance = *tolerance;

    PaintScreenshotRun run{};

    auto ts = detail::readOptional<std::int64_t>(space, detail::path_join<std::string>(last_run_root, "timestamp_ns"));
    if (!ts) {
        return std::unexpected(ts.error());
    }
    run.timestamp_ns = *ts;

    auto status = detail::readOptional<std::string>(space, detail::path_join<std::string>(last_run_root, "status"));
    if (!status) {
        return std::unexpected(status.error());
    }
    run.status = *status;

    auto hw = detail::readOptional<bool>(space, detail::path_join<std::string>(last_run_root, "hardware_capture"));
    if (!hw) {
        return std::unexpected(hw.error());
    }
    run.hardware_capture = *hw;

    auto require_present = detail::readOptional<bool>(space, detail::path_join<std::string>(last_run_root, "require_present"));
    if (!require_present) {
        return std::unexpected(require_present.error());
    }
    run.require_present = *require_present;

    auto mean_err = detail::readOptional<double>(space, detail::path_join<std::string>(last_run_root, "mean_error"));
    if (!mean_err) {
        return std::unexpected(mean_err.error());
    }
    run.mean_error = *mean_err;

    auto max_delta = detail::readOptional<std::int64_t>(space, detail::path_join<std::string>(last_run_root, "max_channel_delta"));
    if (!max_delta) {
        return std::unexpected(max_delta.error());
    }
    if (max_delta->has_value()) {
        run.max_channel_delta = static_cast<std::uint32_t>(**max_delta);
    }

    auto screenshot_path = detail::readOptional<std::string>(space, detail::path_join<std::string>(last_run_root, "screenshot_path"));
    if (!screenshot_path) {
        return std::unexpected(screenshot_path.error());
    }
    run.screenshot_path = *screenshot_path;

    auto diff_path = detail::readOptional<std::string>(space, detail::path_join<std::string>(last_run_root, "diff_path"));
    if (!diff_path) {
        return std::unexpected(diff_path.error());
    }
    run.diff_path = *diff_path;

    if (run.status) {
        run.ok = (*run.status == "match" || *run.status == "captured");
    }
    run.manifest_revision = card.manifest.revision;
    run.tag               = card.manifest.tag;
    run.renderer          = card.manifest.renderer;
    run.width             = card.manifest.width;
    run.height            = card.manifest.height;
    run.sha256            = card.manifest.sha256;
    card.last_run         = run.timestamp_ns ? std::optional<PaintScreenshotRun>{run} : std::nullopt;

    if (options.fallback_json) {
        auto runs = LoadPaintScreenshotRunsFromJson(*options.fallback_json, options.max_runs);
        if (!runs) {
            return std::unexpected(runs.error());
        }
        card.recent_runs = std::move(*runs);
    }

    card.severity = detail::classifySeverity(card.manifest, card.last_run);

    switch (card.severity) {
    case PaintScreenshotSeverity::MissingData:
        card.summary = "No baseline manifest present";
        break;
    case PaintScreenshotSeverity::WaitingForCapture:
        card.summary = "Baseline recorded; waiting for screenshot run";
        break;
    case PaintScreenshotSeverity::Healthy:
        card.summary = "Screenshot matches baseline";
        break;
    case PaintScreenshotSeverity::Attention:
        card.summary = "Screenshot drift detected";
        break;
    }

    if (card.last_run && card.last_run->mean_error) {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(4);
        stream << "mean error=" << *card.last_run->mean_error;
        detail::appendSummary(card.summary, stream.str());
    }

    if (card.manifest.tolerance) {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(4);
        stream << "tolerance=" << *card.manifest.tolerance;
        detail::appendSummary(card.summary, stream.str());
    }

    return card;
}

auto LoadPaintScreenshotRunsFromJson(std::filesystem::path const& path,
                                     std::size_t max_runs)
    -> Expected<std::vector<PaintScreenshotRun>> {
    std::ifstream stream(path);
    if (!stream) {
        return std::unexpected(detail::makeError(Error::Code::NotFound, "failed to open diagnostics json"));
    }
    std::string buffer((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (buffer.empty()) {
        return std::unexpected(detail::makeError(Error::Code::MalformedInput, "empty diagnostics json"));
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(buffer);
    } catch (std::exception const& ex) {
        return std::unexpected(detail::makeError(Error::Code::MalformedInput, ex.what()));
    }

    if (!doc.contains("runs") || !doc["runs"].is_array()) {
        return std::unexpected(detail::makeError(Error::Code::MalformedInput, "diagnostics json missing runs array"));
    }

    std::vector<PaintScreenshotRun> runs;
    runs.reserve(doc["runs"].size());
    for (auto const& entry : doc["runs"]) {
        PaintScreenshotRun run{};
        run.timestamp_ns      = detail::json_optional<std::int64_t>(entry, "timestamp_ns");
        run.timestamp_iso     = detail::json_optional<std::string>(entry, "timestamp_iso");
        run.status            = detail::json_optional<std::string>(entry, "status");
        run.hardware_capture  = detail::json_optional<bool>(entry, "hardware_capture");
        run.require_present   = detail::json_optional<bool>(entry, "require_present");
        run.mean_error        = detail::json_optional<double>(entry, "mean_error");
        if (auto max_delta = detail::json_optional<std::int64_t>(entry, "max_channel_delta")) {
            run.max_channel_delta = static_cast<std::uint32_t>(*max_delta);
        } else if (auto max_delta_u32 = detail::json_optional<std::uint32_t>(entry, "max_channel_delta")) {
            run.max_channel_delta = *max_delta_u32;
        }
        run.screenshot_path   = detail::json_optional<std::string>(entry, "screenshot_path");
        run.diff_path         = detail::json_optional<std::string>(entry, "diff_path");
        run.tag               = detail::json_optional<std::string>(entry, "tag");
        run.manifest_revision = detail::json_optional<std::int64_t>(entry, "manifest_revision");
        run.renderer          = detail::json_optional<std::string>(entry, "renderer");
        if (auto width = detail::json_optional<int>(entry, "width")) {
            run.width = *width;
        }
        if (auto height = detail::json_optional<int>(entry, "height")) {
            run.height = *height;
        }
        run.sha256 = detail::json_optional<std::string>(entry, "sha256");
        if (entry.contains("ok") && !entry["ok"].is_null()) {
            run.ok = entry["ok"].get<bool>();
        }
        runs.push_back(std::move(run));
    }
    if (runs.size() > max_runs) {
        runs.resize(max_runs);
    }
    return runs;
}

auto BuildPaintScreenshotCardFromRuns(std::vector<PaintScreenshotRun> runs,
                                      PaintScreenshotCardOptions const& options)
    -> PaintScreenshotCard {
    PaintScreenshotCard card{};
    card.recent_runs = std::move(runs);
    if (!card.recent_runs.empty()) {
        card.last_run = card.recent_runs.front();
        if (card.last_run->manifest_revision) {
            card.manifest.revision = card.last_run->manifest_revision;
        }
        card.manifest.tag      = card.last_run->tag;
        card.manifest.renderer = card.last_run->renderer;
        card.manifest.width    = card.last_run->width;
        card.manifest.height   = card.last_run->height;
        card.manifest.sha256   = card.last_run->sha256;
    }
    card.severity = detail::classifySeverity(card.manifest, card.last_run);
    switch (card.severity) {
    case PaintScreenshotSeverity::MissingData:
        card.summary = "No runs recorded";
        break;
    case PaintScreenshotSeverity::WaitingForCapture:
        card.summary = "Awaiting screenshot run";
        break;
    case PaintScreenshotSeverity::Healthy:
        card.summary = "Screenshot matches baseline";
        break;
    case PaintScreenshotSeverity::Attention:
        card.summary = "Screenshot drift detected";
        break;
    }
    return card;
}

auto SerializePaintScreenshotCard(PaintScreenshotCard const& card, int indent) -> std::string {
    nlohmann::json json;
    json["severity"] = detail::severityToString(card.severity);
    json["summary"]  = card.summary;
    json["manifest"] = detail::manifestToJson(card.manifest);
    if (card.last_run) {
        json["last_run"] = detail::runToJson(*card.last_run);
    } else {
        json["last_run"] = nullptr;
    }
    nlohmann::json runs = nlohmann::json::array();
    for (auto const& run : card.recent_runs) {
        runs.push_back(detail::runToJson(run));
    }
    json["recent_runs"] = std::move(runs);
    // Treat indent <= 0 as "compact" output (no whitespace) so helpers like
    // pathspace_paint_screenshot_card and the inspector SSE stream can embed
    // the JSON without post-processing. Tests rely on indent==0 producing the
    // minimized form.
    if (indent <= 0) {
        return json.dump();
    }
    return json.dump(indent);
}

} // namespace SP::Inspector
