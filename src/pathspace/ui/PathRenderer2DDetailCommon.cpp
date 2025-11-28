#include "PathRenderer2DDetail.hpp"

#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace SP::UI::PathRenderer2DDetail {
namespace {

auto to_lower_ascii(std::string_view value) -> std::string {
    std::string lowered{value};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

auto parse_bool(std::string_view value) -> std::optional<bool> {
    auto lowered = to_lower_ascii(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return std::nullopt;
}

auto parse_text_pipeline(std::string_view value) -> std::optional<PathRenderer2D::TextPipeline> {
    auto lowered = to_lower_ascii(value);
    if (lowered == "glyph" || lowered == "glyphs" || lowered == "glyph-quads" || lowered == "glyph_quads") {
        return PathRenderer2D::TextPipeline::GlyphQuads;
    }
    if (lowered == "shaped" || lowered == "shaped-text" || lowered == "shaped_text") {
        return PathRenderer2D::TextPipeline::Shaped;
    }
    return std::nullopt;
}

auto env_text_pipeline() -> std::optional<PathRenderer2D::TextPipeline> {
    if (auto* env = std::getenv("PATHSPACE_TEXT_PIPELINE")) {
        return parse_text_pipeline(env);
    }
    return std::nullopt;
}

auto env_disable_text_fallback() -> std::optional<bool> {
    if (auto* env = std::getenv("PATHSPACE_DISABLE_TEXT_FALLBACK")) {
        return parse_bool(env);
    }
    return std::nullopt;
}

constexpr double kPi = 3.14159265358979323846;

auto has_valid_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                          std::uint32_t index) -> bool {
    if (index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (index < bucket.bounds_box_valid.size()) {
        return bucket.bounds_box_valid[index] != 0;
    }
    return true;
}

} // namespace

auto make_error(std::string message, SP::Error::Code code) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

auto damage_metrics_enabled() -> bool {
    if (auto* env = std::getenv("PATHSPACE_UI_DAMAGE_METRICS")) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0 || std::strcmp(env, "off") == 0) {
            return false;
        }
        return true;
    }
    return false;
}

auto determine_text_pipeline(SP::UI::Runtime::RenderSettings const& settings)
    -> std::pair<PathRenderer2D::TextPipeline, bool> {
    auto pipeline = PathRenderer2D::TextPipeline::GlyphQuads;
    bool allow_fallback = true;

    if (auto env_pipeline = env_text_pipeline()) {
        pipeline = *env_pipeline;
    }
    if (auto env_disable = env_disable_text_fallback()) {
        allow_fallback = !(*env_disable);
    }

    if (settings.debug.enabled) {
        if ((settings.debug.flags & SP::UI::Runtime::RenderSettings::Debug::kForceShapedText) != 0) {
            pipeline = PathRenderer2D::TextPipeline::Shaped;
        }
        if ((settings.debug.flags & SP::UI::Runtime::RenderSettings::Debug::kDisableTextFallback) != 0) {
            allow_fallback = false;
        }
    }

    return {pipeline, allow_fallback};
}

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto fingerprint_to_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

auto to_byte(float value) -> std::uint8_t {
    auto clamped = clamp_unit(value);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

auto set_last_error(PathSpace& space,
                    SP::ConcretePathStringView targetPath,
                    std::string const& message,
                    std::uint64_t revision,
                    Runtime::Diagnostics::PathSpaceError::Severity severity,
                    int code) -> SP::Expected<void> {
    if (message.empty()) {
        return Runtime::Diagnostics::ClearTargetError(space, targetPath);
    }

    Runtime::Diagnostics::PathSpaceError error{};
    error.code = code;
    error.severity = severity;
    error.message = message;
    error.path = std::string(targetPath.getPath());
    error.revision = revision;
    return Runtime::Diagnostics::WriteTargetError(space, targetPath, error);
}

auto pipeline_flags_for(Scene::DrawableBucketSnapshot const& bucket,
                        std::size_t drawable_index) -> std::uint32_t {
    if (drawable_index < bucket.pipeline_flags.size()) {
        return bucket.pipeline_flags[drawable_index];
    }
    return 0;
}

auto approximate_drawable_area(Scene::DrawableBucketSnapshot const& bucket,
                               std::uint32_t index) -> double {
    if (has_valid_bounds_box(bucket, index)) {
        auto const& box = bucket.bounds_boxes[index];
        auto width = std::max(0.0f, box.max[0] - box.min[0]);
        auto height = std::max(0.0f, box.max[1] - box.min[1]);
        return static_cast<double>(width) * static_cast<double>(height);
    }
    if (index < bucket.bounds_spheres.size()) {
        auto radius = bucket.bounds_spheres[index].radius;
        if (radius > 0.0f) {
            return static_cast<double>(radius) * static_cast<double>(radius) * kPi;
        }
    }
    return 0.0;
}

auto compute_drawable_bounds(Scene::DrawableBucketSnapshot const& bucket,
                             std::uint32_t index,
                             int width,
                             int height) -> std::optional<PathRenderer2D::DrawableBounds> {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool have_bounds = false;

    if (has_valid_bounds_box(bucket, index)) {
        auto const& box = bucket.bounds_boxes[index];
        min_x = box.min[0];
        min_y = box.min[1];
        max_x = box.max[0];
        max_y = box.max[1];
        have_bounds = true;
    } else if (index < bucket.bounds_spheres.size()) {
        auto const& sphere = bucket.bounds_spheres[index];
        auto radius = sphere.radius;
        min_x = sphere.center[0] - radius;
        max_x = sphere.center[0] + radius;
        min_y = sphere.center[1] - radius;
        max_y = sphere.center[1] + radius;
        have_bounds = true;
    }

    if (!have_bounds) {
        return std::nullopt;
    }

    auto to_min_x = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto to_max_x = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto to_min_y = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto to_max_y = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    PathRenderer2D::DrawableBounds bounds{
        .min_x = to_min_x,
        .min_y = to_min_y,
        .max_x = to_max_x,
        .max_y = to_max_y,
    };

    if (bounds.empty()) {
        return std::nullopt;
    }

    bounds.min_x = std::max(0, bounds.min_x - 1);
    bounds.min_y = std::max(0, bounds.min_y - 1);
    bounds.max_x = std::min(width, bounds.max_x + 1);
    bounds.max_y = std::min(height, bounds.max_y + 1);

    if (bounds.empty()) {
        return std::nullopt;
    }

    return bounds;
}

auto bounds_equal(PathRenderer2D::DrawableBounds const& lhs,
                  PathRenderer2D::DrawableBounds const& rhs) -> bool {
    return lhs.min_x == rhs.min_x
        && lhs.min_y == rhs.min_y
        && lhs.max_x == rhs.max_x
        && lhs.max_y == rhs.max_y;
}

auto pulse_focus_highlight_color(std::array<float, 4> const& srgb,
                                 double time_ms) -> std::array<float, 4> {
    constexpr double kPeriodMs = 1000.0;
    double phase = std::fmod(time_ms, kPeriodMs);
    if (phase < 0.0) {
        phase += kPeriodMs;
    }
    double normalized = (kPeriodMs == 0.0) ? 0.0 : phase / kPeriodMs;
    double wave = std::sin(normalized * 2.0 * kPi);
    float intensity = static_cast<float>(std::abs(wave));
    float mix = std::min(intensity * 0.18f, 1.0f);
    std::array<float, 4> result = srgb;
    std::array<float, 4> target{
        0.0f,
        0.0f,
        0.0f,
        srgb[3],
    };
    if (wave >= 0.0) {
        target = {1.0f, 1.0f, 1.0f, srgb[3]};
    }
    for (int i = 0; i < 3; ++i) {
        result[i] = clamp_unit(srgb[i] * (1.0f - mix) + target[i] * mix);
    }
    result[3] = srgb[3];
    return result;
}

auto schedule_focus_pulse_render(PathSpace& space,
                                 SP::ConcretePathStringView targetPath,
                                 SP::UI::Runtime::RenderSettings const& settings,
                                 std::optional<SP::UI::Runtime::DirtyRectHint> focus_hint,
                                 std::uint64_t frame_index) -> void {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_schedule;
    static std::atomic<std::uint64_t> sequence{0};
    auto now = std::chrono::steady_clock::now();
    constexpr auto kMinimumInterval = std::chrono::milliseconds{96};

    std::string target{targetPath.getPath()};
    if (target.empty()) {
        return;
    }

    bool should_schedule = false;
    {
        std::lock_guard<std::mutex> lock{mutex};
        auto& previous = last_schedule[target];
        if (previous.time_since_epoch().count() == 0
            || now - previous >= kMinimumInterval) {
            previous = now;
            should_schedule = true;
        }
    }
    if (!should_schedule) {
        return;
    }

    // Enqueue an auto-render request so the pulse keeps animating.
    Runtime::AutoRenderRequestEvent event{
        .sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        .reason = "focus-pulse",
        .frame_index = frame_index,
    };
    auto queuePath = target + "/events/renderRequested/queue";
    auto inserted = space.insert(queuePath, event);
    (void)inserted;

    SP::UI::Runtime::DirtyRectHint hint{};
    if (focus_hint.has_value()) {
        hint = *focus_hint;
    } else {
        float width = static_cast<float>(std::max(settings.surface.size_px.width, 0));
        float height = static_cast<float>(std::max(settings.surface.size_px.height, 0));
        if (width <= 0.0f || height <= 0.0f) {
            return;
        }
        hint.min_x = 0.0f;
        hint.min_y = 0.0f;
        hint.max_x = width;
        hint.max_y = height;
    }

    if (hint.max_x <= hint.min_x || hint.max_y <= hint.min_y) {
        return;
    }

    std::array<SP::UI::Runtime::DirtyRectHint, 1> hints{hint};
    (void)Runtime::Renderer::SubmitDirtyRects(space,
                                               targetPath,
                                               std::span<const SP::UI::Runtime::DirtyRectHint>(hints.data(), hints.size()));
}

#if defined(__APPLE__) && PATHSPACE_UI_METAL
auto metal_supports_command(Scene::DrawCommandKind kind) -> bool {
    switch (kind) {
    case Scene::DrawCommandKind::Rect:
    case Scene::DrawCommandKind::RoundedRect:
    case Scene::DrawCommandKind::Image:
    case Scene::DrawCommandKind::TextGlyphs:
        return true;
    default:
        return false;
    }
}
#endif

auto compute_command_payload_offsets(std::vector<std::uint32_t> const& kinds,
                                     std::vector<std::uint8_t> const& payload)
    -> SP::Expected<std::vector<std::size_t>> {
    std::vector<std::size_t> offsets;
    offsets.reserve(kinds.size());
    std::size_t cursor = 0;
    for (auto kind_value : kinds) {
        auto kind = static_cast<Scene::DrawCommandKind>(kind_value);
        auto payload_size = Scene::payload_size_bytes(kind);
        if (payload_size == 0) {
            offsets.push_back(cursor);
            continue;
        }
        if (cursor + payload_size > payload.size()) {
            return std::unexpected(make_error("command payload truncated",
                                              SP::Error::Code::InvalidType));
        }
        offsets.push_back(cursor);
        cursor += payload_size;
    }
    if (cursor != payload.size()) {
        return std::unexpected(make_error("command payload size mismatch",
                                          SP::Error::Code::InvalidType));
    }
    return offsets;
}

} // namespace SP::UI::PathRenderer2DDetail
